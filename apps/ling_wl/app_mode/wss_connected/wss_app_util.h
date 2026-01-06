#ifndef __WSS_APP_UTIL_H__
#define __WSS_APP_UTIL_H__

#include "cpu.h"

// SN集合相关操作
void add_target_sn(const char* sn);
void remove_target_sn(const char* sn);
void clear_target_sn_set(void);
int is_target_sn_in_set(const char* sn);

int send_bin_data(uint8_t *data, uint32_t len);
int send_req_message(const char *send_id, bool is_play, int event_type);
int send_device_status(int status);
int send_nearby_update(void **devices, int device_count);
int send_audio_stream_start(const char *stream_id, const char *stream_type);
int send_audio_stream_end(const char *stream_id, const char *status);
int send_bind_result(const char *sn, bool success, const char *status, const char *stream_id);
int send_audio_play_started(const char *stream_id, const char *status, const char *play_mode);
int send_audio_play_finished(const char *stream_id, const char *status);
int send_btn_tap(int type, int duration);

bool wss_check_exit(void);
bool wss_connect(void);
void wss_disconnect(void);
bool wss_reconnect(void);
const char *wss_get_last_disconnect_reason(void);

#endif