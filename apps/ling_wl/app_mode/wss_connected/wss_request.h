#ifndef __WSS_REQUEST_H__
#define __WSS_REQUEST_H__

#include "cpu.h"

void wss_request_init();
void wss_request_uninit();
bool wss_request_loop();
void wss_request_stop();

bool wss_request_is_busy();         //state != STATE_IDLE

void wss_request_touch_key_down();
bool wss_request_touch_key_up();

void wss_request_send_event(char *sn, int msg_event);
void wss_request_send_event_target(int msg_event);

#endif