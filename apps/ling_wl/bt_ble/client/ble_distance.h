#ifndef __BLUE_DISTANCE_H__
#define __BLUE_DISTANCE_H__

#include "string.h"
#include "cpu.h"
#include "app_mode/lwl_eend_device.h"

// 三个距离的宏定义
#define BLUE_DISTANCE_TWO_METER (-60)
#define BLUE_DISTANCE_FIVE_METER (-85)

/*
    @distance_info_t : ble device information
    @distance_ble_type : ble device number
*/
#define ADV_MAX_LEN 16
#define MAX_SAMPLES 1000
#define MAX_BLE_NUM 10

#define NOTIFY_APP_TIME 2000 // 2s

#define NOTIFY_SELF_TIME 10000 // 10S

enum distance_type {
    DISTANCE_0, // 小于2米
    DISTANCE_1, // 2米到5米
    DISTANCE_2, // 大于5米
};

typedef struct
{
    int sample_count;
    s8 rssi_samples[MAX_SAMPLES];
    s8 functions_rssi;
    s8 average_rssi;
    lwl_eend_dev_t *p_eend_dev;
} disatance_info_t;

// 开始检测RSSI
void monitor_init(void);

void monitor_start(void);

void monitor_stop(void);

void monitor_register_device(lwl_eend_dev_t *p_eend_dev);

void app_function(disatance_info_t *arg);
void self_function(disatance_info_t *arg, int distance_type);
#endif