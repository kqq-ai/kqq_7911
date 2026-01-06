/**
 * ================================================================================
 * WSS设备管理模块 (WSS Device Manager Module)
 * ================================================================================
 * 
 * 【模块功能概述】
 * 本模块主要负责WSS(Wireless Socket Server)连接模式下的设备管理，包括：
 * 1. E端BLE设备的动态检测和管理
 * 2. 基于距离信息的目标设备筛选和优化
 * 3. 设备事件的监听和响应
 * 4. 主动和被动的设备唤起机制
 * 5. 维护target_sn_set，和target_sn_count变量，管理目标设备SN集合
 *
 * 【实现逻辑】
 * 1. 设备集合管理：
 *    - 维护一个大小为16的E端设备集合(edev_set)，存储距离在2.5m以内的设备
 *    - 设备通过距离分级(DISTANCE_0/1/2/3)自动加入或移除集合
 *    - 使用互斥锁(edev_manager_mutex)保护集合的并发访问
 *
 * 2. 设备选择策略：
 *    - 优先选择距离最近的设备(distance < 2.0m)
 *    - 根据RSSI样本数量验证距离有效性
 *    - 移除距离超过2.5m或RSSI无效的设备(当前使用设备例外)
 *    - 列表中只保留两个最近的设备以优化选择效率
 *
 * 3. 事件驱动：
 *    - 监听BLE距离变化事件(LWL_EVENT_TYPE_EEND_DEV_DISTANCE_CHANGE)
 *    - 在事件回调中自动更新设备集合
 *    - 支持注册/注销事件处理回调
 *
 * 4. 触发机制：
 *    - 被动触发：距离<2m且1秒内首次检测到时自动触发ORMET_NEAR_TRIGGER事件
 *    - 主动触发：通过wss_device_call()函数显式请求最近的设备
 *    - 防抖处理：1秒超时(TTS_BIND_TIMEOUT_MS)防止频繁触发
 *
 * ================================================================================
 * 【外部接口依赖】
 * ================================================================================
 */

/**
 * wss_device_init()
 *   功能：初始化WSS设备管理模块
 *   - 启用BLE简单客户端模块
 *   - 创建和初始化互斥锁
 *   - 清空设备集合和目标SN集合
 *   - 注册设备事件监听回调(device_event_handler_cb)
 *   调用层：应用主程序启动阶段
 *
 * wss_device_uninit()
 *   功能：反初始化WSS设备管理模块
 *   - 注销事件监听回调
 *   - 禁用BLE简单客户端模块
 *   - 清空目标SN集合
 *   调用层：应用关闭或模式切换
 *
 * wss_device_loop()
 *   功能：设备循环检测处理(应在主循环中周期调用)
 *   返回值：返回距离<2m的最近设备指针，否则返回NULL
 *   行为：
 *     - 防抖：1秒内只选择一次新设备
 *     - 自动发送ORMET_NEAR_TRIGGER事件到目标设备
 *   调用层：应用主循环或定时任务
 *
 * wss_device_call()
 *   功能：主动请求与最近设备进行通话/交互
 *   返回值：返回成功发送请求的设备指针，否则返回NULL
 *   行为：
 *     - 获取当前距离最近的设备
 *     - 发送ORMET_NEAR_TRIGGER事件
 *     - 更新防抖计时器
 *   调用层：用户交互处理或特殊业务逻辑
 *
 * wss_device_is_calling()
 *   功能：判断最近唤起的设备是否仍在通话范围内(2.5m)
 *   返回值：true=在范围内，false=超出范围或未选中设备
 *   调用层：通话过程中的范围检测
 */

/**
 * ================================================================================
 * 【关键依赖头文件说明】
 * ================================================================================
 */

