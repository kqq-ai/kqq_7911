#include "lwl_app_core.h"
#include "os/os_api.h"
#include "syscfg_id.h"
#include "wifi/wifi_connect.h"

static lwl_app_context_t* s_app_context = NULL;
static OS_MUTEX g_mutex;

static volatile int g_event_scheduler_exit = 0;

static void lwl_simple_event_scheduler(void *priv)
{
    ASSERT(s_app_context != NULL);

    lwl_event_t event;
    while (!g_event_scheduler_exit) {
        my_blocking_queue_dequeue(&s_app_context->event_queue, &event);

        os_mutex_pend(&s_app_context->event_handlers_mutex, 0);
        for (int i = 0; i < LWL_EVENT_HANDLER_MAX_CNT; ++i) {
            if (s_app_context->event_handlers[i] != NULL) {
                s_app_context->event_handlers[i](&event);
            }
        }
        os_mutex_post(&s_app_context->event_handlers_mutex);
    }
    log_i("lwl_simple_event_scheduler thread exit.");
}

static void init_app_context(lwl_app_context_t* context)
{
    memset(s_app_context, 0, sizeof(lwl_app_context_t));

    int ret = syscfg_read(CFG_USER_MAC, s_app_context->mac, 6);
    if (ret < 0) {
        wifi_get_mac(s_app_context->mac);
        syscfg_write(CFG_USER_MAC, s_app_context->mac, 6);
    }
    log_i_buf(s_app_context->mac, 6);

    g_event_scheduler_exit = 0;

    s_app_context->dev_map = map_create();
    os_mutex_create(&s_app_context->dev_map_mutex);

    ASSERT(MYERROR_Success == my_blocking_queue_init(&s_app_context->event_queue, sizeof(lwl_event_t), LWL_EVENT_QUEUE_CAPACITY));
    os_mutex_create(&s_app_context->event_handlers_mutex);

    thread_fork("lwl_event_scheduler", 15, 512, 0, 0, lwl_simple_event_scheduler, NULL);
}

lwl_app_context_t* lwl_app_core_context_get_instance()
{
    if (s_app_context == NULL) { 
        os_mutex_pend(&g_mutex, 0);
        if (s_app_context == NULL) {
            s_app_context = (lwl_app_context_t *)malloc(sizeof(lwl_app_context_t));

            init_app_context(s_app_context);
        }
        os_mutex_post(&g_mutex);
    }
    return s_app_context;
}

void lwl_app_core_init()
{
    os_mutex_create(&g_mutex);
}

void lwl_app_context_free(lwl_app_context_t *ctx)
{
    if (!ctx) return;
    if (ctx->dev_map) {
        map_destroy(ctx->dev_map);
        ctx->dev_map = NULL;
    }
    pthread_mutex_destroy(&ctx->dev_map_mutex);
    pthread_mutex_destroy(&ctx->event_handlers_mutex);
    my_blocking_queue_destroy(&ctx->event_queue);
    free(ctx);
}

void lwl_app_core_destroy()
{
    g_event_scheduler_exit = 1; // 通知事件线程退出
    msleep(20); // 等待线程安全退出（可根据实际情况调整等待时间）
    if (s_app_context != NULL) {
        lwl_app_context_free(s_app_context);
        s_app_context = NULL;
    }
    os_mutex_del(&g_mutex, 1);
}

/**
 * @brief 添加新的E端设备信息到设备信息表中
 * @param [in] sn 序列号
 * @return 设备对象引用
 */
lwl_eend_dev_t *add_new_eend_dev(u8 *sn)
{
    ASSERT(s_app_context != NULL);
    ASSERT(strlen(sn) == SN_SIZE - 1);

    lwl_eend_dev_t *pdev = (lwl_eend_dev_t *)malloc(sizeof(lwl_eend_dev_t));
    ASSERT(pdev != NULL);
    lwl_eend_dev_init(sn, pdev);
    
    ASSERT(map_insert(s_app_context->dev_map, sn, pdev) == true);
    return pdev;
}

/**
 * @brief 从已有的E端设备信息表中查找序列号匹配的设备对象引用
 * 
 * @param [in] sn 序列号
 * @return 设备对象引用，如果未找到返回 NULL
 */
lwl_eend_dev_t* find_eend_dev_by_sn(u8 *sn)
{
    ASSERT(s_app_context != NULL);
    return (lwl_eend_dev_t *)map_find(s_app_context->dev_map, sn);
}

/**
 * @brief 遍历所有E端设备
 * 
 * @param [in] callback 回调函数
 * @param [in] priv 回调函数私有数据
 * @note 回调函数的参数为设备对象引用和私有数据
 */
