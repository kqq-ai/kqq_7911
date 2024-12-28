#ifndef __BLUE_DISTANCE_H__
#define __BLUE_DISTANCE_H__

#include "string.h"
#include "cpu.h"

// 三个距离的宏定义
#define BLUE_DISTANCE_HALF_METER (-40)
#define BLUE_DISTANCE_THREE_METER (-52)

#define ADV_MAX_LEN 16
#define MAX_SAMPLES 12
#define MAX_BLE_NUM 10

#define NOTIFY_APP_TIME 1000 // 10s

#define NOTIFY_SELF_TIME 5000 // 0.5S

enum distance_type {
    DISTANCE_0, // 小于0.5米
    DISTANCE_1, // 0.5米到3米
    DISTANCE_2, // 大于3米
};

typedef struct
{
    int ip_id;
    int session_id;
    char adv_name[ADV_MAX_LEN];
    u16 adv_name_len;
    s8 adv_rssi;
    int sample_count;
    s8 rssi_samples[MAX_SAMPLES];
    s8 adv_test_max_size;
    s8 functions_rssi;
    s8 average_rssi;
} disatance_info_t;

typedef struct {
    void (*start_scan)(void *rssi_cb_priv, 
        void (*rssi_cb)(void *priv, char *dev_name, u16 dev_name_len, s8 rssi), void* priv, void* cb);
    void (*stop_scan)(void);
} monitor_ble_ops;

// 开始检测RSSI
void monitor_init(void *priv);

void monitor_start(void *priv);

void monitor_stop(void *priv);

void monitor_register_ble_ops(monitor_ble_ops ops);

void monitor_register_device(char *adv_name, u16 adv_name_len);

void app_function(disatance_info_t *arg);
void self_function(disatance_info_t *arg, int distance_type);
#endif