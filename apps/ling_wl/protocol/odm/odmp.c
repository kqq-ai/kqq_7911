#include "odmp.h"

#include <string.h>
#include <math.h>
#include "cpu.h"
#include "cJSON_common/cJSON.h"
#include "util/cjson_util.h"
#include "sys_time.h"
#include "system/timer.h"
#include "system/sys_time.h"
#include "driver/device/ioctl_cmds.h"
#include "driver/device/device.h"
#include "generic/jiffies.h"
#include "app_mode/lwl_eend_device.h"
#include "app_mode/lwl_app_core.h"

enum ParseErrorCode {
    PARSE_Success,
    PARSE_Error,
};

#define LING_UUID_BUFSIZE 37

// message_text 类型2的文本缓冲区
#define MESSAGE_TEXT_BUFFER_SIZE 512  // 缓冲区大小
#define MESSAGE_TEXT_SEND_ID_SIZE 16   // send_id 缓冲区大小
static char g_message_text_buffer[MESSAGE_TEXT_BUFFER_SIZE];
static int g_message_text_buffer_len = 0;  // 当前缓冲区已使用长度
static char g_message_text_send_id[MESSAGE_TEXT_SEND_ID_SIZE];  // 保存 send_id
static u16 g_message_text_timeout_id = 0;  // 超时定时器ID

g_odmp_cbk_t g_odmp_cbk = NULL;
g_odmp_audio_data_cbk_t g_odmp_audio_data_cbk = NULL;
g_odmp_disconnect_cbk_t g_odmp_disconnect_cbk = NULL;

static const char *odmp_et_str[] = {
    [OET_PING]                  = "ping",
    [OET_REQ_MESSAGE]           = "req_message",
    [OET_AUDIO_STATUS]          = "audio_status",
    [OET_SET_AUDIO_FORMAT]      = "set_audio_format",
    [OET_NEARBY_UPDATE]         = "nearby_update",
    [OET_AUDIO_STREAM_START]    = "audio_stream_start",
};

static const char *odmp_ast_str[] = {
    [OAST_ASR]                  = "ASR",
    [OAST_TTS]                  = "TTS",
    [OAST_STREAM_AUDIO]         = "STREAM_AUDIO",
    [OAST_SCRIPT]               = "SCR",
};

static const char *odmp_ase_str[] = {
    [OASE_START]                = "start",
    [OASE_END]                  = "end",
};

static const char *odmp_rmet_str[] = {
    [ORMET_INITIAL_SETUP]       = "initial_setup",
    [ORMET_NEAR_TRIGGER]        = "near_trigger",
    [ORMET_SWITCH_SUCCESS]      = "switch_success",
    [ORMET_BUTTON_TRIGGER]      = "button_trigger",
    [ORMET_PLAY_STOP]           = "play_stop",
    [ORMET_PLAY_END]            = "play_end",
    [ORMET_PLAY_NEXT]       = "play_next",
};

inline const char *odmp_et_to_text(int event_type) {
    return odmp_et_str[event_type];
}

inline const char *odmp_ast_to_text(int audio_status_type) {
    return odmp_ast_str[audio_status_type];
}

inline const char *odmp_ase_to_text(int audio_status_event) {
    return odmp_ase_str[audio_status_event];
}

inline const char *odmp_rmet_to_text(int req_message_event_type) {
    return odmp_rmet_str[req_message_event_type];
}

/**
 * @name    generate_uuid
 * @brief   生成UUID字符串
 * @details
 *      时间戳部分占UUID的前8个字符
 *      随机数部分占后面12个字符
 *      UUID格式为xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx，其中x是随机十六进制数字，y是特定范围的随机十六进制数字
 * 
 * @param[out]  uuid_str    buf大小至少为37字节，返回字符串\0结尾
 */
