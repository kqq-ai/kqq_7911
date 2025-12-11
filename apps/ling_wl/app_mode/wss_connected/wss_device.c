#include "wss_device.h"
#include "wss_request.h"
#include "cpu.h"
#include "app_cookie.h"

#include "../lwl_app_framework.h"
#include "util/cqueue.h"
#include "util/ringbuffer.h"
#include "bt_ble/client/ble_distance.h"
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

static u32 g_curr_time = 0;
static u32 last_dequeue_time = 0;
#define TTS_BIND_TIMEOUT_MS   (1 * 1000) // 1s

// #define log_d log_i // Debug log macro

static lwl_app_context_t *g_app_ctx = NULL;
static lwl_eend_dev_t *edev_last;

typedef struct {
    char sn[SN_SIZE];
    u32 last_get_time;
    double distance;
    lwl_eend_dev_t *edev;
} edev_entry_t;

#define MAX_EDEV_SIZE 16

edev_entry_t edev_set[MAX_EDEV_SIZE] = {0};
static int edev_set_count = 0;
static OS_MUTEX edev_manager_mutex;

// 添加E端设备到集合
static void add_edev(lwl_eend_dev_t *pdev)
{
    if (!pdev) return;
    os_mutex_pend(&edev_manager_mutex, 0);
    // 检查是否已存在
    for (int i = 0; i < edev_set_count; ++i) {
        if (strncmp(edev_set[i].sn, pdev->sn, SN_SIZE) == 0) {
            edev_set[i].edev = pdev;
            os_mutex_post(&edev_manager_mutex);
            return; // 已存在
        }
    }
    log_i("add_edev sn=%s", pdev->sn);
    if (edev_set_count >= MAX_EDEV_SIZE) {
        log_i("Edev set full, cannot add more.");
    } else {
        strncpy(edev_set[edev_set_count].sn, pdev->sn, SN_SIZE);
        edev_set[edev_set_count].last_get_time = 0;
        edev_set[edev_set_count].edev = pdev;
        edev_set_count++;
    }
    add_target_sn(pdev->sn);
    os_mutex_post(&edev_manager_mutex);
}

// 从集合中删除E端设备
static void remove_edev(lwl_eend_dev_t *pdev)
{
    if (!pdev) return;
    os_mutex_pend(&edev_manager_mutex, 0);
    for (int i = 0; i < edev_set_count; ++i) {
        if (strncmp(edev_set[i].sn, pdev->sn, SN_SIZE) == 0) {
            log_i("remove_edev sn=%s", pdev->sn);
            edev_set[i] = edev_set[edev_set_count - 1];
            edev_set[edev_set_count - 1].edev = NULL;
            edev_set_count--;
            break;
        }
    }
    remove_target_sn(pdev->sn);
    os_mutex_post(&edev_manager_mutex);
}

// 清空E端设备集合
static void clear_edev_set()
{
    os_mutex_pend(&edev_manager_mutex, 0);
    memset(edev_set, 0, sizeof(edev_set));
    edev_set_count = 0;
    os_mutex_post(&edev_manager_mutex);
}

// 从E端设备集合取最小值
static lwl_eend_dev_t* get_edev()
{
    os_mutex_pend(&edev_manager_mutex, 0);
    if (edev_set_count == 0) {
        os_mutex_post(&edev_manager_mutex);
        return NULL;
    }
    // 找到最距离最近的设备
    int min_index = 0;
    for (int i = 0; i < edev_set_count; i++) {
        lwl_eend_dev_t *pdev = find_eend_dev_by_sn(edev_set[i].sn);
        if (!pdev || pdev->distance_info.distance >3.5f) {
            if (edev_last == edev_set[i].edev) {
                edev_last = NULL;
            }
            remove_target_sn(edev_set[i].sn);
            edev_set[i] = edev_set[edev_set_count - 1];
            edev_set[edev_set_count - 1].edev = NULL;
            edev_set_count--;
            i--;
            continue;
        }
        edev_set[i].edev = pdev;
        edev_set[i].distance = pdev->distance_info.distance;
        if (edev_set[min_index].distance > edev_set[i].distance) {
            min_index = i;
        }
    }
    // 找到2m内最早更新的设备
    // for (int i = 0; i < edev_set_count; i++) {
    //     if (edev_set[i].edev->distance_info.distance < 2.0f) {
    //         if (edev_set[min_index].last_get_time  > edev_set[i].last_get_time) {
    //             min_index = i;
    //         }
    //     }
    // }
    lwl_eend_dev_t *pdev = NULL;
    if (edev_set_count > 0) {
        pdev = edev_set[min_index].edev;
    }
    os_mutex_post(&edev_manager_mutex);
    return pdev;
}

