#ifndef __BLUE_DISTANCE_H__
#define __BLUE_DISTANCE_H__

#include "string.h"
#include "cpu.h"
#include "app_mode/lwl_app_core.h"
#include "app_mode/lwl_eend_device.h"

// 三个距离的宏定义
#define BLUE_DISTANCE_TWO_METER (-60)
#define BLUE_DISTANCE_FIVE_METER (-85)

#define NOTIFY_APP_TIME 5000 // 5s
#define NOTIFY_SELF_TIME 10000 // 10S

enum distance_type {
    DISTANCE_0, // 小于2米
    DISTANCE_1, // 2米到5米
    DISTANCE_2, // 大于5米
    DISTANCE_3, // RSSI等于-128或没有检测到
};

// 开始检测RSSI
void monitor_init(void);

void monitor_start(void);

void monitor_stop(void);

void app_function(lwl_eend_dev_t *arg);
void self_function(lwl_eend_dev_t *arg, int distance_type);
#endif