void eend_dev_traverse(void (*callback)(lwl_eend_dev_t *, void *), void *priv)
{
    ASSERT(s_app_context != NULL);

    MapIterator it = map_begin(s_app_context->dev_map); // 持有锁
    MapIterator end = map_end(s_app_context->dev_map);

    for (; !map_iterator_equal(it, end); map_iterator_next(&it)) {
        callback((lwl_eend_dev_t *)map_iterator_value(it), priv);
    }
    
    map_iterator_unlock(it); // 释放锁
}

/**
 * @brief 清空并释放所有E端设备对象
 */
void clear_all_eend_dev(void)
{
    ASSERT(s_app_context != NULL);

    os_mutex_pend(&s_app_context->dev_map_mutex, 0);

    MapIterator it = map_begin(s_app_context->dev_map);
    MapIterator end = map_end(s_app_context->dev_map);

    // 先收集所有key，防止遍历过程中修改map导致崩溃
    int dev_count = map_size(s_app_context->dev_map);
    char **sn_list = malloc(dev_count * sizeof(char *));
    int idx = 0;
    for (; !map_iterator_equal(it, end); map_iterator_next(&it)) {
        char *sn = (char *)map_iterator_key(it);
        sn_list[idx] = strdup(sn);
        idx++;

        lwl_eend_dev_t *pdev = (lwl_eend_dev_t *)map_iterator_value(it);
        bka_eend_dev_handshake_info_t *pdev_info = &pdev->handshake_info;
        pdev_info->alive = 0;
    }
    map_iterator_unlock(it);
    // 删除并释放所有pdev
    for (int i = 0; i < idx; ++i) {
        lwl_eend_dev_t *pdev = (lwl_eend_dev_t *)map_find(s_app_context->dev_map, sn_list[i]);
        if (pdev) {
            map_erase(s_app_context->dev_map, sn_list[i]);
            free(pdev);
        }
        free(sn_list[i]);
    }
    free(sn_list);

    os_mutex_post(&s_app_context->dev_map_mutex);
}

void dump_all_eend_info()
{
    MapIterator it = map_begin(s_app_context->dev_map); // 持有锁
    MapIterator end = map_end(s_app_context->dev_map);

    /**
     * 
     */

    //log_i("+-----------------+-----+---+-------------------+------+-----------+-----+");
    //log_i("|       sn        | bat | P |     bound_mac     | rssi |  distance | num |");
    //log_i("+-----------------+-----+---+-------------------+------+-----------+-----+");

    for (; !map_iterator_equal(it, end); map_iterator_next(&it)) {
        lwl_eend_dev_t *pdev = (lwl_eend_dev_t *)map_iterator_value(it);

        //log_i("| %15.15s | %3d | %1d | %02X:%02X:%02X:%02X:%02X:%02X | %4d | %9s | %3d |", pdev->sn, 
        //    pdev->bat_level, pdev->power_on_state, 
        //    pdev->bound_mac[0], pdev->bound_mac[1], pdev->bound_mac[2], 
        //    pdev->bound_mac[3], pdev->bound_mac[4], pdev->bound_mac[5],
        //    pdev->distance_info.rssi, distance_type_to_text(pdev->distance_info.distance_type),pdev->distance_info.rssi_sample_count);
    }
    //log_i("+-----------------+-----+---+-------------------+------+-----------+------+");

    
    map_iterator_unlock(it); // 释放锁
}

/**
 * @brief 查找信号最强的E端设备
 * @return 信号最强的设备对象引用，如果未找到返回 NULL
 */
lwl_eend_dev_t *find_eend_dev_closest(void)
{
    ASSERT(s_app_context != NULL);
    
    lwl_eend_dev_t *result = NULL;
    s8 rssi = -128;

    MapIterator it = map_begin(s_app_context->dev_map); // 持有锁
    MapIterator end = map_end(s_app_context->dev_map);

    for (; !map_iterator_equal(it, end); map_iterator_next(&it)) {
        lwl_eend_dev_t *dev = (lwl_eend_dev_t *)map_iterator_value(it);
        if (rssi < dev->distance_info.rssi) {
            rssi = dev->distance_info.rssi;
            result = dev;
        }
    }
    map_iterator_unlock(it); // 释放锁

    return result;
}

/**
 * @brief 获取2米范围内的E端设备数量
 * @return 2米范围内的E端设备数量
 */
int get_2m_edev_count(void)
{
    ASSERT(s_app_context != NULL);
    
    int count = 0;
    MapIterator it = map_begin(s_app_context->dev_map); // 持有锁
    MapIterator end = map_end(s_app_context->dev_map);

    for (; !map_iterator_equal(it, end); map_iterator_next(&it)) {
        lwl_eend_dev_t *dev = (lwl_eend_dev_t *)map_iterator_value(it);

        if (dev->distance_info.distance_type == DISTANCE_0) {
            count++;
        }
    }
    map_iterator_unlock(it); // 释放锁
    return count;
}