/**
 * wss_request.h
 *   接口：wss_request_send_event(char* sn, u32 event_type)
 *   功能：向指定设备发送WSS请求事件
 *   参数：sn - 目标设备SN，event_type - 事件类型(如ORMET_NEAR_TRIGGER)
 *
 * bt_ble/client/ble_distance.h
 *   接口：ble_simple_client_module_enable(int enable)
 *   功能：启用/禁用BLE简单客户端模块的距离检测
 *   参数：enable - 1启用，0禁用
 *
 * app_mode/lwl_app_core.h
 *   接口：lwl_app_core_context_get_instance()
 *   功能：获取全局应用上下文指针
 *   返回：lwl_app_context_t* - 应用上下文结构体
 *
 * app_cookie.h
 *   接口：
 *     - add_target_sn(char* sn) - 将设备SN加入目标监控列表
 *     - clear_target_sn_set() - 清空目标SN集合
 *     - find_eend_dev_by_sn(char* sn) - 根据SN查找E端设备结构体指针
 *   功能：设备SN和E端设备的查询/管理
 *
 * lwl_app_framework.h
 *   接口：
 *     - lwl_event_handler_add(void* callback) - 注册事件处理回调
 *     - lwl_event_handler_remove(void* callback) - 注销事件处理回调
 *   事件定义：lwl_event_t结构体，包含事件类型和距离信息
 *            LWL_EVENT_TYPE_EEND_DEV_DISTANCE_CHANGE - 距离变化事件
 *            DISTANCE_0/1/2/3 - 距离分级枚举
 *
 * device.h
 *   结构体：lwl_eend_dev_t - E端设备结构体
 *   包含字段：
 *     - sn[SN_SIZE+1] - 设备序列号
 *     - distance_info.distance - 当前距离值(米)
 *     - distance_info.distance_type - 距离分级(DISTANCE_0/1/2/3)
 *     - distance_info.rssi_sample_count - RSSI样本数量(用于距离有效性验证)
 *
 * timer.h
 *   接口：timer_get_ms()
 *   功能：获取系统毫秒时间戳
 *   返回：u32 - 毫秒时间戳
 *
 * os/os_api.h
 *   接口：
 *     - os_mutex_create(OS_MUTEX* mutex) - 创建互斥锁
 *     - os_mutex_pend(OS_MUTEX* mutex, u32 timeout) - 获取互斥锁
 *     - os_mutex_post(OS_MUTEX* mutex) - 释放互斥锁
 *     - os_mutex_valid(OS_MUTEX* mutex) - 检查互斥锁是否有效
 *   功能：RTOS级互斥保护
 *
 * log/log_mqtt.h
 *   接口：log_mqtt(...) - MQTT日志输出宏(被重定义为log_i)
 *   用途：调试和监控日志，通过MQTT可实时查看
 *
 * ================================================================================
 * 【全局变量与常量】
 * ================================================================================
 * 
 * g_curr_time: 当前系统时间戳(毫秒)，每次loop更新
 * last_dequeue_time: 上一次选中设备的时间戳，用于防抖
 * edev_last: 上一次返回的设备指针，防止重复处理
 * edev_set[MAX_EDEV_SIZE]: E端设备集合，最多支持16个设备
 * edev_set_count: 集合中当前有效设备个数
 * edev_manager_mutex: 设备集合的并发访问互斥锁
 * 
 * TTS_BIND_TIMEOUT_MS: 防抖时间门限，1秒内同一设备不重复触发
 * MAX_EDEV_SIZE: 设备集合最大容量(16个)
 * SN_SIZE: 设备序列号最大长度(定义在device.h或相关头文件)
 *
 * ================================================================================
 */

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


static u32 g_curr_time = 0;
static u32 last_dequeue_time = 0;
#define TTS_BIND_TIMEOUT_MS   (1 * 1000) // 1s
#define MAX_DIS 2.5f
// #define log_d log_i // Debug log macro

static lwl_app_context_t *g_app_ctx = NULL;
static lwl_eend_dev_t *edev_last;

typedef struct {
    char sn[SN_SIZE+1];
    u32 last_get_time;
    double distance;
    lwl_eend_dev_t *edev;
} edev_entry_t;

#define MAX_EDEV_SIZE 16

edev_entry_t edev_set[MAX_EDEV_SIZE] = {0};
static int edev_set_count = 0;
static OS_MUTEX edev_manager_mutex;




/**
 * 清空E端设备集合
 */
static void clear_edev_set()
{
    os_mutex_pend(&edev_manager_mutex, 0);
    memset(edev_set, 0, sizeof(edev_set));
    edev_set_count = 0;
    os_mutex_post(&edev_manager_mutex);
}

/**
 * 清理和更新集合中的设备信息(已持有互斥锁)
 * 功能：刷新距离信息，移除无效设备(不再获取锁)
 */
static void refresh_and_clean_edev_set(void)
{
    /* 第一步：刷新距离，移除无效设备(距离>MAX_DIS或RSSI无效) */
    for (int i = 0; i < edev_set_count; i++) {
        lwl_eend_dev_t *pdev = find_eend_dev_by_sn(edev_set[i].sn);
        
        if (!pdev || pdev->distance_info.distance > MAX_DIS || 
            pdev->distance_info.rssi_sample_count == 0) {
            if (edev_last == edev_set[i].edev && pdev->distance_info.rssi_sample_count == 0) {
                edev_last = NULL;
            }
            
            edev_set[i] = edev_set[edev_set_count - 1];
            edev_set[edev_set_count - 1].edev = NULL;
            edev_set_count--;
            i--;
            continue;
        }
        
        edev_set[i].edev = pdev;
        edev_set[i].distance = pdev->distance_info.distance;
    }
}


