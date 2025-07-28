#ifndef __WSS_APP_UTIL_H__
#define __WSS_APP_UTIL_H__

#include "cpu.h"

// SN集合相关操作
void add_target_sn(const char* sn);
void remove_target_sn(const char* sn);
void clear_target_sn_set(void);
int is_target_sn_in_set(const char* sn);
// char** get_target_sn_set(int* count);

int send_bin_data(uint8_t *data, uint32_t len);
int send_audio_status(int type, int event);
int send_req_message(const char *send_id, bool is_play, int event_type);

void wss_exception_exit();
bool wss_check_exit();

bool wss_connect();
void wss_disconnect();

#endif