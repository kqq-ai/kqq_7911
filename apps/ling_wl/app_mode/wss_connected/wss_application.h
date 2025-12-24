#ifndef __WSS_APPLICATION_H__
#define __WSS_APPLICATION_H__

#include "cpu.h"

#define play_prompt_sound(x) play_prompt(CONFIG_CP_VOICE_FILE_PATH x);

void play_prompt(const char *fname);

void wss_application_init();
void wss_application_uninit();
void wss_application_reset();
bool wss_application_loop();

bool wss_application_check_power_key();

#endif