/**
 * 更新E设备最后使用时间
 * 参数：sn - 目标设备序列号
 */
bool update_edev_time(char* sn)
{
    if (!sn) {
        log_i("[update_edev_time] Invalid: sn is NULL");
        return false;
    }

    os_mutex_pend(&edev_manager_mutex, 0);
    
    /* 查找并更新目标设备 */
    refresh_and_clean_edev_set();
    int index = -1;
    for (int i = 0; i < edev_set_count; ++i) {
        if (strncmp(edev_set[i].sn, sn, SN_SIZE) == 0) {
            edev_set[i].last_get_time = g_curr_time;
            edev_last = edev_set[i].edev;
            index = i;
            break;
        }
    }

    if (index == -1) {
        log_i("[update_edev_time] Device sn=%s not found", sn);
        edev_last = NULL;
    }
    clear_target_sn_set();

    /* 第三步：优化集合大小，仅保留最近的2个 */
    int min_index = (index == 0) ? 1 : 0;
    for (int i = 0; i < edev_set_count; i++) {
        if (i != index && edev_set[i].distance < edev_set[min_index].distance) {
            min_index = i;
        }
    }
    if (edev_set_count > min_index) {
        add_target_sn(edev_set[min_index].sn);
    }

    os_mutex_post(&edev_manager_mutex);
    return index != -1;
}

/**
 * 初始化WSS设备管理模块
 * 操作：获取上下文→创建互斥锁→清空集合→启用BLE→注册回调
 */
void wss_device_init()
{
    log_i("[wss_device_init] Initializing...");

    g_app_ctx = lwl_app_core_context_get_instance();
    if (!g_app_ctx) {
        log_i("[wss_device_init] Warning: g_app_ctx is NULL");
    }

    if (!os_mutex_valid(&edev_manager_mutex)) {
        os_mutex_create(&edev_manager_mutex);
        log_i("[wss_device_init] Create mutex");
    }

    clear_edev_set();
    log_i("[wss_device_init] Clear edev_set");

    log_i("[wss_device_init] Clear target_sn_set");

    ble_simple_client_module_enable(1);
    log_i("[wss_device_init] Enable BLE module");

    // 移除device_event_handler_cb注册,不再使用基于距离类型的事件处理
    // lwl_event_handler_add(device_event_handler_cb);
    // log_i("[wss_device_init] Register event handler");

    edev_last = NULL;

    log_i("[wss_device_init] Done");
}

/**
 * 反初始化WSS设备管理模块
 * 操作：注销回调→禁用BLE→清空资源
 */
void wss_device_uninit()
{
    log_i("[wss_device_uninit] Uninitializing...");

    // 移除device_event_handler_cb注销,不再使用基于距离类型的事件处理
    // lwl_event_handler_remove(device_event_handler_cb);
    // log_i("[wss_device_uninit] Unregister event handler");

    ble_simple_client_module_enable(0);
    log_i("[wss_device_uninit] Disable BLE module");

    log_i("[wss_device_uninit] Clear target_sn_set");

    clear_edev_set();
    log_i("[wss_device_uninit] Clear edev_set");

    edev_last = NULL;

    msleep(10);

    log_i("[wss_device_uninit] Done");
}

/**
 * 查找距离<2m的最近设备
 * 返回值：最近设备指针，如果没有符合条件的设备则返回NULL
 */
static lwl_eend_dev_t *find_nearest_device_under_2m(void)
{
    if (!g_app_ctx || !g_app_ctx->dev_map) {
        return NULL;
    }

    lwl_eend_dev_t *result = NULL;
    double min_distance = 2.0f; // 只查找距离<2m的设备

    // 尝试获取互斥锁（超时时间0表示无限等待，但在实际应用中不应该阻塞太久）
    if (os_mutex_pend(&g_app_ctx->dev_map_mutex, 0) != 0) {
        log_i("[find_nearest_device_under_2m] Failed to acquire mutex");
        return NULL;
    }
    
    MapIterator it = map_begin(g_app_ctx->dev_map);
    MapIterator end = map_end(g_app_ctx->dev_map);

    for (; !map_iterator_equal(it, end); map_iterator_next(&it)) {
        lwl_eend_dev_t *dev = (lwl_eend_dev_t *)map_iterator_value(it);
        
        // 检查设备有效性：距离<2m，RSSI样本有效
        if (dev && dev->distance_info.rssi_sample_count > 0 && 
            dev->distance_info.distance < min_distance) {
            min_distance = dev->distance_info.distance;
            result = dev;
        }
    }
    map_iterator_unlock(it);
    os_mutex_post(&g_app_ctx->dev_map_mutex);

    return result;
}

