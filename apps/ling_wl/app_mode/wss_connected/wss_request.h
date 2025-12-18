#ifndef __WSS_REQUEST_H__
#define __WSS_REQUEST_H__

#include "cpu.h"

void wss_request_init();
void wss_request_uninit();
bool wss_request_loop();

bool wss_request_is_busy();         //state != STATE_IDLE
bool wss_request_is_playing();      //state == STATE_DOWNLOAD || STATE_PLAYING || STATE_TTS_BIND

void wss_request_stop();
void wss_request_pause();
void wss_request_resume();

void wss_request_send_event(char *sn, int msg_event);
void wss_request_send_event_target(int msg_event);

#endif