/**
 * @brief 获取5米范围内的E端设备数量
 * @return 5米范围内的E端设备数量
 */
int get_5m_edev_count()
{
    ASSERT(s_app_context != NULL);
    
    int count = 0;
    MapIterator it = map_begin(s_app_context->dev_map); // 持有锁
    MapIterator end = map_end(s_app_context->dev_map);

    for (; !map_iterator_equal(it, end); map_iterator_next(&it)) {
        lwl_eend_dev_t *dev = (lwl_eend_dev_t *)map_iterator_value(it);

        if (dev->distance_info.distance_type == DISTANCE_0 ||
            dev->distance_info.distance_type == DISTANCE_1) {
            count++;
        }
    }
    map_iterator_unlock(it); // 释放锁
    return count;
}

/**
 * @brief 获取5米范围内的E端设备序列号列表
 * @param [out] targets 序列号列表
 * @param [out] targets_num 序列号数量
 */
void make_5m_edev_targets(char ***targets, int *targets_num)
{
    ASSERT(s_app_context != NULL);

    os_mutex_pend(&s_app_context->dev_map_mutex, 0);
    
    char **target_list = NULL;
    int num = get_5m_edev_count();

    if (num == 0) {
        *targets = NULL;
        *targets_num = 0;
        os_mutex_post(&s_app_context->dev_map_mutex);
        return;
    }

    target_list = malloc(num * sizeof(char *));

    int i = 0;

    MapIterator it = map_begin(s_app_context->dev_map); // 持有锁
    MapIterator end = map_end(s_app_context->dev_map);

    for (; !map_iterator_equal(it, end); map_iterator_next(&it)) {
        char *sn = (char *)map_iterator_key(it);
        lwl_eend_dev_t *dev = (lwl_eend_dev_t *)map_iterator_value(it);

        if (dev->distance_info.distance_type == DISTANCE_0 || dev->distance_info.distance_type == DISTANCE_1) {
            target_list[i] = malloc(strlen(sn) + 1);
            strcpy(target_list[i], sn);
            i++;
        }
    }
    ASSERT(i == num);

    *targets = target_list;
    *targets_num = num;
    
    map_iterator_unlock(it); // 释放锁

    os_mutex_post(&s_app_context->dev_map_mutex);
}

/**
 * @brief 释放5米范围内的E端设备序列号列表
 * @param [in] targets 序列号列表
 * @param [in] targets_num 序列号数量
 */
void free_5m_edev_targets(char **targets, int targets_num)
{
    os_mutex_pend(&s_app_context->dev_map_mutex, 0);
    for (int i = 0; i < targets_num; i++) {
        free(targets[i]);
    }
    free(targets);
    os_mutex_post(&s_app_context->dev_map_mutex);
}

/**
 * @brief 添加lwl事件处理回调
 * 
 * @param [in] handler 回调函数引用
 */
void lwl_event_handler_add(lwl_event_handler_cb_t *handler) 
{
    ASSERT(s_app_context != NULL);

    os_mutex_pend(&s_app_context->event_handlers_mutex, 0);
    int i;
    for (i = 0; i < LWL_EVENT_HANDLER_MAX_CNT; ++i) {
        if (s_app_context->event_handlers[i] == NULL) {
            break;
        }
    }
    if (i == LWL_EVENT_HANDLER_MAX_CNT) {
        log_e("[LWL]Failed to add event_handler.");
    } else {
        s_app_context->event_handlers[i] = handler;
    }
    os_mutex_post(&s_app_context->event_handlers_mutex);
}

/**
 * @brief 移除lwl事件处理回调
 * 
 * @param [in] handler 回调函数引用
 */
void lwl_event_handler_remove(lwl_event_handler_cb_t *handler)
{
    ASSERT(s_app_context != NULL);

    os_mutex_pend(&s_app_context->event_handlers_mutex, 0);
    int i;
    for (i = 0; i < LWL_EVENT_HANDLER_MAX_CNT; ++i) {
        if (s_app_context->event_handlers[i] == handler) {
            break;
        }
    }
    if (i == LWL_EVENT_HANDLER_MAX_CNT) {
        log_e("[LWL]Failed to remove event_handler.");
    } else {
        s_app_context->event_handlers[i] = NULL;
    }
    os_mutex_post(&s_app_context->event_handlers_mutex);
}

/**
 * @brief 发送事件通知
 * 
 * 将事件拷贝一份存入事件队列
 * 
 * @param [in] event 事件对象引用
 */
void lwl_event_notify(lwl_event_t *event) 
{
    ASSERT(s_app_context != NULL);
    my_blocking_queue_enqueue(&s_app_context->event_queue, event);
}

