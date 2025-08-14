#ifndef __WSS_APPLICATION_H__
#define __WSS_APPLICATION_H__

#include "cpu.h"
#include "wss/wss_client.h"

void wss_application_vailed(int Volume);

void wss_application_init();
void wss_application_uninit();
bool wss_application_loop();

#endif