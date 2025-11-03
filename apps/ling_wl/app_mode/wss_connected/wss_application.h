#ifndef __WSS_APPLICATION_H__
#define __WSS_APPLICATION_H__

#include "cpu.h"

#define play_prompt_sound(x) wss_app_mode_play(CONFIG_CP_VOICE_FILE_PATH x);

void wss_application_vailed();
void wss_app_mode_play(const char *fname);

void wss_application_init();
void wss_application_uninit();
bool wss_application_loop();

#endif