static void generate_uuid(char *uuid_str) {
    uint8_t random_buf[12];
    uint32_t timestamp = timer_get_ms();

    memset(uuid_str, 0xFF, LING_UUID_BUFSIZE);

    // 获取时间戳部分
    snprintf(uuid_str, 9, "%08x", (unsigned long)timestamp);

    // 获取随机数部分
    get_random_number(random_buf, sizeof(random_buf));

    // printf_buf(random_buf, sizeof(random_buf));

    random_buf[2] = (random_buf[2] & 0x0F) | 0x40; // 版本4
    random_buf[4] = (random_buf[4] & 0x3F) | 0x80; // 变体1 (RFC 4122)

    // 格式化UUID字符串
    snprintf(uuid_str + 8, 6, "-%04x", ((unsigned int)(random_buf[0]) << 8) | random_buf[1]);
    snprintf(uuid_str + 13, 6, "-%04x", ((unsigned int)(random_buf[2]) << 8) | random_buf[3]);
    snprintf(uuid_str + 18, 6, "-%04x", ((unsigned int)(random_buf[4]) << 8) | random_buf[5]);
    snprintf(uuid_str + 23, 14, "-%04x%04x%04X", ((unsigned int)(random_buf[6]) << 8)| random_buf[7],
             ((unsigned int)(random_buf[8]) << 8) | random_buf[9],
             ((unsigned int)(random_buf[10]) << 8) | random_buf[11]);

    uuid_str[36] = '\0';

    // printf_buf(uuid_str, LING_UUID_BUFSIZE);
}

cJSON *odmp_message_base(int type) 
{
    char uuid[LING_UUID_BUFSIZE];
    generate_uuid(uuid);

    cJSON *nroot = cJSON_CreateObject();
	cJSON_AddStringToObject(nroot,"uuid", uuid);
	cJSON_AddStringToObject(nroot,"event", odmp_et_str[type]);
    return nroot;
}

char *odmp_send_ping()
{
    /*
    {
        "uuid": "xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx",
        "event": "ping"
    }
    */
    cJSON *nroot = odmp_message_base(OET_PING);
	char *json_str = cJSON_PrintUnformatted(nroot);

    if (NULL != nroot) {
        cJSON_Delete(nroot);
    }

    return json_str;
}

char *odmp_req_message(const char **targets, int targets_num, const char *c_id, const char *send_id, bool is_play, int event_type)
{
    /*
    {
        "uuid": "xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx",
        "event": "req_message"
        "data": {
            "targets": [
                "test_小熊",
                "test_乐乐"
            ],
            "c_id": "luka",
            "send_id": "test_小熊",
            "is_play": false,
            "event": 1
        }
    }
    */
    cJSON *nroot = odmp_message_base(OET_REQ_MESSAGE);
    cJSON *nroot_data = cJSON_CreateObject();
    cJSON_AddItemToObject(nroot, "data", nroot_data);
    
    // targets
    cJSON *aroot_data_targets = cJSON_CreateArray();
    if (targets != NULL && targets_num > 0) {
        for (int i = 0; i < targets_num; ++i) {
            cJSON_AddItemToArray(aroot_data_targets, cJSON_CreateString(targets[i]));
        }
    } 
    cJSON_AddItemToObject(nroot_data, "targets", aroot_data_targets);
    
    cJSON_AddStringToObject(nroot_data, "c_id", c_id);
    cJSON_AddStringToObject(nroot_data, "send_id", send_id ? send_id : "");
    cJSON_AddBoolToObject(nroot_data, "is_play", is_play);
    cJSON_AddNumberToObject(nroot_data, "event", event_type);

	char *json_str = cJSON_PrintUnformatted(nroot);

    if (NULL != nroot) {
        cJSON_Delete(nroot);
    }
    return json_str;
}

char *odmp_audio_status(const char *c_id, int type, int event)
{
    /*
    {
        "uuid": "xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx",
        "event": "audio_status"
        "data": {
            "c_id": "CC1CC1CC1CC1",
            "type": 1,
            "Event ": 1，
            "audio_params": {
                "format": "opus",
                "sample_rate": 16000,
            }
        }
    }
    */
    cJSON *nroot = odmp_message_base(OET_AUDIO_STATUS);
    cJSON *nroot_data = cJSON_CreateObject();
    cJSON_AddItemToObject(nroot, "data", nroot_data);

	cJSON_AddStringToObject(nroot_data, "c_id", c_id);
    cJSON_AddNumberToObject(nroot_data, "type", type);
    cJSON_AddNumberToObject(nroot_data, "event", event);

    if (type == OAST_ASR) {
        cJSON *naudio_params = cJSON_CreateObject();
        cJSON_AddStringToObject(naudio_params, "format", "mp3");
        cJSON_AddNumberToObject(naudio_params, "sample_rate", 16000);
        cJSON_AddItemToObject(nroot_data, "audio_params", naudio_params);
    }

	char *json_str = cJSON_PrintUnformatted(nroot);

    if (NULL != nroot) {
        cJSON_Delete(nroot);
    }

    return json_str;
}

