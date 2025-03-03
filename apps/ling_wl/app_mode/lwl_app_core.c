#include "lwl_app_core.h"
#include "os/os_api.h"

static lwl_app_context_t* s_app_context = NULL;
static OS_MUTEX g_mutex;

static void lwl_simple_event_scheduler(void *priv)
{
    ASSERT(s_app_context != NULL);

    lwl_event_t event;
    while (true) {
        my_blocking_queue_dequeue(&s_app_context->event_queue, &event);
        
        os_mutex_pend(&g_mutex, 0);
        for (int i = 0; i < LWL_EVENT_HANDLER_MAX_CNT; ++i) {
            if (s_app_context->event_handlers[i] != NULL) {
                s_app_context->event_handlers[i](&event);
            }
        }
        os_mutex_post(&g_mutex);
    }
}

static void init_app_context(lwl_app_context_t* context)
{
    memset(s_app_context, 0, sizeof(lwl_app_context_t));
    s_app_context->active_eend_idx = -1;

    my_blocking_queue_init(&s_app_context->event_queue, sizeof(lwl_event_t), LWL_EVENT_QUEUE_CAPACITY);

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

void lwl_app_core_destroy()
{
    if (s_app_context != NULL) {
        free(s_app_context);
        s_app_context = NULL;
    }
    os_mutex_del(&g_mutex, 1);
}

/**
 * @brief 获取当前激活的设备对象引用
 * 
 * @return 设备对象引用，如果未找到返回 NULL
 */
lwl_eend_dev_t* find_eend_dev_active()
{
    ASSERT(s_app_context != NULL);
    os_mutex_pend(&g_mutex, 0);
    lwl_eend_dev_t *p = (s_app_context->active_eend_idx == -1) ? NULL : &s_app_context->eend_dev_list[s_app_context->active_eend_idx];
    os_mutex_post(&g_mutex);
    return p;
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
    os_mutex_pend(&g_mutex, 0);
    int i;
    for (i = 0; i < EEND_MAX_CNT; ++i) {
        if (memcmp(s_app_context->eend_dev_list[i].sn, sn, SN_SIZE) == 0) {
            break;
        }
    }
    lwl_eend_dev_t *p = (i != EEND_MAX_CNT) ? &s_app_context->eend_dev_list[i] : NULL;
    os_mutex_post(&g_mutex);
    return p;
}

/**
 * @brief 设置当前激活的E端设备
 * 
 * @param [in] pdev 设备对象引用
 */
void set_eend_dev_active(lwl_eend_dev_t *pdev)
{
    ASSERT(s_app_context != NULL);
    os_mutex_pend(&g_mutex, 0);
    int i;
    for (i = 0; i < EEND_MAX_CNT; ++i) {
        if (&s_app_context->eend_dev_list[i] == pdev) {
            break;
        }
    }
    s_app_context->active_eend_idx = (i == EEND_MAX_CNT) ? -1 : i;
    os_mutex_post(&g_mutex);
}

/**
 * @brief 添加lwl事件处理回调
 * 
 * @param [in] handler 回调函数引用
 */
void lwl_event_handler_add(lwl_event_handler_cb_t *handler) 
{
    ASSERT(s_app_context != NULL);

    os_mutex_pend(&g_mutex, 0);
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
    os_mutex_post(&g_mutex);
}

/**
 * @brief 移除lwl事件处理回调
 * 
 * @param [in] handler 回调函数引用
 */
void lwl_event_handler_remove(lwl_event_handler_cb_t *handler)
{
    ASSERT(s_app_context != NULL);

    os_mutex_pend(&g_mutex, 0);
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
    os_mutex_post(&g_mutex);
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

