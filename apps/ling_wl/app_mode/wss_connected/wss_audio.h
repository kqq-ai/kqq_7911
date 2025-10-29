#ifndef __WSS_AUDIO_H__
#define __WSS_AUDIO_H__

#include "cpu.h"

void wss_audio_init();
void wss_audio_uninit();

u32 get_time_stamp();

bool is_audio_playing();
void wss_ble_audio_start();
void wss_ble_audio_stop();

bool is_recording_uploading();
bool is_mic_on();
void wss_mic_start(uint timestamp);
void wss_mic_stop(bool need_upload);

void set_Pi_volume(int volume);

#endif