char *odmp_set_audio_format(const char *c_id, int type)
{
    /*
    {
        "uuid":"98f3501d-4b20-43dd-92bd-58f92a9f6a7f",
        "event":"set_audio_format",
        "data":{
            "c_id":"ZYCNC5E21XEEEF0",
            "audio_params":{
                "format":"opus",
                "sample_rate":16000
                }
            },
        "storage":{}
    }
    */
    cJSON *nroot = odmp_message_base(OET_SET_AUDIO_FORMAT);
    cJSON *nroot_data = cJSON_CreateObject();
    cJSON_AddItemToObject(nroot, "data", nroot_data);

	cJSON_AddStringToObject(nroot_data, "c_id", c_id);
    if (type == OAST_TTS) {
        cJSON *naudio_params = cJSON_CreateObject();
        cJSON_AddStringToObject(naudio_params, "format", "mp3");///opus
        cJSON_AddNumberToObject(naudio_params, "sample_rate", 8000);
        cJSON_AddItemToObject(nroot_data, "audio_params", naudio_params);
    }

	char *json_str = cJSON_PrintUnformatted(nroot);

    if (NULL != nroot) {
        cJSON_Delete(nroot);
    }

    return json_str;
}

char *odmp_device_status(int status)
{
    /*
{
"event":"device_status",
"data":{
    "status":0
    }
}
    */
    char *send_data = (char *)malloc(1024);
    if (send_data == NULL) {
        return NULL;
    }
    
    snprintf(send_data, 1024, "{ \"event\": \"device_status\", \"data\": { \"status\": %d } }", 
                 status);

    return send_data;
}

/**
 * @brief 生成audio_stream_start消息
 * @return JSON字符串,需要调用者释放
 * @note 消息格式:
 * {
 *   "event": "audio_stream_start",
 *   "data": {
 *     "stream_id": "resp_20251225141650001",
 *     "stream_type": "uplink",
 *     "format": "mp3",
 *     "sample_rate": 16000,
 *     "channels": 1,
 *     "play_mode": "",
 *     "s_vad": true
 *   }
 * }
 */
char *odmp_audio_stream_start(const char *stream_id, const char *stream_type)
{
    if (stream_id == NULL || stream_type == NULL) {
        return NULL;
    }

    // 创建JSON对象
    cJSON *nroot = cJSON_CreateObject();
    cJSON_AddStringToObject(nroot, "event", "audio_stream_start");
    
    cJSON *nroot_data = cJSON_CreateObject();
    cJSON_AddItemToObject(nroot, "data", nroot_data);
    
    // 添加data字段
    cJSON_AddStringToObject(nroot_data, "stream_id", stream_id);
    cJSON_AddStringToObject(nroot_data, "stream_type", stream_type);
    cJSON_AddStringToObject(nroot_data, "format", "mp3");
    cJSON_AddNumberToObject(nroot_data, "sample_rate", 16000);
    cJSON_AddNumberToObject(nroot_data, "channels", 1);
    cJSON_AddStringToObject(nroot_data, "play_mode", "");
    cJSON_AddBoolToObject(nroot_data, "s_vad", 1);
    
    char *json_str = cJSON_PrintUnformatted(nroot);
    
    if (NULL != nroot) {
        cJSON_Delete(nroot);
    }
    
    return json_str;
}

/**
 * @brief 生成audio_stream_end消息
 * @param stream_id 流ID，必须与audio_stream_start发送时的stream_id相同
 * @param status 状态，例如 "user_interrupted"
 * @return JSON字符串,需要调用者释放
 * @note 消息格式:
 * {
 *   "event": "audio_stream_end",
 *   "data": {
 *     "stream_id": "resp_20251225141650001",
 *     "status": "user_interrupted",
 *     "stream_type": "uplink"
 *   }
 * }
 */
