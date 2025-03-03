#ifndef __LWL_APP_CORE_H__
#define __LWL_APP_CORE_H__

#include "cpu.h"
#include "wss/wss_client.h"
#include "audio/recorder.h"
#include "net_music_play_my.h"
#include "os/os_api.h"
#include "util/threadpool.h"
#include "lwl_eend_device.h"
#include "app_lfs.h"
#include "util/cqueue.h"

#define EEND_MAX_CNT 3

#define LWL_EVENT_QUEUE_CAPACITY 20
#define LWL_EVENT_HANDLER_MAX_CNT 10

typedef enum {
    LWL_EVENT_TYPE_UART_SYNC,
    LWL_EVENT_TYPE_UART_IO_KEY_SHORT_CLICK,
    LWL_EVENT_TYPE_UART_TOUCH_KEY_DOWN,
    LWL_EVENT_TYPE_UART_SHUTDOWN,

    LWL_EVENT_TYPE_NET_DISCONNECTED,
    LWL_EVENT_TYPE_BACKEND_DISCONNECTED,
} LWL_EVENT_TYPE;

typedef struct {
    u8 battery;
    u8 charging;
} lwl_event_uart_cmd_sync_t;

typedef struct {
    u8 event_type;
    union {
        lwl_event_uart_cmd_sync_t uart_cmd_sync;
    };
} lwl_event_t;

typedef void (*lwl_event_handler_cb_t)(lwl_event_t *);

typedef struct {
    char wifi_ssid[33];
    char wifi_passwd[64];
    char *wss_url;
    struct websocket_struct *websockets_info;
    recorder_hdl *recorder;
    music_player *music_player;
    threadpool_t *threadpool;
    void *uartdev;
    lfs_t *lfs_hdl;
    int active_eend_idx;
    lwl_eend_dev_t eend_dev_list[EEND_MAX_CNT];
    
    MyBlockingQueue event_queue;    // queue<lwl_event_t>
    lwl_event_handler_cb_t event_handlers[LWL_EVENT_HANDLER_MAX_CNT];

} lwl_app_context_t;

/**
 * @name app_core_context_get_instance
 * @brief 获取app_context
 * @note 此函数会阻塞当前任务，不可在中断中执行
 */
lwl_app_context_t* lwl_app_core_context_get_instance();

void lwl_app_core_init();
void lwl_app_core_destroy();

lwl_eend_dev_t* find_eend_dev_active();
lwl_eend_dev_t* find_eend_dev_by_sn(u8 *sn);
void set_eend_dev_active(lwl_eend_dev_t *pdev);

void lwl_event_handler_add(lwl_event_handler_cb_t *handler);
void lwl_event_handler_remove(lwl_event_handler_cb_t *handler);
void lwl_event_notify(lwl_event_t *event);

#endif