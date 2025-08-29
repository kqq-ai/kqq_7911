#include "wss_device.h"
#include "wss_request.h"
#include "cpu.h"

#include "../lwl_app_framework.h"
#include "util/cqueue.h"
#include "util/ringbuffer.h"
#include "bt_ble/client/ble_distance.h"
#include "async_msg_loop.h"
#include "app_mode/lwl_app_core.h"
#include "device.h"
#include "util/threadpool_tasks.h"

#include "ota/http_ota.h"
#include "uart.h"

#include "protocol/odm/odmp.h"

#include "protocol/cp/c_uart_p.h"

#include "timer.h" // 确保有获取毫秒时间的函数
#include "os/os_api.h"
#include "log/log_mqtt.h"

#define log_i log_mqtt

static u32 last_dequeue_time = 0;

// #define log_d log_i // Debug log macro

static lwl_app_context_t *g_app_ctx = NULL;
static lwl_eend_dev_t *g_edev_closest = NULL; // 服务器维护

static bool is_edev_far_away(lwl_eend_dev_t *pdev)
{
    return (pdev->distance_info.distance_type == DISTANCE_2) || (pdev->distance_info.distance_type == DISTANCE_3);
}

#define MAX_EDEV_SET 16
#define MAX_EDEV_QUEUE 16

// 集合：保存所有DISTANCE_0的设备
static lwl_eend_dev_t *edev_set[MAX_EDEV_SET] = {0};
static int edev_set_count = 0;

// 队列：保存所有最近变为DISTANCE_0的设备
static lwl_eend_dev_t *edev_queue[MAX_EDEV_QUEUE] = {0};
static int edev_queue_head = 0;
static int edev_queue_tail = 0;

static OS_MUTEX edev_set_mutex;
static OS_MUTEX edev_queue_mutex;

// 不加锁的内部版本，仅供 add_to_edev_set 内部调用
static int is_in_edev_set_nolock(lwl_eend_dev_t *pdev) {
    for (int i = 0; i < edev_set_count; ++i) {
        if (edev_set[i] == pdev) {
            return 1;
        }
    }
    return 0;
}

// 仅供外部调用的加锁版本
static int is_in_edev_set(lwl_eend_dev_t *pdev) {
    int found = 0;
    os_mutex_pend(&edev_set_mutex, 0);
    for (int i = 0; i < edev_set_count; ++i) {
        if (edev_set[i] == pdev) {
            found = 1;
            break;
        }
    }
    os_mutex_post(&edev_set_mutex);
    return found;
}

static void add_to_edev_set(lwl_eend_dev_t *pdev) {
    if (!pdev) return;
    os_mutex_pend(&edev_set_mutex, 0);
    if (is_in_edev_set_nolock(pdev) || edev_set_count >= MAX_EDEV_SET) {
        os_mutex_post(&edev_set_mutex);
        return;
    }
    edev_set[edev_set_count++] = pdev;
    os_mutex_post(&edev_set_mutex);
}

static void remove_from_edev_set(lwl_eend_dev_t *pdev) {
    os_mutex_pend(&edev_set_mutex, 0);
    for (int i = 0; i < edev_set_count; ++i) {
        if (edev_set[i] == pdev) {
            for (int j = i; j < edev_set_count - 1; ++j) {
                edev_set[j] = edev_set[j + 1];
            }
            edev_set[--edev_set_count] = NULL;
            break;
        }
    }
    os_mutex_post(&edev_set_mutex);
}

// 清空edev集合
static void clear_edev_set(void) {
    os_mutex_pend(&edev_set_mutex, 0);
    for (int i = 0; i < MAX_EDEV_SET; ++i) {
        edev_set[i] = NULL;
    }
    edev_set_count = 0;
    os_mutex_post(&edev_set_mutex);
}

// 打印edev队列所有设备的sn
static void print_edev_queue(void) {
    os_mutex_pend(&edev_queue_mutex, 0);
    int idx = edev_queue_head;
    log_i("[EDEV_QUEUE] head=%d tail=%d: ", edev_queue_head, edev_queue_tail);
    while (idx != edev_queue_tail) {
        if (edev_queue[idx]) {
            log_i("%s ", edev_queue[idx]->sn);
        }
        idx = (idx + 1) % MAX_EDEV_QUEUE;
    }
    log_i("\n");
    os_mutex_post(&edev_queue_mutex);
}

// 清空edev队列
static void clear_edev_queue(void) {
    os_mutex_pend(&edev_queue_mutex, 0);
    for (int i = 0; i < MAX_EDEV_QUEUE; ++i) {
        edev_queue[i] = NULL;
    }
    edev_queue_head = 0;
    edev_queue_tail = 0;
    os_mutex_post(&edev_queue_mutex);
}

// 队列操作（加锁版）
static int is_edev_queue_full() {
    int full;
    os_mutex_pend(&edev_queue_mutex, 0);
    full = ((edev_queue_tail + 1) % MAX_EDEV_QUEUE) == edev_queue_head;
    os_mutex_post(&edev_queue_mutex);
    return full;
}
static int is_edev_queue_empty() {
    int empty;
    os_mutex_pend(&edev_queue_mutex, 0);
    empty = (edev_queue_head == edev_queue_tail);
    os_mutex_post(&edev_queue_mutex);
    return empty;
}

// 判断设备是否已在队列中（加锁内部使用）
static int is_in_edev_queue_nolock(lwl_eend_dev_t *pdev) {
    int idx = edev_queue_head;
    while (idx != edev_queue_tail) {
        if (edev_queue[idx] == pdev) {
            return 1;
        }
        idx = (idx + 1) % MAX_EDEV_QUEUE;
    }
    return 0;
}

