#ifndef __ODMP_H__
#define __ODMP_H__

#include "cpu.h"

#undef errno

enum odmp_req_msg_event_type_e{
    // 初次使用
    ORMET_INITIAL_SETUP         = 1,
    // 蓝牙测距
    ORMET_NEAR_TRIGGER          = 21,
    ORMET_NEW_DEVICE_IN         = 22,
    ORMET_DEVICE_LEAVE          = 23,
    ORMET_SWITCH_SUCCESS        = 24,
    ORMET_SWITCH_FAILED         = 25,
    // 按键触发
    ORMET_BUTTON_TRIGGER        = 3,
    ORMET_BUTTON_PRESS          = 31,
    ORMET_BUTTON_DOUBLE_PRESS   = 32,
    ORMET_BUTTON_START          = 33,
    ORMET_BUTTON_END            = 34,
    // 加速度
    ORMET_MOVE_PATROL           = 41,
    ORMET_MOVE_STOP             = 42,
    ORMET_MOVE_SHAKE            = 43,
    // VAD:语音上传
    ORMET_VAD_SPEAKER_START     = 51,
    ORMET_VAD_SPEAKER_END       = 52,
    // 状态
    // ORMET_LOWER_POWER           = 61,
    // ORMET_LOWER_BATTERY         = 62,
    ORMET_PLAY_START            = 53,
    ORMET_PLAY_NEAR_END         = 54,
    ORMET_PLAY_END              = 55,
};

enum odmp_audio_status_type_e{
    OAST_ASR                    = 1,
    OAST_TTS                    = 2,
};

enum odmp_audio_status_event_e{
    OASE_START                  = 1,
    OASE_END                    = 2,
    OASE_ABNORMAL               = 3,
    OASE_SENTENCE_START         = 4,
    OASE_SENTENCE_END           = 5,
};

enum odmp_event_type_e {
    OET_REQ_MESSAGE             = 1,
    OET_AUDIO_STATUS            = 2,
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
    int channels;
    int frame_duration;
} odmp_audio_param_t;

typedef struct {
    char *c_id;
    int type;
    int event;
    int has_audio_param;
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


const char *odmp_et_to_text(int event_type);
const char *odmp_ast_to_text(int audio_status_type);
const char *odmp_ase_to_text(int audio_status_event);
const char *odmp_rmet_to_text(int req_message_event_type);

char *odmp_req_message(const char **targets, int targets_num, const char *c_id, const char *send_id, bool is_play, int ormet);
char *odmp_audio_status(const char *c_id, int oast, int oase);

char *odmp_req_message_initial_setup(const char **targets, int targets_num, const char *c_id, const char *send_id);
char *odmp_req_message_near_trigger(const char **targets, int targets_num, const char *c_id, const char *send_id);
char *odmp_req_message_new_device_in(const char **targets, int targets_num, const char *c_id, const char *send_id);
char *odmp_req_message_device_leave(const char **targets, int targets_num, const char *c_id, const char *send_id);
char *odmp_req_message_switch_success(const char **targets, int targets_num, const char *c_id, const char *send_id);
char *odmp_req_message_switch_failed(const char **targets, int targets_num, const char *c_id, const char *send_id);
char *odmp_req_message_button_trigger(const char **targets, int targets_num, const char *c_id, const char *send_id);
char *odmp_req_message_button_press(const char **targets, int targets_num, const char *c_id, const char *send_id);
char *odmp_req_message_button_double_press(const char **targets, int targets_num, const char *c_id, const char *send_id);
char *odmp_req_message_button_start(const char **targets, int targets_num, const char *c_id, const char *send_id);
char *odmp_req_message_button_end(const char **targets, int targets_num, const char *c_id, const char *send_id);
char *odmp_req_message_move_patrol(const char **targets, int targets_num, const char *c_id, const char *send_id);
char *odmp_req_message_move_stop(const char **targets, int targets_num, const char *c_id, const char *send_id);
char *odmp_req_message_move_shake(const char **targets, int targets_num, const char *c_id, const char *send_id);
char *odmp_req_message_vad_speaker_start(const char **targets, int targets_num, const char *c_id, const char *send_id);
char *odmp_req_message_vad_speaker_end(const char **targets, int targets_num, const char *c_id, const char *send_id);
char *odmp_req_message_lower_power(const char **targets, int targets_num, const char *c_id, const char *send_id);
char *odmp_req_message_lower_battery(const char **targets, int targets_num, const char *c_id, const char *send_id);
char *odmp_req_message_play_start(const char **targets, int targets_num, const char *c_id, const char *send_id);
char *odmp_req_message_play_near_end(const char **targets, int targets_num, const char *c_id, const char *send_id);
char *odmp_req_message_play_end(const char **targets, int targets_num, const char *c_id, const char *send_id);


void odmp_register_rsp_cbk(g_odmp_cbk_t odmp_cbk);
void odmp_register_audio_data_cbk(g_odmp_audio_data_cbk_t odmp_audio_data_cbk);

void odmp_rsp_handler(u8 *buf, u32 len);
void odmp_audio_data_handler(u8 *buf, u32 len);

#endif /*__ODMP_H__*/