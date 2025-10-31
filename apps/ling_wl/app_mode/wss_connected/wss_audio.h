#ifndef __WSS_AUDIO_H__
#define __WSS_AUDIO_H__

#include "cpu.h"

void wss_audio_init();
void wss_audio_uninit();

u32 get_time_stamp();

bool is_audio_playing();
bool is_recording_uploading();

void wss_ble_audio_start(); // request 控制 MIC 录音状态
void wss_ble_audio_stop();  // request 控制 MIC 录音状态

void wss_mic_start();       // APP 控制 MIC 录音状态
void wss_mic_stop();        // APP 控制 MIC 录音状态

void wss_vad_start(uint timestamp); // request 控制 VAD 及音频上传状态
void wss_vad_stop();                // request 控制 VAD 及音频上传状态

void set_Pi_volume(int volume);

#endif