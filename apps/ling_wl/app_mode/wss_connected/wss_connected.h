#ifndef __WSS_CONNECTED_H__
#define __WSS_CONNECTED_H__

#include "app_core.h"

int app_wss_conncted_event_handler(struct application *app, struct sys_event *event);
void app_wss_connected_mode(void);

#endif