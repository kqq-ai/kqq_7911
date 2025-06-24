#include "odmp.h"

#include <string.h>
#include "cpu.h"
#include "cJSON_common/cJSON.h"
#include "util/cjson_util.h"
#include "sys_time.h"

enum ParseErrorCode {
    PARSE_Success,
    PARSE_Error,
};

#define LING_UUID_BUFSIZE 37

g_odmp_cbk_t g_odmp_cbk = NULL;
g_odmp_audio_data_cbk_t g_odmp_audio_data_cbk = NULL;
g_odmp_disconnect_cbk_t g_odmp_disconnect_cbk = NULL;

static const char *odmp_et_str[] = {
    [OET_REQ_MESSAGE]           = "req_message",
    [OET_AUDIO_STATUS]          = "audio_status",
};

static const char *odmp_ast_str[] = {
    [OAST_ASR]                  = "ASR",
    [OAST_TTS]                  = "TTS",
};

static const char *odmp_ase_str[] = {
    [OASE_START]                = "start",
    [OASE_END]                  = "end",
    [OASE_ABNORMAL]             = "abnormal",
    [OASE_SENTENCE_START]       = "sentence_start",
    [OASE_SENTENCE_END]         = "sentence_end",
};

static const char *odmp_rmet_str[] = {
    [ORMET_INITIAL_SETUP]       = "initial_setup",
    [ORMET_NEAR_TRIGGER]        = "near_trigger",
    [ORMET_NEW_DEVICE_IN]       = "new_device_in",
    [ORMET_DEVICE_LEAVE]        = "device_leave",
    [ORMET_SWITCH_SUCCESS]      = "switch_success",
    [ORMET_SWITCH_FAILED]       = "switch_failed",
    [ORMET_BUTTON_TRIGGER]      = "button_trigger",
    [ORMET_BUTTON_PRESS]        = "button_press",
    [ORMET_BUTTON_DOUBLE_PRESS] = "button_double_press",
    [ORMET_BUTTON_START]        = "button_start",
    [ORMET_BUTTON_END]          = "button_end",
    [ORMET_MOVE_PATROL]         = "move_patrol",
    [ORMET_MOVE_STOP]           = "move_stop",
    [ORMET_MOVE_SHAKE]          = "move_shake",
    [ORMET_VAD_SPEAKER_START]   = "vad_speaker_start",
    [ORMET_VAD_SPEAKER_END]     = "vad_speaker_end",
    [ORMET_LOWER_POWER]         = "lower_power",
    [ORMET_LOWER_BATTERY]       = "lower_battery",
    [ORMET_PLAY_START]          = "play_start",
    [ORMET_PLAY_NEAR_END]       = "play_near_end",
    [ORMET_PLAY_END]            = "play_end",
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
    cJSON_AddStringToObject(nroot_data, "send_id", send_id);
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
                "format": "pcm",
                "sample_rate": 16000,
                "channels": 1,
                "frame_duration": 60
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
        cJSON_AddNumberToObject(naudio_params, "channels", 1);
        cJSON_AddNumberToObject(naudio_params, "frame_duration", 60);
        cJSON_AddItemToObject(nroot_data, "audio_params", naudio_params);
    }

	char *json_str = cJSON_PrintUnformatted(nroot);

    if (NULL != nroot) {
        cJSON_Delete(nroot);
    }

    return json_str;
}

inline char *odmp_req_message_initial_setup(const char **targets, int targets_num, const char *c_id, const char *send_id) {
    return odmp_req_message(targets, targets_num, c_id, send_id, false, ORMET_INITIAL_SETUP);
}

inline char *odmp_req_message_near_trigger(const char **targets, int targets_num, const char *c_id, const char *send_id) {
    return odmp_req_message(targets, targets_num, c_id, send_id, false, ORMET_NEAR_TRIGGER);
}

inline char *odmp_req_message_new_device_in(const char **targets, int targets_num, const char *c_id, const char *send_id) {
    return odmp_req_message(targets, targets_num, c_id, send_id, false, ORMET_NEW_DEVICE_IN);
}

inline char *odmp_req_message_device_leave(const char **targets, int targets_num, const char *c_id, const char *send_id) {
    return odmp_req_message(targets, targets_num, c_id, send_id, false, ORMET_DEVICE_LEAVE);
}

inline char *odmp_req_message_switch_success(const char **targets, int targets_num, const char *c_id, const char *send_id) {
    return odmp_req_message(targets, targets_num, c_id, send_id, false, ORMET_SWITCH_SUCCESS);
}