char *odmp_audio_stream_end(const char *stream_id, const char *status)
{
    if (stream_id == NULL || status == NULL) {
        return NULL;
    }
    
    // 创建JSON对象
    cJSON *nroot = cJSON_CreateObject();
    cJSON_AddStringToObject(nroot, "event", "audio_stream_end");
    
    cJSON *nroot_data = cJSON_CreateObject();
    cJSON_AddItemToObject(nroot, "data", nroot_data);
    
    // 添加data字段
    cJSON_AddStringToObject(nroot_data, "stream_id", stream_id);
    cJSON_AddStringToObject(nroot_data, "status", status);
    cJSON_AddStringToObject(nroot_data, "stream_type", "uplink");
    
    char *json_str = cJSON_PrintUnformatted(nroot);
    
    if (NULL != nroot) {
        cJSON_Delete(nroot);
    }
    
    return json_str;
}

/**
 * @brief 生成bind_result消息
 * @param sn E端设备序列号
 * @param success 是否成功
 * @param status 状态，例如 "connected"
 * @param stream_id 流ID
 * @return JSON字符串,需要调用者释放
 * @note 消息格式:
 * {
 *   "event": "bind_result",
 *   "data": {
 *     "sn": "E9K2512003147",
 *     "success": true,
 *     "status": "connected",
 *     "stream_id": "xxx"
 *   }
 * }
 */
char *odmp_bind_result(const char *sn, bool success, const char *status, const char *stream_id)
{
    if (sn == NULL || status == NULL || stream_id == NULL) {
        return NULL;
    }
    
    // 创建JSON对象
    cJSON *nroot = cJSON_CreateObject();
    cJSON_AddStringToObject(nroot, "event", "bind_result");
    
    cJSON *nroot_data = cJSON_CreateObject();
    cJSON_AddItemToObject(nroot, "data", nroot_data);
    
    // 添加data字段
    cJSON_AddStringToObject(nroot_data, "sn", sn);
    cJSON_AddBoolToObject(nroot_data, "success", success ? 1 : 0);
    cJSON_AddStringToObject(nroot_data, "status", status);
    cJSON_AddStringToObject(nroot_data, "stream_id", stream_id);
    
    char *json_str = cJSON_PrintUnformatted(nroot);
    
    if (NULL != nroot) {
        cJSON_Delete(nroot);
    }
    
    return json_str;
}

/**
 * @brief 生成audio_play_started消息
 * @param stream_id 流ID
 * @param status 状态，例如 "normal_start"
 * @param play_mode 播放模式，例如 "normal"
 * @return JSON字符串,需要调用者释放
 * @note 消息格式:
 * {
 *   "event": "audio_play_started",
 *   "data": {
 *     "stream_id": "s:asstart_20251225221345022",
 *     "status": "normal_start",
 *     "play_mode": "normal"
 *   }
 * }
 */
char *odmp_audio_play_started(const char *stream_id, const char *status, const char *play_mode)
{
    if (stream_id == NULL || status == NULL || play_mode == NULL) {
        return NULL;
    }
    
    // 创建JSON对象
    cJSON *nroot = cJSON_CreateObject();
    cJSON_AddStringToObject(nroot, "event", "audio_play_started");
    
    cJSON *nroot_data = cJSON_CreateObject();
    cJSON_AddItemToObject(nroot, "data", nroot_data);
    
    // 添加data字段
    cJSON_AddStringToObject(nroot_data, "stream_id", stream_id);
    cJSON_AddStringToObject(nroot_data, "status", status);
    cJSON_AddStringToObject(nroot_data, "play_mode", play_mode);
    
    char *json_str = cJSON_PrintUnformatted(nroot);
    
    if (NULL != nroot) {
        cJSON_Delete(nroot);
    }
    
    return json_str;
}

/**
 * @brief 生成audio_play_finished消息
 * @param stream_id 流ID，必须与audio_play_started发送时的stream_id相同
 * @param status 状态，例如 "playback_complete"
 * @return JSON字符串,需要调用者释放
 * @note 消息格式:
 * {
 *   "event": "audio_play_finished",
 *   "data": {
 *     "stream_id": "s:asstart_20251225221345022",
 *     "status": "playback_complete"
 *   }
 * }
 */
