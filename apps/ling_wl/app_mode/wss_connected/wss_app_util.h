#ifndef __WSS_APP_UTIL_H__
#define __WSS_APP_UTIL_H__

#include "wss/wss_client.h"

int communication_talk_message(char *content);
int communication_init_role(int ip_id);
int communication_session_switch(int session_id, int ip_id);
int communication_message_asr();
int communication_message_asr_start();
int communication_message_asr_end();
int communication_message_asr_data(char *data_base64);
int communication_message_stop();
int communication_ping();

int send_bin_data(uint8_t *data, uint32_t len);
int send_audio_status(const char *c_id, int type, int event);
int send_req_message(const char *c_id, const char *send_id, bool is_play, int event_type);

#endif