inline char *odmp_req_message_switch_failed(const char **targets, int targets_num, const char *c_id, const char *send_id) {
    return odmp_req_message(targets, targets_num, c_id, send_id, false, ORMET_SWITCH_FAILED);
}

inline char *odmp_req_message_button_trigger(const char **targets, int targets_num, const char *c_id, const char *send_id) {
    return odmp_req_message(targets, targets_num, c_id, send_id, false, ORMET_BUTTON_TRIGGER);
}

inline char *odmp_req_message_button_press(const char **targets, int targets_num, const char *c_id, const char *send_id) {
    return odmp_req_message(targets, targets_num, c_id, send_id, false, ORMET_BUTTON_PRESS);
}

inline char *odmp_req_message_button_double_press(const char **targets, int targets_num, const char *c_id, const char *send_id) {
    return odmp_req_message(targets, targets_num, c_id, send_id, false, ORMET_BUTTON_DOUBLE_PRESS);
}

inline char *odmp_req_message_button_start(const char **targets, int targets_num, const char *c_id, const char *send_id) {
    return odmp_req_message(targets, targets_num, c_id, send_id, false, ORMET_BUTTON_START);
}

inline char *odmp_req_message_button_end(const char **targets, int targets_num, const char *c_id, const char *send_id) {
    return odmp_req_message(targets, targets_num, c_id, send_id, false, ORMET_BUTTON_END);
}

inline char *odmp_req_message_move_patrol(const char **targets, int targets_num, const char *c_id, const char *send_id) {
    return odmp_req_message(targets, targets_num, c_id, send_id, false, ORMET_MOVE_PATROL);
}

inline char *odmp_req_message_move_stop(const char **targets, int targets_num, const char *c_id, const char *send_id) {
    return odmp_req_message(targets, targets_num, c_id, send_id, false, ORMET_MOVE_STOP);
}

inline char *odmp_req_message_move_shake(const char **targets, int targets_num, const char *c_id, const char *send_id) {
    return odmp_req_message(targets, targets_num, c_id, send_id, false, ORMET_MOVE_SHAKE);
}

inline char *odmp_req_message_vad_speaker_start(const char **targets, int targets_num, const char *c_id, const char *send_id) {
    return odmp_req_message(targets, targets_num, c_id, send_id, false, ORMET_VAD_SPEAKER_START);
}

inline char *odmp_req_message_vad_speaker_end(const char **targets, int targets_num, const char *c_id, const char *send_id) {
    return odmp_req_message(targets, targets_num, c_id, send_id, false, ORMET_VAD_SPEAKER_END);
}

inline char *odmp_req_message_lower_power(const char **targets, int targets_num, const char *c_id, const char *send_id) {
    return odmp_req_message(targets, targets_num, c_id, send_id, false, ORMET_LOWER_POWER);
}

inline char *odmp_req_message_lower_battery(const char **targets, int targets_num, const char *c_id, const char *send_id) {
    return odmp_req_message(targets, targets_num, c_id, send_id, false, ORMET_LOWER_BATTERY);
}

inline char *odmp_req_message_play_start(const char **targets, int targets_num, const char *c_id, const char *send_id) {
    return odmp_req_message(targets, targets_num, c_id, send_id, false, ORMET_PLAY_START);
}

inline char *odmp_req_message_play_near_end(const char **targets, int targets_num, const char *c_id, const char *send_id) {
    return odmp_req_message(targets, targets_num, c_id, send_id, false, ORMET_PLAY_NEAR_END);
}

inline char *odmp_req_message_play_end(const char **targets, int targets_num, const char *c_id, const char *send_id) {
    return odmp_req_message(targets, targets_num, c_id, send_id, false, ORMET_PLAY_END);
}

static int initial_setup_response_event_handler(const cJSON *nroot, odmp_package_t *pkg)
{
    const cJSON *ndata = cjson_get_object(nroot, "data");
    if (ndata == NULL) {
        return PARSE_Error;
    }
    if (cjson_get_boolean_value(ndata, "setup_complete", &pkg->initial_setup_response.setup_complete)) {
        return PARSE_Error;
    }
    return PARSE_Success;
}

static int device_switch_response_event_handler(const cJSON *nroot, odmp_package_t *pkg)
{
    const cJSON *ndata = cjson_get_object(nroot, "data");
    if (ndata == NULL) {
        return PARSE_Error;
    }
    char *vdevice_id = cjson_get_string_value(ndata, "device_id");
    if (vdevice_id == NULL) {
        return PARSE_Error;
    }
    pkg->device_switch_response.device_id = vdevice_id;
    char *vstatus = cjson_get_string_value(ndata, "status");
    if (vstatus == NULL) {
        return PARSE_Error;
    }
    pkg->device_switch_response.status = vstatus;
    return PARSE_Success;
}

