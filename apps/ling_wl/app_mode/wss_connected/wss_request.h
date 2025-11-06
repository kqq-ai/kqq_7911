#ifndef __WSS_REQUEST_H__
#define __WSS_REQUEST_H__

#include "cpu.h"

void wss_request_init();
void wss_request_uninit();
bool wss_request_loop();
void wss_request_stop();

bool wss_request_is_busy();         //state != STATE_IDLE
bool is_player_busy();              //state == STATE_DOWNLOAD || is_audio_playing()

void set_mic(bool on);
void wss_request_resume_le_audio(); // disable C speaker and set Pi_volume
void wss_request_resume_mic();      // resume mic if g_mic_on == true && not busy

void wss_request_touch_key_down();
bool wss_request_touch_key_up();

void wss_request_send_event(char *sn, int msg_event);

#endif