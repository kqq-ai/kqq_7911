
#include "wss_device.h"
#include "wss_request.h"
#include "wss_app_util.h"
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
#include <string.h>
#include <stdlib.h>


static u32 g_curr_time = 0;
static u32 last_dequeue_time = 0;
#define TTS_BIND_TIMEOUT_MS   (1 * 1000) // 1s
#define MAX_DIS 2.5f
// #define log_d log_i // Debug log macro

static lwl_app_context_t *g_app_ctx = NULL;

/**
 * 验证E设备是否有效
 * 参数：sn - 目标设备序列号
 * 返回：true - 设备存在且有效，false - 设备不存在或无效
 */
bool update_edev_time(char* sn)
{
    if (!sn) {
        log_i("[update_edev_time] Invalid: sn is NULL");
        return false;
    }

    // 直接查找设备并验证有效性
    lwl_eend_dev_t *pdev = find_eend_dev_by_sn(sn);
    if (!pdev) {
        log_i("[update_edev_time] Device sn=%s not found", sn);
        return false;
    }

    // 验证设备距离和RSSI是否有效
    if (pdev->distance_info.distance > MAX_DIS || 
        pdev->distance_info.rssi_sample_count == 0) {
        log_i("[update_edev_time] Device sn=%s invalid: distance=%.2f, rssi_count=%d", 
              sn, pdev->distance_info.distance, pdev->distance_info.rssi_sample_count);
        return false;
    }

    return true;
}

/**
 * 初始化WSS设备管理模块
 * 操作：获取上下文→启用BLE
 */
void wss_device_init()
{
    log_i("[wss_device_init] Initializing...");

    g_app_ctx = lwl_app_core_context_get_instance();
    if (!g_app_ctx) {
        log_i("[wss_device_init] Warning: g_app_ctx is NULL");
    }

    log_i("[wss_device_init] Clear target_sn_set");

    ble_simple_client_module_enable(1);
    log_i("[wss_device_init] Enable BLE module");

    log_i("[wss_device_init] Done");
}

/**
 * 反初始化WSS设备管理模块
 * 操作：禁用BLE→清空资源
 */
void wss_device_uninit()
{
    log_i("[wss_device_uninit] Uninitializing...");

    ble_simple_client_module_enable(0);
    log_i("[wss_device_uninit] Disable BLE module");

    log_i("[wss_device_uninit] Clear target_sn_set");

    msleep(10);

    log_i("[wss_device_uninit] Done");
}

//=================================================================================================
// nearby_update事件处理（每500ms发送一次附近设备信息）
//=================================================================================================
/**
 * @brief nearby_update事件处理回调函数
 * @param e 事件指针
 * @note 每500ms触发一次,收集所有扫描到的E端设备信息并通过WebSocket发送
 */
/**
 * @brief 检查设备是否有效（有RSSI样本）
 * @param dev 设备指针
 * @return true 设备有效，false 设备无效
 */
static inline bool is_valid_device(lwl_eend_dev_t *dev)
{
    return dev != NULL && dev->distance_info.rssi_sample_count > 0;
}

/**
 * @brief 将设备插入到已排序数组的正确位置（插入排序）
 * @param devices 设备指针数组
 * @param count 当前数组中的设备数量
 * @param dev 要插入的设备
 * @note 按距离从小到大排序，devices[0]为最近设备
 */
static void insert_device_sorted(void **devices, int count, lwl_eend_dev_t *dev)
{
    double dev_distance = dev->distance_info.distance;
    
    // 找到插入位置：找到第一个距离大于等于当前设备的位置
    // 如果所有设备距离都更小，则插入到最后（位置count）
    int insert_pos = count;
    for (int i = 0; i < count; i++) {
        lwl_eend_dev_t *existing_dev = (lwl_eend_dev_t *)devices[i];
        if (existing_dev->distance_info.distance >= dev_distance) {
            insert_pos = i;
            break;
        }
    }
    
    // 将插入位置之后的元素后移
    for (int i = count; i > insert_pos; i--) {
        devices[i] = devices[i - 1];
    }
    
    // 插入新设备
    devices[insert_pos] = (void *)dev;
}

static void nearby_update_event_handler(lwl_event_t *e)
{
    if (!e) {
        log_e("[nearby_update] ERROR: event is NULL");
        return;
    }

    // 检查事件类型
    if (e->event_type != LWL_EVENT_TYPE_NEARBY_UPDATE) {
        return;
    }

    // 获取应用上下文
    lwl_app_context_t *ctx = lwl_app_core_context_get_instance();
    if (!ctx || !ctx->dev_map) {
        log_e("[nearby_update] ERROR: context or dev_map is NULL");
        return;
    }

    // 第一次遍历:统计有效设备数量(有RSSI样本的设备)
    int valid_device_count = 0;
    MapIterator it = map_begin(ctx->dev_map);
    MapIterator end = map_end(ctx->dev_map);

    for (; !map_iterator_equal(it, end); map_iterator_next(&it)) {
        lwl_eend_dev_t *dev = (lwl_eend_dev_t *)map_iterator_value(it);
        if (is_valid_device(dev)) {
            valid_device_count++;
        }
    }
    map_iterator_unlock(it);

    // 如果没有有效设备,发送空列表
    if (valid_device_count == 0) {
        log_i("[nearby_update] No valid devices, sending empty list");
        send_nearby_update(NULL, 0);
        return;
    }

    // 分配设备指针数组
    void **devices = (void **)malloc(valid_device_count * sizeof(void *));
    if (!devices) {
        log_e("[nearby_update] ERROR: malloc failed for device array");
        return;
    }

    // 第二次遍历:收集有效设备并按距离排序（插入排序）
    // 使用插入排序的原因：设备数量通常较少(<10个)，插入排序在小规模数据上性能优秀
    // 且可以在收集过程中实时排序，代码简单易维护
    int device_count = 0;
    it = map_begin(ctx->dev_map);
    end = map_end(ctx->dev_map);

    for (; !map_iterator_equal(it, end); map_iterator_next(&it)) {
        lwl_eend_dev_t *dev = (lwl_eend_dev_t *)map_iterator_value(it);
        if (is_valid_device(dev)) {
            // 将设备插入到已排序数组的正确位置
            insert_device_sorted(devices, device_count, dev);
            device_count++;
        }
    }
    map_iterator_unlock(it);

    // 发送nearby_update消息
    int ret = send_nearby_update(devices, device_count);
    if (ret != 0) {
        log_e("[nearby_update] Failed to send nearby_update message");
    }

    // 释放设备指针数组
    free(devices);
}

/**
 * @brief 初始化nearby_update事件处理器
 */
void nearby_update_handler_init(void)
{
    log_i("[nearby_update] Initializing nearby_update handler...");
    lwl_event_handler_add(nearby_update_event_handler);
    log_i("[nearby_update] Handler registered");
}

/**
 * @brief 反初始化nearby_update事件处理器
 */
void nearby_update_handler_uninit(void)
{
    log_i("[nearby_update] Uninitializing nearby_update handler...");
    lwl_event_handler_remove(nearby_update_event_handler);
    log_i("[nearby_update] Handler unregistered");
}
