#ifndef __WSS_CLIENT_H__
#define __WSS_CLIENT_H__


#include "cpu.h"

void wss_asr_start(void);
void wss_asr_data(void *data, u32 size);
void wss_asr_end(void);

#endif