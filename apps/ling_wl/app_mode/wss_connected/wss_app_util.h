#ifndef __WSS_APP_UTIL_H__
#define __WSS_APP_UTIL_H__

#include "wss/wss_client.h"

void communication_talk_message(struct websocket_struct *websockets_info, char *content);
void communication_init_role(struct websocket_struct *websockets_info, int ip_id);
void wss_2ip_switch(struct websocket_struct *websockets_info, int session_id, int ip_id);
void communication_message_asr(struct websocket_struct *websockets_info);
void communication_message_asr_start(struct websocket_struct *websockets_info);
void communication_message_asr_end(struct websocket_struct *websockets_info);
void communication_message_asr_data(struct websocket_struct *websockets_info, char *data_base64);
void communication_message_stop(struct websocket_struct *websockets_info);
void wss_ping_send(struct websocket_struct *websockets_info);

#endif