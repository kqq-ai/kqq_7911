#ifndef __WSS_REQUEST_H__
#define __WSS_REQUEST_H__

#include "cpu.h"

void wss_request_init();
void wss_request_uninit();

void wss_request_send_event(char *sn, int msg_event);
void wss_request_send_event_target(int msg_event);

const char *wss_request_get_stream_id(void);

#endif