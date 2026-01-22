#ifndef __ODMP_H__
#define __ODMP_H__

#include "cpu.h"

#undef errno

enum odmp_req_msg_event_type_e {
  ORMET_NONE = 0,
  // 初次使用
  ORMET_INITIAL_SETUP = 1,
  // 蓝牙测距
  ORMET_NEAR_TRIGGER = 21,
  ORMET_SWITCH_SUCCESS = 24,
  ORMET_SWITCH_FAILED = 25,
  // 按键触发
  ORMET_BUTTON_TRIGGER = 31,
  // 播放状态
  ORMET_PLAY_STOP = 50,
  ORMET_PLAY_END = 55,
  ORMET_PLAY_NEXT = 56,
};

enum odmp_audio_status_type_e {
  OAST_ASR = 1,
  OAST_TTS = 2,
  OAST_STREAM_AUDIO = 3,
  OAST_SCRIPT = 4,
};

enum odmp_audio_status_event_e {
  OASE_START = 1,
  OASE_END = 2,
};

enum odmp_device_status_e {
  DEVICE_STATUS_IDLE = 0,           // 在线
  DEVICE_STATUS_AUDIO_PLAYING = 1,  // 正在播放...
  DEVICE_STATUS_RECORDING = 2,      // 正在录音...
  DEVICE_STATUS_BLE_AUDIO_STOP = 3, // 播放结束
};

enum odmp_event_type_e {
  OET_PING = 0,
  OET_REQ_MESSAGE = 1,
  OET_AUDIO_STATUS = 2,
  OET_SET_AUDIO_FORMAT = 3,
  OET_NEARBY_UPDATE = 4,      // nearby_update事件
  OET_AUDIO_STREAM_START = 5, // audio_stream_start事件
};

enum ORX_EVENT_TYPE {
  TTS_BIND,
  MESSAGE_TEXT,
  PONG,
  AUDIO_STREAM_START,
  AUDIO_STREAM_END,
};

typedef struct {
  bool setup_complete;
} odmp_pkg_initial_setup_response_t;

typedef struct {
  char *device_id;
  char *status;
} odmp_pkg_device_switch_response_t;

typedef struct {
  char *sn;
  char *stream_id;
  int role_volume;
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
  char *reason;
  int err_code;
} odmp_pkg_error_event_t;

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
    odmp_pkg_error_event_t error;
  };
} odmp_package_t;

typedef int (*g_odmp_cbk_t)(int pkg_type, odmp_package_t *pkg);
typedef int (*g_odmp_audio_data_cbk_t)(uint8_t *data, uint32_t len);
typedef int (*g_odmp_disconnect_cbk_t)(void);

const char *odmp_et_to_text(int event_type);
const char *odmp_ast_to_text(int audio_status_type);
const char *odmp_ase_to_text(int audio_status_event);
const char *odmp_rmet_to_text(int req_message_event_type);

char *odmp_send_ping();
char *odmp_req_message(const char **targets, int targets_num, const char *c_id,
                       const char *send_id, bool is_play, int ormet);
char *odmp_audio_status(const char *c_id, int oast, int oase);

char *odmp_set_audio_format(const char *c_id, int type);
char *odmp_device_status(int status);
char *odmp_nearby_update(void **devices, int device_count, const char *c_id);
char *odmp_audio_stream_start(const char *stream_id, const char *stream_type);
char *odmp_audio_stream_end(const char *stream_id, const char *status);
char *odmp_bind_result(const char *sn, bool success, const char *status,
                       const char *stream_id);
char *odmp_audio_play_started(const char *stream_id, const char *status,
                              const char *play_mode);
char *odmp_audio_play_finished(const char *stream_id, const char *status);
char *odmp_btn_tap(int type, int duration);

void odmp_register_rsp_cbk(g_odmp_cbk_t odmp_cbk);
void odmp_register_audio_data_cbk(g_odmp_audio_data_cbk_t odmp_audio_data_cbk);
void odmp_register_disconnect_cbk(g_odmp_disconnect_cbk_t odmp_disconnect_cbk);

void odmp_rsp_handler(u8 *buf, u32 len);
void odmp_audio_data_handler(u8 *buf, u32 len);
void odmp_disconnect_handler(void);

#endif /*__ODMP_H__*/