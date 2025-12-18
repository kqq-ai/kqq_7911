#ifndef __WSS_MIC_H__
#define __WSS_MIC_H__

#include "cpu.h"

void wss_mic_init();
void wss_mic_uninit();

bool is_recording_uploading();// 录音上传状态

void wss_mic_start();       // APP 控制 MIC 录音状态
void wss_mic_stop();        // APP 控制 MIC 录音状态

void wss_vad_start(uint timestamp); // APP 控制 VAD 及音频上传状态

void wss_asr_start();
void wss_asr_stop();

#endif