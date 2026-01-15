#ifndef __WSS_MIC_H__
#define __WSS_MIC_H__

#include "cpu.h"

void wss_mic_init();
void wss_mic_uninit();

void wss_vad_upload_start(uint timestamp); // APP 控制 VAD 及音频上传状态
void wss_mic_upload_start();
bool wss_upload_stop();

#endif