#ifndef __WSS_APP_UTIL_H__
#define __WSS_APP_UTIL_H__

#include "wss/wss_client.h"

static void communication_talk_message(struct websocket_strcut *websockets_info, char *content);

static void communication_init_role(struct websocket_struct *websockets_info, int ip_id);

static void wss_2ip_switch(struct websocket_struct *websockets_info, int session_id, int ip_id);

static void communication_message_asr(struct websocket_strcut *websockets_info);

static void communication_message_asr_start(struct websocket_strcut *websockets_info);

static void communication_message_asr_end(struct websocket_strcut *websockets_info);

static void communication_message_asr_data(struct websocket_strcut *websockets_info, char *data_base64);

static void communication_message_stop(struct websocket_strcut *websockets_info);

static void wss_ping_send(struct websocket_struct *websockets_info);

#endif