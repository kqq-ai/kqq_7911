#ifndef __WSS_AUDIO_H__
#define __WSS_AUDIO_H__

#include "cpu.h"

void wss_audio_init();
void wss_audio_uninit();

void wss_ble_audio_start(const char *stream_id); // request 控制 允许WSS音频数据写入播放器，允许播放
void wss_ble_audio_stop(const char *stream_id, bool finish);  // request 控制 停止数据写入，暂停播放器，finish=true表示实际播放完成

void wss_audio_allow_playback();
void wss_audio_set_stream_end();
bool wss_audio_is_stream_end();

void set_tts_volume(int volume);

#endif