static void enqueue_edev(lwl_eend_dev_t *pdev) {
    if (!pdev) return;
    os_mutex_pend(&edev_queue_mutex, 0);
    if (is_in_edev_queue_nolock(pdev)) {
        os_mutex_post(&edev_queue_mutex);
        return; // 已存在，不重复入队
    }
    if (((edev_queue_tail + 1) % MAX_EDEV_QUEUE) == edev_queue_head) {
        os_mutex_post(&edev_queue_mutex);
        return; // 队列已满
    }
    edev_queue[edev_queue_tail] = pdev;
    edev_queue_tail = (edev_queue_tail + 1) % MAX_EDEV_QUEUE;
    os_mutex_post(&edev_queue_mutex);
}
static lwl_eend_dev_t* dequeue_edev() {
    lwl_eend_dev_t *pdev = NULL;
    if (is_edev_queue_empty()) {
        return NULL; // 队列为空
    }
    // print_edev_queue(); // 打印队列内容
    os_mutex_pend(&edev_queue_mutex, 0);
    if (edev_queue_head != edev_queue_tail) {
        pdev = edev_queue[edev_queue_head];
        edev_queue[edev_queue_head] = NULL;
        edev_queue_head = (edev_queue_head + 1) % MAX_EDEV_QUEUE;
    }
    os_mutex_post(&edev_queue_mutex);
    return pdev;
}

//=================================================================================================
// LWL event callback
//=================================================================================================
static void device_event_handler_cb(lwl_event_t *e)
{
    lwl_eend_dev_t *p_eend_dev = e->distance_change.pdev;

    switch (e->event_type) {
    case LWL_EVENT_TYPE_EEND_DEV_CLOSEST_CHANGE:
        log_i("LWL_EVENT_TYPE_EEND_DEV_CLOSEST_CHANGE");
        if (p_eend_dev && p_eend_dev->distance_info.distance_type == DISTANCE_0) {
            // if (!is_in_edev_set(p_eend_dev)) {
            //     add_to_edev_set(p_eend_dev);
            //     enqueue_edev(p_eend_dev);
            //     add_target_sn(p_eend_dev->sn);
            // } else {
            //     enqueue_edev(p_eend_dev);
            // }
        }
        break;
    case LWL_EVENT_TYPE_EEND_DEV_DISTANCE_CHANGE:
        char sn_string[SN_SIZE * 2 + 1] = "(NULL)";
        p_eend_dev->get_string(p_eend_dev, sn_string, sizeof(sn_string));
        log_i("[MEASDIST] %s: %d %d\r\n", sn_string, p_eend_dev->distance_info.rssi, p_eend_dev->distance_info.distance_type);
        log_i("LWL_EVENT_TYPE_EEND_DEV_DISTANCE_CHANGE");
        if (!p_eend_dev) break;
        switch (p_eend_dev->distance_info.distance_type) {
        case DISTANCE_0:
            log_d("[MEASDIST] distance < 2m : do things1 here --- %s\r\n", sn_string);
            if (!is_in_edev_set(p_eend_dev)) {
                add_to_edev_set(p_eend_dev);
                enqueue_edev(p_eend_dev);
                add_target_sn(p_eend_dev->sn);
            }
            break;
        case DISTANCE_1:
            log_d("[MEASDIST] distance > 2m  < 5m do things2 here ---%s\r\n", sn_string);
            // remove_from_edev_set(p_eend_dev);
            break;
        case DISTANCE_2:
            log_d("[MEASDIST] distance > 5m do things3 here ---%s\r\n", sn_string);
            remove_target_sn(p_eend_dev->sn);
            remove_from_edev_set(p_eend_dev);
            break;
        case DISTANCE_3:
            log_d("[MEASDIST] not found ---%s\r\n", sn_string);
            remove_target_sn(p_eend_dev->sn);
            remove_from_edev_set(p_eend_dev);
            break;
        default:
            break;
        }
        break;
    case LWL_EVENT_TYPE_EEND_DEV_NEW_IN:
        log_i("ORMET_NEW_DEVICE_IN");
        break;
    }
}

void wss_device_init()
{
    log_i("Init WSS device\n");
    g_app_ctx = lwl_app_core_context_get_instance();
    ble_simple_server_module_enable(0);
    ble_simple_client_module_enable(1);

    os_mutex_create(&edev_set_mutex);
    os_mutex_create(&edev_queue_mutex);

    /* 注册lwl事件处理回调 */
    lwl_event_handler_add(device_event_handler_cb);

    // 初始化集合和队列
    clear_edev_set();
    clear_edev_queue();
    clear_target_sn_set();
}

void wss_device_uninit()
{
    log_i("Uninit WSS device\n");

    clear_target_sn_set();
    /* 取消注册lwl事件处理回调 */
    lwl_event_handler_remove(device_event_handler_cb);
    os_mutex_del(&edev_set_mutex, 1);
    os_mutex_del(&edev_queue_mutex, 1);
}

lwl_eend_dev_t *wss_device_loop()
{
    u32 now = timer_get_ms();

    // 1s内不再取新设备
    if (now - last_dequeue_time < 1000) {
        return NULL;
    }

    lwl_eend_dev_t *pdev = dequeue_edev();
    if (pdev) {
        wss_request_send_event(pdev, ORMET_NEAR_TRIGGER);
        last_dequeue_time = now;
        return pdev;
    }
    return NULL;
}