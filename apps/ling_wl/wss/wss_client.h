#ifndef __WSS_CLIENT_H__
#define __WSS_CLIENT_H__

#include "cpu.h"
#include "web_socket/websocket_api.h"

typedef void (*user_wss_recv_cb_t)(u8 *buf, u32 len, u8 type);

struct websocket_struct *wss_server_connect(char *url, user_wss_recv_cb_t websockets_callback);
int  wss_server_send(struct websocket_struct *websockets_info, u8 *buf, int size);
void wss_server_disconnect(struct websocket_struct *websockets_info);

#endif