char *odmp_audio_play_finished(const char *stream_id, const char *status)
{
    if (stream_id == NULL || status == NULL) {
        return NULL;
    }
    
    // 创建JSON对象
    cJSON *nroot = cJSON_CreateObject();
    cJSON_AddStringToObject(nroot, "event", "audio_play_finished");
    
    cJSON *nroot_data = cJSON_CreateObject();
    cJSON_AddItemToObject(nroot, "data", nroot_data);
    
    // 添加data字段
    cJSON_AddStringToObject(nroot_data, "stream_id", stream_id);
    cJSON_AddStringToObject(nroot_data, "status", status);
    
    char *json_str = cJSON_PrintUnformatted(nroot);
    
    if (NULL != nroot) {
        cJSON_Delete(nroot);
    }
    
    return json_str;
}

/**
 * @brief 生成nearby_update消息
 * @param devices 设备指针数组
 * @param device_count 设备数量
 * @param c_id C端设备ID
 * @return JSON字符串,需要调用者释放
 */
char *odmp_nearby_update(void **devices, int device_count, const char *c_id)
{
    /*
    nearby_update
    {
      "msg_id": "c:WL-C8K2512000891:nbu_20251225165803472",
      "event": "nearby_update",
      "data": {
        "scan_time": "2025-12-25T16:58:02.819Z",
        "devices": [
          {
            "sn": "E9K2512003147",
            "rssi": -49,
            "distance_cm": 45,
            "battery": 88,
          }
        ]
      }
    }
    */
    
    // 生成msg_id: c:{c_id}:nbu_{timestamp}
    char msg_id[128];
    u32 timestamp_ms = timer_get_ms();
    snprintf(msg_id, sizeof(msg_id), "c:%s:nbu_%u", c_id, timestamp_ms);
    
    // 获取ISO8601格式的扫描时间
    char scan_time[32];
    lwl_rtc_get_iso8601_time(scan_time, sizeof(scan_time));
    
    // 创建JSON对象
    cJSON *nroot = cJSON_CreateObject();
    cJSON_AddStringToObject(nroot, "msg_id", msg_id);
    cJSON_AddStringToObject(nroot, "event", "nearby_update");
    
    cJSON *nroot_data = cJSON_CreateObject();
    cJSON_AddItemToObject(nroot, "data", nroot_data);
    
    cJSON_AddStringToObject(nroot_data, "scan_time", scan_time);
    
    // 创建devices数组
    cJSON *devices_array = cJSON_CreateArray();
    cJSON_AddItemToObject(nroot_data, "devices", devices_array);
    
    // 遍历所有设备,添加到数组中
    for (int i = 0; i < device_count; i++) {
        lwl_eend_dev_t *dev = (lwl_eend_dev_t *)devices[i];
        if (dev == NULL) {
            continue;
        }
        
        // 只添加有RSSI样本的设备
        if (dev->distance_info.rssi_sample_count <= 0) {
            continue;
        }
        
        cJSON *device_obj = cJSON_CreateObject();
        
        // sn: 设备序列号
        cJSON_AddStringToObject(device_obj, "sn", (char *)dev->sn);
        
        // rssi: 最新RSSI值(取rssi_samples数组最后一个)
        int rssi_idx = dev->distance_info.rssi_sample_count - 1;
        s8 rssi = dev->distance_info.rssi_samples[rssi_idx];
        cJSON_AddNumberToObject(device_obj, "rssi", rssi);
        
        // distance_cm: 距离(米转厘米,四舍五入)
        int distance_cm = (int)round(dev->distance_info.distance * 100.0);
        cJSON_AddNumberToObject(device_obj, "distance_cm", distance_cm);
        
        // battery: 电量百分比(bat_level * 100 / 15)
        int battery = (dev->bat_level * 100) / 15;
        cJSON_AddNumberToObject(device_obj, "battery", battery);
        
        cJSON_AddItemToArray(devices_array, device_obj);
    }
    
    char *json_str = cJSON_PrintUnformatted(nroot);
    
    if (NULL != nroot) {
        cJSON_Delete(nroot);
    }
    
    return json_str;
}

