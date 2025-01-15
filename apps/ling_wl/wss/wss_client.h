#ifndef __WSS_CLIENT_H__
#define __WSS_CLIENT_H__

#include "web_socket/websocket_api.h"

struct websocket_struct *connect_wss_server(char *url);
void disconnect_wss_server(struct websocket_struct *websockets_info);

#endif