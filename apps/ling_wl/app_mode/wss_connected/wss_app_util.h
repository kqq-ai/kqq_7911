#ifndef __WSS_APP_UTIL_H__
#define __WSS_APP_UTIL_H__

#include "wss/wss_client.h"

int send_bin_data(uint8_t *data, uint32_t len);
int send_audio_status(const char *c_id, int type, int event);
int send_req_message(const char *c_id, const char *send_id, bool is_play, int event_type);

#endif