#ifndef __BLUE_DISTANCE_H__
#define __BLUE_DISTANCE_H__

#include "string.h"
#include "cpu.h"
#include "app_mode/lwl_app_core.h"
#include "app_mode/lwl_eend_device.h"

const char *distance_type_to_text(enum distance_type type);

// 开始检测RSSI
void monitor_init(void);

void monitor_start(void);

void monitor_stop(void);

void app_function(lwl_eend_dev_t *arg);
void self_function(lwl_eend_dev_t *arg, int distance_type);
#endif