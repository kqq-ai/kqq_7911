#ifndef __WSS_MIC_H__
#define __WSS_MIC_H__

#include "cpu.h"

void wss_mic_init();
void wss_mic_uninit();

void wss_mic_upload_start();
bool wss_upload_stop();

#endif