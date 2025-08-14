#ifndef __WSS_REQUEST_H__
#define __WSS_REQUEST_H__

#include "cpu.h"
#include "app_mode/lwl_app_core.h"

void wss_request_send_event(lwl_eend_dev_t *p_dev, int msg_event);

void wss_request_init();
void wss_request_uninit();
bool wss_request_loop();

bool is_request_busy();

#endif