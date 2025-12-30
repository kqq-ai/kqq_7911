#ifndef __WSS_AUDIO_H__
#define __WSS_AUDIO_H__

#include "cpu.h"

void wss_audio_init();
void wss_audio_uninit();

void wss_ble_audio_start(); // request 控制 允许WSS音频数据写入播放器，允许播放
void wss_ble_audio_stop();  // request 控制 停止数据写入，暂停播放器

void wss_audio_allow_playback();

void set_tts_volume(int volume);

#endif