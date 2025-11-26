#ifndef __ODMP_H__
#define __ODMP_H__

#include "cpu.h"

#undef errno

enum odmp_req_msg_event_type_e{
    ORMET_NONE                  = 0,
    // 初次使用
    ORMET_INITIAL_SETUP         = 1,
    // 蓝牙测距
    ORMET_NEAR_TRIGGER          = 21,
    ORMET_SWITCH_SUCCESS        = 24,
    ORMET_SWITCH_FAILED         = 25,
    // 按键触发
    ORMET_BUTTON_TRIGGER        = 31,
    // 播放状态
    ORMET_PLAY_STOP             = 50,
    ORMET_PLAY_END              = 55,
    ORMET_PLAY_NEXT             = 56,
};

enum odmp_audio_status_type_e{
    OAST_ASR                    = 1,
    OAST_TTS                    = 2,
    OAST_STREAM_AUDIO           = 3,
};

enum odmp_audio_status_event_e{
    OASE_START                  = 1,
    OASE_END                    = 2,
};

enum odmp_event_type_e {
    OET_REQ_MESSAGE             = 1,
    OET_AUDIO_STATUS            = 2,
    OET_SET_AUDIO_FORMAT        = 3,
};


enum ORX_EVENT_TYPE {
    OR_NONE,
    OR_TTS_BIND,
    OR_AUDIO_STATUS,
    OR_INITIAL_SETUP_RESPONSE,
    OR_DEVICE_SWITCH_RESPONSE,
    OR_MESSAGE_TEXT,
    OR_EXECUTION_TIME,
    OR_PONG,
};

typedef struct {
    bool setup_complete;
} odmp_pkg_initial_setup_response_t;

typedef struct {
    char *device_id;
    char *status;
} odmp_pkg_device_switch_response_t;

typedef struct {
    char *c_id;
    char *send_id;
    int wait_time;
} odmp_pkg_tts_bind_t;

typedef struct {
    char *format;
    int sample_rate;
} odmp_audio_param_t;

typedef struct {
    char *c_id;
    int type;
    int event;
    int has_audio_param;
    int volume;
    odmp_audio_param_t audio_param;
} odmp_pkg_audio_status_t;

typedef struct {
    char *c_id;
    char *send_id;
    char *text;
    int type;
} odmp_pkg_message_text_t;

typedef struct {
    char *event;
    int execution_time;
} odmp_pkg_execution_time_t;

typedef struct {
} odmp_pkg_pong_t;

typedef struct {
    u8 type;
    int errno;
    char *uuid;
    char *event;
    char *errmsg;
    char *debug;
    union {
        odmp_pkg_audio_status_t audio_status;
        odmp_pkg_tts_bind_t tts_bind;
        odmp_pkg_initial_setup_response_t initial_setup_response;
        odmp_pkg_device_switch_response_t device_switch_response;
        odmp_pkg_message_text_t message_text;
        odmp_pkg_execution_time_t execution_time;
        odmp_pkg_pong_t pong;
    };
} odmp_package_t;

typedef int (*g_odmp_cbk_t)(int pkg_type, odmp_package_t *pkg);
typedef int (*g_odmp_audio_data_cbk_t)(uint8_t *data, uint32_t len);
typedef int (*g_odmp_disconnect_cbk_t)(void);


const char *odmp_et_to_text(int event_type);
const char *odmp_ast_to_text(int audio_status_type);
const char *odmp_ase_to_text(int audio_status_event);
const char *odmp_rmet_to_text(int req_message_event_type);

char *odmp_req_message(const char **targets, int targets_num, const char *c_id, const char *send_id, bool is_play, int ormet);
char *odmp_audio_status(const char *c_id, int oast, int oase);

char *odmp_set_audio_format(const char *c_id, int type);

void odmp_register_rsp_cbk(g_odmp_cbk_t odmp_cbk);
void odmp_register_audio_data_cbk(g_odmp_audio_data_cbk_t odmp_audio_data_cbk);
void odmp_register_disconnect_cbk(g_odmp_disconnect_cbk_t odmp_disconnect_cbk);

void odmp_rsp_handler(u8 *buf, u32 len);
void odmp_audio_data_handler(u8 *buf, u32 len);
void odmp_disconnect_handler(void);

#endif /*__ODMP_H__*/