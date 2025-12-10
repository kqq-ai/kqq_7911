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
    [OET_PING]                  = "ping",
    [OET_REQ_MESSAGE]           = "req_message",
    [OET_AUDIO_STATUS]          = "audio_status",
    [OET_SET_AUDIO_FORMAT]      = "set_audio_format",
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

        if (cjson_get_integer_value(naudio_params, "volume", &pkg->audio_status.volume)) {
            pkg->audio_status.volume = 70;
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
        /*  Successful */
        if (entry->handler != NULL) {
            int ret = entry->handler(nroot, &pkg);
            if (ret == PARSE_Success && g_odmp_cbk != NULL) {
                log_i("odmp_rsp_handler: event %s, type %d", pkg.event, entry->type);
                g_odmp_cbk(entry->type, &pkg);
            }
        }
        if (entry->release != NULL) {
            entry->release(&pkg);
        }
    } else {
        /*  Error */
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
