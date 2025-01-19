#ifndef __POWER_ON_H__
#define __POWER_ON_H__

#include "app_core.h"

int app_power_on_event_handler(struct application *app, struct sys_event *event);
void app_power_on_mode(void);


#endif