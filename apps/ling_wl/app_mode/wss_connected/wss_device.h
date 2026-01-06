#ifndef __WSS_DEVICE_H__
#define __WSS_DEVICE_H__

#include "cpu.h"
#include "app_mode/lwl_app_core.h"

void wss_device_init();
void wss_device_uninit();
lwl_eend_dev_t * wss_device_loop();
lwl_eend_dev_t * wss_device_call();
bool update_edev_time(char* sn);

void nearby_update_handler_init(void);
void nearby_update_handler_uninit(void);

#endif