// 更新E设备使用时间
void update_edev_time(char* sn)
{
    if (!sn) return;
    os_mutex_pend(&edev_manager_mutex, 0);
    log_i("get_edev found pdev=%s from %d", sn, edev_set_count);
    edev_last = NULL;
    for (int i = 0; i < edev_set_count; ++i) {
        lwl_eend_dev_t *pdev = find_eend_dev_by_sn(edev_set[i].sn);
        if (!pdev || pdev->distance_info.distance >3.5f) {
            if (edev_last == edev_set[i].edev) {
                edev_last = NULL;
            }
            remove_target_sn(edev_set[i].sn);
            edev_set[i] = edev_set[edev_set_count - 1];
            edev_set[edev_set_count - 1].edev = NULL;
            edev_set_count--;
            i--;
            continue;
        }
        edev_set[i].edev = pdev;
        edev_set[i].distance = pdev->distance_info.distance;
        if (strncmp(edev_set[i].sn, sn, SN_SIZE) == 0) {
            edev_set[i].last_get_time = g_curr_time;
            edev_last = edev_set[i].edev;
        }
    }
    os_mutex_post(&edev_manager_mutex);
}

//=================================================================================================
// 处理回调中的E端设备距离事件，把距离在0和1的加入到集合中，距离在2和3的从集合中删除
//=================================================================================================
static void device_event_handler_cb(lwl_event_t *e)
{
    os_mutex_pend(&edev_manager_mutex, 0);
    switch (e->event_type) {
    case LWL_EVENT_TYPE_EEND_DEV_DISTANCE_CHANGE:
        lwl_eend_dev_t *p_eend_dev = e->distance_change.pdev;
        assert(p_eend_dev != NULL);
        if (p_eend_dev->distance_info.rssi_sample_count == 0) break;
        switch (p_eend_dev->distance_info.distance_type) {
        case DISTANCE_0:
        case DISTANCE_1:
            add_edev(p_eend_dev);
            break;
        case DISTANCE_2:
        case DISTANCE_3:
            remove_edev(p_eend_dev);
            break;
        default:
            break;
        }
        break;
    }
    os_mutex_post(&edev_manager_mutex);
}

// 初始化WSS设备模块
// 包括注册事件回调，初始化集合和队列等，开始检测蓝牙设备
void wss_device_init()
{
    log_i("Init WSS device\n");
    g_app_ctx = lwl_app_core_context_get_instance();
    ble_simple_client_module_enable(1);

    if (!os_mutex_valid(&edev_manager_mutex)) {
        os_mutex_create(&edev_manager_mutex);
    }
    // 初始化集合和队列
    clear_edev_set();
    clear_target_sn_set();

    /* 注册lwl事件处理回调 */
    lwl_event_handler_add(device_event_handler_cb);

    edev_last = NULL;
}

// 反初始化WSS设备模块
// 包括取消注册事件回调，释放集合和队列等资源，停止检测蓝牙设备
void wss_device_uninit()
{
    log_i("Uninit WSS device\n");
    /* 取消注册lwl事件处理回调 */
    lwl_event_handler_remove(device_event_handler_cb);
    
    ble_simple_client_module_enable(0);

    clear_target_sn_set();
    msleep(10);
}

// 设备循环处理函数
// 检测E端设备集合中距离最近的设备，如果距离小于2米，则发送请求事件并返回该设备指针
lwl_eend_dev_t *wss_device_loop()
{
    g_curr_time = timer_get_ms();

    // 1s内不再取新设备
    if (g_curr_time - last_dequeue_time < TTS_BIND_TIMEOUT_MS) {
        return NULL;
    }

    lwl_eend_dev_t *pdev = get_edev();
    if (pdev == edev_last) return NULL;
    if (pdev && pdev->distance_info.distance < 2.0f) {
        wss_request_send_event(pdev, ORMET_NEAR_TRIGGER);
        last_dequeue_time = g_curr_time;
        return pdev;
    }
    return NULL;
}

// 主动请求最近设备通话
lwl_eend_dev_t *wss_device_call()
{
    g_curr_time = timer_get_ms();
    lwl_eend_dev_t *pdev = get_edev();
    char* sn = NULL;
    if (pdev) sn = pdev->sn;
    if (sn) {
        wss_request_send_event(sn, ORMET_NEAR_TRIGGER);
        last_dequeue_time = g_curr_time;
        return pdev;
    }
    return NULL;
}

// 判断最近唤起的设备是否还在2m内
bool wss_device_is_calling()
{
    if (!edev_last) return false;
    return edev_last->distance_info.distance < 2.5f;
}