/**
 * 设备循环检测处理(应在主循环中周期调用)
 * 返回值：返回距离<2m的最近设备指针，否则返回NULL
 * 行为：
 *   - 防抖：1秒内只选择一次新设备
 *   - 自动发送ORMET_NEAR_TRIGGER事件到目标设备
 */
lwl_eend_dev_t *wss_device_loop()
{
    g_curr_time = timer_get_ms();
    
    // 防抖检查：1秒内不重复触发
    if (g_curr_time - last_dequeue_time < TTS_BIND_TIMEOUT_MS) {
        return edev_last;
    }

    // 查找距离<2m的最近设备
    lwl_eend_dev_t *nearest_dev = find_nearest_device_under_2m();
    
    if (!nearest_dev) {
        // 如果没有找到新设备，检查当前设备是否仍然有效
        if (edev_last) {
            // 检查上下文是否有效
            if (!g_app_ctx || !g_app_ctx->dev_map) {
                edev_last = NULL;
                return NULL;
            }
            
            // 验证设备是否仍然存在于dev_map中
            lwl_eend_dev_t *pdev = find_eend_dev_by_sn((u8 *)edev_last->sn);
            if (!pdev || pdev != edev_last || 
                pdev->distance_info.distance >= 2.0f || 
                pdev->distance_info.rssi_sample_count == 0) {
                edev_last = NULL;
            }
        }
        return edev_last;
    }

    // 如果是新设备或设备已变化，发送触发事件
    if (edev_last != nearest_dev) {
        wss_request_send_event((char *)nearest_dev->sn, ORMET_NEAR_TRIGGER);
        log_i("[wss_device_loop] Send ORMET_NEAR_TRIGGER to device %s, distance=%.2fm", 
              (char *)nearest_dev->sn, nearest_dev->distance_info.distance);
        
        edev_last = nearest_dev;
        last_dequeue_time = g_curr_time;
    }

    return edev_last;
}

/**
 * 主动请求与最近设备进行通话/交互
 * 返回值：返回成功发送请求的设备指针，否则返回NULL
 * 行为：
 *   - 获取当前距离最近的设备（距离<2m）
 *   - 发送ORMET_NEAR_TRIGGER事件
 *   - 更新防抖计时器
 */
lwl_eend_dev_t *wss_device_call()
{
    g_curr_time = timer_get_ms();
    
    // 查找距离<2m的最近设备
    lwl_eend_dev_t *nearest_dev = find_nearest_device_under_2m();
    
    if (!nearest_dev) {
        log_i("[wss_device_call] No device found within 2m");
        return NULL;
    }

    // 发送触发事件
    wss_request_send_event((char *)nearest_dev->sn, ORMET_NEAR_TRIGGER);
    log_i("[wss_device_call] Send ORMET_NEAR_TRIGGER to device %s, distance=%.2fm", 
          (char *)nearest_dev->sn, nearest_dev->distance_info.distance);
    
    edev_last = nearest_dev;
    last_dequeue_time = g_curr_time;

    return nearest_dev;
}

/**
 * 判断最近唤起的设备是否仍在通话范围内(2.5m)
 * 返回值：true=在范围内，false=超出范围或未选中设备
 */
bool wss_device_is_calling()
{
    if (!edev_last) {
        return false;
    }

    // 检查上下文是否有效
    if (!g_app_ctx || !g_app_ctx->dev_map) {
        edev_last = NULL;
        return false;
    }

    // 查找当前设备信息，验证设备是否仍然存在
    lwl_eend_dev_t *pdev = find_eend_dev_by_sn((u8 *)edev_last->sn);
    
    if (!pdev || pdev != edev_last) {
        // 设备不存在或指针不匹配（设备可能已被释放和重新分配）
        edev_last = NULL;
        return false;
    }

    // 检查设备是否在范围内：距离<2.5m且RSSI有效
    if (pdev->distance_info.distance > MAX_DIS || 
        pdev->distance_info.rssi_sample_count == 0) {
        edev_last = NULL;
        return false;
    }

    return true;
}

