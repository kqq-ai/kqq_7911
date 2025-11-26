#ifndef __WSS_AUDIO_H__
#define __WSS_AUDIO_H__

#include "cpu.h"

void wss_audio_init();
void wss_audio_uninit();

bool is_audio_playing();    // 网络音频流的播放器是否启动或准备启动（有超时机制）
bool is_recording_uploading();// 录音上传状态

void music_play_stop_in_thread(); // 用线程停止音频播放，避免阻塞主线程

void wss_ble_audio_start(); // request 控制 允许WSS音频数据写入播放器，允许播放
void wss_ble_audio_stop();  // request 控制 停止数据写入，暂停播放器

void wss_mic_start();       // APP 控制 MIC 录音状态
void wss_mic_stop();        // APP 控制 MIC 录音状态

void wss_vad_start(uint timestamp); // APP 控制 VAD 及音频上传状态

void wss_asr_start();
void wss_asr_stop();

void set_tts_volume(int volume);
void set_C_speaker_on(bool on);

#endif