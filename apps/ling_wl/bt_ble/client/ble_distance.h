#ifndef __BLUE_DISTANCE_H__
#define __BLUE_DISTANCE_H__

#include "string.h"
#include "cpu.h"
#include "app_mode/lwl_app_core.h"
#include "app_mode/lwl_eend_device.h"

// 三个距离的宏定义
#define BLUE_DISTANCE_TWO_METER (-60)
#define BLUE_DISTANCE_FIVE_METER (-85)

#define NOTIFY_APP_TIME 1000 // 1s
#define NOTIFY_SELF_TIME 10000 // 10S


#define PRINT_RSSI_LOG 0 // 是否打印RSSI日志

const char *distance_type_to_text(enum distance_type type);

// 开始检测RSSI
void monitor_init(void);

void monitor_start(void);

void monitor_stop(void);

void app_function(lwl_eend_dev_t *arg);
void self_function(lwl_eend_dev_t *arg, int distance_type);
#endif