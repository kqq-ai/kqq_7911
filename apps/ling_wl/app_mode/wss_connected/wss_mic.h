#ifndef __WSS_MIC_H__
#define __WSS_MIC_H__

#include "cpu.h"

void wss_mic_init();
void wss_mic_uninit();

void wss_mic_start(const char *stream_id);       // APP 控制 MIC 录音状态
void wss_mic_stop();        // APP 控制 MIC 录音状态

#endif