#ifndef __WSS_APP_UTIL_H__
#define __WSS_APP_UTIL_H__

#include "cpu.h"

int send_bin_data(uint8_t *data, uint32_t len);
int send_audio_status(int type, int event);
int send_req_message(const char *send_id, bool is_play, int event_type);

#endif