static int tts_bind_event_handler(const cJSON *nroot, odmp_package_t *pkg)
{
    const cJSON *ndata = cjson_get_object(nroot, "data");
    if (ndata == NULL) {
        return PARSE_Error;
    }
    char *v_c_id = cjson_get_string_value(ndata, "c_id");
    if (v_c_id == NULL) {
        return PARSE_Error;
    }
    pkg->tts_bind.c_id = v_c_id;
    char *v_send_id = cjson_get_string_value(ndata, "send_id");
    if (v_send_id == NULL) {
        return PARSE_Error;
    }
    pkg->tts_bind.send_id = v_send_id;
    if (cjson_get_integer_value(ndata, "wait_time", &pkg->tts_bind.wait_time)) {
        return PARSE_Error;
    }
    return PARSE_Success;
}

static int audio_status_event_handler(const cJSON *nroot, odmp_package_t *pkg)
{
    const cJSON *ndata = cjson_get_object(nroot, "data");
    if (ndata == NULL) {
        return PARSE_Error;
    }

    char *v_c_id = cjson_get_string_value(ndata, "c_id");
    if (v_c_id == NULL) {
        return PARSE_Error;
    }
    pkg->audio_status.c_id = v_c_id;

    if (cjson_get_integer_value(ndata, "type", &pkg->audio_status.type)) {
        return PARSE_Error;
    }
    if (cjson_get_integer_value(ndata, "event", &pkg->audio_status.event)) {
        return PARSE_Error;
    }

    const cJSON *naudio_params = cjson_get_object(ndata, "audio_params");
    if (naudio_params == NULL) {
        pkg->audio_status.has_audio_param = 0;
    } else {
        char *vformat = cjson_get_string_value(naudio_params, "format");
        if (vformat == NULL) {
            return PARSE_Error;
        }
        pkg->audio_status.audio_param.format = vformat;

        if (cjson_get_integer_value(naudio_params, "sample_rate", &pkg->audio_status.audio_param.sample_rate)) {
            return PARSE_Error;
        }
        if (cjson_get_integer_value(naudio_params, "channels", &pkg->audio_status.audio_param.channels)) {
            return PARSE_Error;
        }
        if (cjson_get_integer_value(naudio_params, "frame_duration", &pkg->audio_status.audio_param.frame_duration)) {
            return PARSE_Error;
        }
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

static int execution_time_event_handler(const cJSON *nroot, odmp_package_t *pkg)
{
    const cJSON *ndata = cjson_get_object(nroot, "data");
    if (ndata == NULL) {
        return PARSE_Error;
    }
    char *vevent = cjson_get_string_value(ndata, "event");
    if (vevent == NULL) {
        return PARSE_Error;
    }
    pkg->execution_time.event = vevent;
    if (cjson_get_integer_value(ndata, "execution_time", &pkg->execution_time.execution_time)) {
        return PARSE_Error;
    }
    return PARSE_Success;
}

static int pong_event_handler(const cJSON *nroot, odmp_package_t *pkg)
{
    return PARSE_Success;
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
    {"message_text", OR_MESSAGE_TEXT, message_text_event_handler, NULL}, 
    {"execution_time", OR_EXECUTION_TIME, execution_time_event_handler, NULL},
    {"pong", OR_PONG, pong_event_handler}, 
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

    if (cjson_get_integer_value(nroot, "errno", &pkg.errno)) {
        // errno not exists
    }
    // pkg.errmsg = cjson_get_string_value(nroot, "errmsg");
    pkg.event  = cjson_get_string_value(nroot, "event");
    pkg.uuid   = cjson_get_string_value(nroot, "uuid");
    // pkg.debug  = cjson_get_string_value(nroot, "debug");
    if (pkg.event == NULL) {
        goto error_ret;
    }
    
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
        /* LingOS Successful */
        if (entry->handler != NULL) {
            int ret = entry->handler(nroot, &pkg);
            if (ret == PARSE_Success && g_odmp_cbk != NULL) {
                g_odmp_cbk(entry->type, &pkg);
            }
        }
        if (entry->release != NULL) {
            entry->release(&pkg);
        }
    } else {
        /* LingOS Error */
        if (g_odmp_cbk != NULL) {
            g_odmp_cbk(entry->type, &pkg);
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