static int tts_bind_event_handler(const cJSON *nroot, odmp_package_t *pkg)
{
    const cJSON *ndata = cjson_get_object(nroot, "data");
    if (ndata == NULL) {
        return PARSE_Error;
    }
    char *v_sn = cjson_get_string_value(ndata, "sn");
    if (v_sn == NULL) {
        return PARSE_Error;
    }
    pkg->tts_bind.sn = v_sn;
    char *v_stream_id = cjson_get_string_value(ndata, "stream_id");
    if (v_stream_id == NULL) {
        return PARSE_Error;
    }
    pkg->tts_bind.stream_id = v_stream_id;
    if (cjson_get_integer_value(ndata, "role_volume", &pkg->tts_bind.role_volume)) {
        return PARSE_Error;
    }
    return PARSE_Success;
}

static int message_text_event_handler(const cJSON *nroot, odmp_package_t *pkg)
{
    const cJSON *ndata = cjson_get_object(nroot, "data");
    if (ndata == NULL) {
        return PARSE_Error;
    }
    char *v_c_id = cjson_get_string_value(ndata, "c_id");
    if (v_c_id == NULL) {
        return PARSE_Error;
    }
    pkg->message_text.c_id = v_c_id;
    char *vsend_id = cjson_get_string_value(ndata, "send_id");
    if (vsend_id == NULL) {
        return PARSE_Error;
    }
    pkg->message_text.send_id = vsend_id;
    char *vtext = cjson_get_string_value(ndata, "text");
    if (vtext == NULL) {
        return PARSE_Error;
    }
    pkg->message_text.text = vtext;
    if (cjson_get_integer_value(ndata, "type", &pkg->message_text.type)) {
        return PARSE_Error;
    }
    return PARSE_Success;
}
static int pong_event_handler(const cJSON *nroot, odmp_package_t *pkg)
{
    return PARSE_Success;
}

static int audio_status_event_handler(const cJSON *nroot, odmp_package_t *pkg)
{
    return PARSE_Success;
}

static int initial_setup_response_event_handler(const cJSON *nroot, odmp_package_t *pkg)
{
    return PARSE_Success;
}

static int device_switch_response_event_handler(const cJSON *nroot, odmp_package_t *pkg)
{
    return PARSE_Success;
}

static int error_event_handler(const cJSON *nroot, odmp_package_t *pkg)
{
    return PARSE_Success;
}

static int execution_time_event_handler(const cJSON *nroot, odmp_package_t *pkg)
{
    return PARSE_Success;
}

static int audio_stream_start_event_handler(const cJSON *nroot, odmp_package_t *pkg)
{
    const cJSON *ndata = cjson_get_object(nroot, "data");
    if (ndata == NULL) {
        return PARSE_Error;
    }
    char *stream_type = cjson_get_string_value(ndata, "stream_type");
    if (stream_type == NULL) {
        return PARSE_Error;
    }
    // 只处理 downlink 类型的流
    if (strcmp(stream_type, "downlink") == 0) {
        char *stream_id = cjson_get_string_value(ndata, "stream_id");
        log_i("🌲🌲 【gy】服务端开始下放音频: stream_id=%s, stream_type=%s", 
              stream_id ? stream_id : "NULL", stream_type);
    }
    return PARSE_Success;
}

static int audio_stream_end_event_handler(const cJSON *nroot, odmp_package_t *pkg)
{
    const cJSON *ndata = cjson_get_object(nroot, "data");
    if (ndata == NULL) {
        return PARSE_Error;
    }
    char *stream_type = cjson_get_string_value(ndata, "stream_type");
    if (stream_type == NULL) {
        return PARSE_Error;
    }
    // 只处理 downlink 类型的流
    if (strcmp(stream_type, "downlink") == 0) {
        char *stream_id = cjson_get_string_value(ndata, "stream_id");
        char *status = cjson_get_string_value(ndata, "status");
        log_i("🌲🌲 【gy】服务端结束下放音频: stream_id=%s, stream_type=%s, status=%s", 
              stream_id ? stream_id : "NULL", stream_type, status ? status : "NULL");
    }
    return PARSE_Success;
}

// message_text type=2 的超时回调函数
static void message_text_timeout_callback(void *priv)
{
    if (g_message_text_buffer_len > 0) {
        // 打印缓冲区内容，需要打印send_id
        log_mqtt("💬💬💬 [MESSAGE_TEXT] 【%s】回复： %s", g_message_text_send_id, g_message_text_buffer);
        // 清空缓冲区
        g_message_text_buffer_len = 0;
        g_message_text_buffer[0] = '\0';
        g_message_text_send_id[0] = '\0';  // 清空 send_id
    }
    g_message_text_timeout_id = 0;  // 清除定时器ID
}

