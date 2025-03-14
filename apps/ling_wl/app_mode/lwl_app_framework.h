#ifndef __LWL_APP_FRAMEWORK_H__
#define __LWL_APP_FRAMEWORK_H__

#include "cpu.h"
#include "app_core.h"

enum {
    APP_POWER_ON_MODE           = 1,
    APP_DEFAULT_MODE            = 2,
    APP_NET_CONNECTING_MODE     = 3,
    APP_NET_CONNECTED_MODE      = 4,
    APP_UPDATE_MODE             = 5,
    APP_WSS_CONNECTED_MODE      = 6,
};

int lwl_app_event_handler(struct application *app, struct sys_event *event);

int lwl_app_mode_switch_to(u8 app_task);

int lwl_app_mode_switch_back();

u8 lwl_app_mode_exitting();

void lwl_app_mode_exit_ok();

void lwl_app_init(u8 init_mode);

void lwl_app_mode_loop();

#endif