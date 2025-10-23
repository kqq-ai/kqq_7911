#include "wss_device.h"
#include "wss_request.h"
#include "cpu.h"
#include "app_lingwl.h"

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

static u32 g_curr_time = 0;
static u32 last_dequeue_time = 0;
#define TTS_BIND_TIMEOUT_MS   (1 * 1000) // 1s
static void set_led_device_call(void)    { uart_send_set_led_color(COLOR_GREEN); }   //请求音频灯光变绿色

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
        edev_set[edev_set_count].last_get_time = g_curr_time;
        edev_set[edev_set_count].edev = pdev;
        edev_set_count++;
    }
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
    for (int i = 0; i < edev_set_count; i++) {
        if (edev_set[i].edev->distance_info.distance < 2.0f) {
            if (edev_set[min_index].last_get_time  > edev_set[i].last_get_time) {
                min_index = i;
            }
        }
    }
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
    for (int i = 0; i < edev_set_count; ++i) {
        if (strncmp(edev_set[i].sn, sn, SN_SIZE) == 0) {
            edev_set[i].last_get_time = g_curr_time;
            break;
        }
    }
    os_mutex_post(&edev_manager_mutex);
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
        break;
    case LWL_EVENT_TYPE_EEND_DEV_DISTANCE_CHANGE:
        char sn_string[SN_SIZE * 2 + 1] = "(NULL)";
        p_eend_dev->get_string(p_eend_dev, sn_string, sizeof(sn_string));
        log_i("[MEASDIST] LWL_EVENT_TYPE_EEND_DEV_DISTANCE_CHANGE %s: %d\r\n", sn_string, p_eend_dev->distance_info.distance_type);
        if (!p_eend_dev || p_eend_dev->distance_info.rssi_sample_count == 0) break;
        switch (p_eend_dev->distance_info.distance_type) {
        case DISTANCE_0:
            log_d("[MEASDIST] distance < 2m : do things1 here --- %s\r\n", sn_string);
            add_edev(p_eend_dev);
            add_target_sn(p_eend_dev->sn);
            break;
        case DISTANCE_1:
            log_d("[MEASDIST] distance > 2m  < 5m do things2 here ---%s\r\n", sn_string);
            // remove_from_edev_set(p_eend_dev);
            add_edev(p_eend_dev);
            add_target_sn(p_eend_dev->sn);
            break;
        case DISTANCE_2:
            log_d("[MEASDIST] distance > 5m do things3 here ---%s\r\n", sn_string);
            remove_edev(p_eend_dev);
            remove_target_sn(p_eend_dev->sn);
            break;
        case DISTANCE_3:
            log_d("[MEASDIST] not found ---%s\r\n", sn_string);
            remove_edev(p_eend_dev);
            remove_target_sn(p_eend_dev->sn);
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
    ble_simple_client_module_enable(1);

    os_mutex_create(&edev_manager_mutex);

    /* 注册lwl事件处理回调 */
    lwl_event_handler_add(device_event_handler_cb);

    // 初始化集合和队列
    clear_edev_set();
    clear_target_sn_set();
    edev_last = NULL;
}

void wss_device_uninit()
{
    log_i("Uninit WSS device\n");
    ble_simple_client_module_enable(0);

    clear_target_sn_set();
    /* 取消注册lwl事件处理回调 */
    lwl_event_handler_remove(device_event_handler_cb);
    msleep(10);
    os_mutex_del(&edev_manager_mutex, 1);
}

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
        edev_last = pdev;
        wss_request_send_event(pdev, ORMET_NEAR_TRIGGER);
        set_led_device_call();
        last_dequeue_time = g_curr_time;
        return pdev;
    }
    return NULL;
}

bool wss_device_call()
{
    g_curr_time = timer_get_ms();
#ifdef C_SPEAKLER
    char* pdev = "KPCNC5J26AEEGJ8";//小智
#else
    lwl_eend_dev_t *pdev = get_edev();
#endif
    if (pdev) {
        edev_last = pdev;
        wss_request_send_event(pdev, ORMET_NEAR_TRIGGER);
        set_led_device_call();
        last_dequeue_time = g_curr_time;
    }
    return true;
}

bool wss_device_is_calling()
{
    if (!edev_last) return false;
    return edev_last->distance_info.distance < 2.0f;
}