void mqtt_print_pkg_content(const cJSON *nroot, const char *event)
{
    const cJSON *ndata = cjson_get_object(nroot, "data");
    if (ndata == NULL) {
        return;
    }
    if (strcmp(event, "audio_status") == 0) {
        int type = 0;
        int event = 0;
        cjson_get_integer_value(ndata, "type", &type);
        cjson_get_integer_value(ndata, "event", &event);
        log_mqtt("🚹️⬅️⬅️⬅️ [AUDIO_STATUS] type: %d, event: %d", type, event);
    }
    else if (strcmp(event, "tts_bind") == 0) {
        char *sn = cjson_get_string_value(ndata, "sn");
        char *stream_id = cjson_get_string_value(ndata, "stream_id");
        int role_volume = 0;
        cjson_get_integer_value(ndata, "role_volume", &role_volume);
        if (sn != NULL) {
            log_mqtt("🚹️⬅️⬅️⬅️ [TTS_BIND] sn: %s, stream_id: %s, role_volume: %d", sn, stream_id ? stream_id : "NULL", role_volume);
        }
    }
    else if (strcmp(event, "message_text") == 0) {
        int type = 0;
        cjson_get_integer_value(ndata, "type", &type);
        char *text = cjson_get_string_value(ndata, "text");
        
        if (type == 1) {
            // type=1: 直接打印"用户说"
            log_mqtt("🗿🗿🗿 [MESSAGE_TEXT] 用户说：%s", text);
        } else if (type == 2) {
            // type=2: 累加文本到缓冲区
            if (text != NULL) {
                // 如果是第一条 type=2 消息，保存 send_id
                if (g_message_text_buffer_len == 0) {
                    char *send_id = cjson_get_string_value(ndata, "send_id");
                    if (send_id != NULL) {
                        int send_id_len = strlen(send_id);
                        int copy_len = (send_id_len < MESSAGE_TEXT_SEND_ID_SIZE - 1) ? send_id_len : (MESSAGE_TEXT_SEND_ID_SIZE - 1);
                        memcpy(g_message_text_send_id, send_id, copy_len);
                        g_message_text_send_id[copy_len] = '\0';
                    } else {
                        g_message_text_send_id[0] = '\0';
                    }
                }
                
                int text_len = strlen(text);
                int remaining = MESSAGE_TEXT_BUFFER_SIZE - g_message_text_buffer_len - 1;
                
                if (remaining > 0) {
                    // 如果缓冲区为空，这是第一条 type=2 消息，启动定时器
                    // 如果已有定时器，删除并重新启动（重置超时）
                    if (g_message_text_timeout_id != 0) {
                        sys_timeout_del(g_message_text_timeout_id);
                    }
                    // 启动新的1秒超时定时器
                    g_message_text_timeout_id = sys_timeout_add(NULL, message_text_timeout_callback, 1000);
                    
                    // 累加文本（追加到缓冲区）
                    int copy_len = (text_len < remaining) ? text_len : remaining;
                    memcpy(g_message_text_buffer + g_message_text_buffer_len, text, copy_len);
                    g_message_text_buffer_len += copy_len;
                    g_message_text_buffer[g_message_text_buffer_len] = '\0';
                }
            }
        }
    }
}

void odmp_register_rsp_cbk(g_odmp_cbk_t odmp_cbk)
{
    g_odmp_cbk = odmp_cbk;
}

void odmp_register_audio_data_cbk(g_odmp_audio_data_cbk_t odmp_audio_data_cbk)
{
    g_odmp_audio_data_cbk = odmp_audio_data_cbk;
}

void odmp_register_disconnect_cbk(g_odmp_disconnect_cbk_t odmp_disconnect_cbk)
{
    g_odmp_disconnect_cbk = odmp_disconnect_cbk;
}

struct odmp_rsp_cb_entry_t {
    const char* event_name;
    int type;
    int (*handler)(const cJSON *nroot, odmp_package_t *pkg);
    void (*release)(odmp_package_t *pkg);
};

static struct odmp_rsp_cb_entry_t g_rsp_cb_table[] = {
    {"audio_status", OR_AUDIO_STATUS, audio_status_event_handler, NULL},
    {"tts_bind", OR_TTS_BIND, tts_bind_event_handler, NULL},
    {"initial_setup_response", OR_INITIAL_SETUP_RESPONSE, initial_setup_response_event_handler, NULL}, 
    {"device_switch_response", OR_DEVICE_SWITCH_RESPONSE, device_switch_response_event_handler, NULL}, 
    {"event_error", OR_ERROR_EVENT, error_event_handler, NULL}, 
    {"message_text", OR_MESSAGE_TEXT, message_text_event_handler, NULL}, 
    {"execution_time", OR_EXECUTION_TIME, execution_time_event_handler, NULL},
    {"pong", OR_PONG, pong_event_handler, NULL},
    {"audio_stream_start", OR_AUDIO_STREAM_START, audio_stream_start_event_handler, NULL},
    {"audio_stream_end", OR_AUDIO_STREAM_END, audio_stream_end_event_handler, NULL},
};
static int g_rsp_cb_table_size = sizeof(g_rsp_cb_table) / sizeof(g_rsp_cb_table[0]);

void odmp_rsp_handler(u8 *buf, u32 len)
{
    odmp_package_t pkg;
    memset(&pkg, 0, sizeof(odmp_package_t));

    const cJSON *nroot = cjson_parse(buf);
    if (nroot == NULL) {
        goto error_ret;
    }
    // pkg.errmsg = cjson_get_string_value(nroot, "errmsg");
    pkg.event  = cjson_get_string_value(nroot, "event");
    pkg.uuid   = cjson_get_string_value(nroot, "uuid");
    // pkg.debug  = cjson_get_string_value(nroot, "debug");
    if (pkg.event == NULL) {
        goto error_ret;
    }
    mqtt_print_pkg_content(nroot, pkg.event);
    struct odmp_rsp_cb_entry_t *entry = NULL;
    for (int i = 0; i < g_rsp_cb_table_size; ++i) {
        if (strcmp(pkg.event, g_rsp_cb_table[i].event_name) == 0) {
            entry = &g_rsp_cb_table[i];
            break;
        }
    }

    if (entry == NULL) {
        log_i("Error: can't find event handler: %s", pkg.event);
        goto error_ret;
    }

    if (pkg.errno == 0) {
        /*  Successful */
        if (entry->handler != NULL) {
            int ret = entry->handler(nroot, &pkg);
            log_i("【dg】[odmp_rsp_handler] event %s, handler ret=%d, g_odmp_cbk=%p", 
                  pkg.event, ret, g_odmp_cbk);
            if (ret == PARSE_Success) {
                if (g_odmp_cbk != NULL) {
                    log_i("odmp_rsp_handler: event %s, type %d", pkg.event, entry->type);
                    g_odmp_cbk(entry->type, &pkg);
                } else {
                    log_e("【dg】[odmp_rsp_handler] event %s g_odmp_cbk is NULL, cannot process event", pkg.event);
                }
            } else {
                log_e("【dg】[odmp_rsp_handler] event %s handler returned PARSE_Error", pkg.event);
            }
        }
        if (entry->release != NULL) {
            entry->release(&pkg);
        }
    } else {
        /*  Error */
        log_i("【dg】[odmp_rsp_handler] event %s, errno=%d, g_odmp_cbk=%p", 
              pkg.event, pkg.errno, g_odmp_cbk);
        if (g_odmp_cbk != NULL) {
            g_odmp_cbk(entry->type, &pkg);
        } else {
            log_e("【dg】[odmp_rsp_handler] event %s g_odmp_cbk is NULL, cannot process error event", pkg.event);
        }
    }

error_ret:
    if (nroot != NULL) {
        cJSON_Delete(nroot);
        nroot = NULL;
    }
    return;
}

void odmp_audio_data_handler(u8 *buf, u32 len) 
{
    if (g_odmp_audio_data_cbk) {
        g_odmp_audio_data_cbk(buf, len);
    }
}

void odmp_disconnect_handler(void)
{
    if (g_odmp_disconnect_cbk) {
        g_odmp_disconnect_cbk();
    }
}
