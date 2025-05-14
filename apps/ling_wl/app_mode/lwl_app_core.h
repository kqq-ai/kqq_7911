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
#include "util/tree_map.h"

#define EEND_MAX_CNT 3 /**< Maximum number of EEND devices supported */

#define LWL_EVENT_QUEUE_CAPACITY 20 /**< Capacity of the event queue */
#define LWL_EVENT_HANDLER_MAX_CNT 10 /**< Maximum number of event handlers */

/**
 * @brief Enumeration of LWL event types
 */
typedef enum {
    LWL_EVENT_TYPE_UART_SYNC,                  /**< UART synchronization event */
    LWL_EVENT_TYPE_UART_IO_KEY_SHORT_CLICK,    /**< Short click on IO key */
    LWL_EVENT_TYPE_UART_TOUCH_KEY_DOWN,        /**< Touch key pressed event */
    LWL_EVENT_TYPE_UART_SHUTDOWN,              /**< System shutdown event */
    LWL_EVENT_TYPE_UART_MOVE_MORE_THAN_ONE_METER, /**< Movement detection event */

    LWL_EVENT_TYPE_NET_DISCONNECTED,           /**< Network disconnected event */
    LWL_EVENT_TYPE_BACKEND_DISCONNECTED,       /**< Backend service disconnected event */
} LWL_EVENT_TYPE;

/**
 * @brief UART sync command data structure
 */
typedef struct {
    u8 battery;     /**< Current battery level */
    u8 charging;    /**< Charging status */
} lwl_event_uart_cmd_sync_t;

/**
 * @brief Generic LWL event structure
 */
typedef struct {
    u8 event_type;  /**< Type of the event (from LWL_EVENT_TYPE) */
    union {
        lwl_event_uart_cmd_sync_t uart_cmd_sync; /**< UART sync command data */
    };
} lwl_event_t;

/**
 * @brief Event handler callback function type
 * @param event Pointer to the event data
 */
typedef void (*lwl_event_handler_cb_t)(lwl_event_t *);

/**
 * @brief Main application context structure
 */
typedef struct {
    char wifi_ssid[33];              /**< WiFi SSID configuration */
    char wifi_passwd[64];            /**< WiFi password */
    char lw_x_dev_code[33];          /**< Device code */
    char lw_ota_md5[33];             /**< OTA update MD5 checksum */
    int  lw_ota_ver_code;            /**< OTA version code */
    char lw_ota_ver_name[33];        /**< OTA version name */
    char *wss_url;                   /**< WebSocket server URL */
    struct websocket_struct *websockets_info; /**< WebSocket connection info */
    recorder_hdl *recorder;          /**< Audio recorder handle */
    music_player *music_player;      /**< Music player instance */
    threadpool_t *threadpool;        /**< Thread pool for asynchronous tasks */
    void *uartdev;                   /**< UART device handle */
    lfs_t *lfs_hdl;                  /**< LittleFS filesystem handle */
    int active_eend_idx;             /**< Index of active EEND device */

    Map *dev_map;                    /**< Map<char *sn, lwl_eend_dev_t *dev> */

    MyBlockingQueue event_queue;     /**< Event queue (queue<lwl_event_t>) */
    lwl_event_handler_cb_t event_handlers[LWL_EVENT_HANDLER_MAX_CNT]; /**< Registered event handlers */
} lwl_app_context_t;

/**
 * @name app_core_context_get_instance
 * @brief 获取app_context
 * @note 此函数会阻塞当前任务，不可在中断中执行
 */
lwl_app_context_t* lwl_app_core_context_get_instance();

/**
 * @brief Initialize the application core
 * @note Must be called before any other core functions
 */
void lwl_app_core_init();

/**
 * @brief Clean up and destroy application core resources
 */
void lwl_app_core_destroy();

/**
 * @brief 添加新的E端设备信息到设备信息表中
 * @param [in] sn 序列号
 * @return 设备对象引用
 */
lwl_eend_dev_t *add_new_eend_dev(u8 *sn);

/**
 * @brief 遍历所有E端设备
 * 
 * @param [in] callback 回调函数
 * @param [in] priv 回调函数私有数据
 * @note 回调函数的参数为设备对象引用和私有数据
 */
void eend_dev_traverse(void (*callback)(lwl_eend_dev_t *, void *), void *priv);

/**
 * @brief Find an EEND device by its serial number
 * @param sn Pointer to the serial number to search for
 * @return Pointer to the matching EEND device, or NULL if not found
 */
lwl_eend_dev_t* find_eend_dev_by_sn(u8 *sn);

/**
 * @brief 查找信号最强的E端设备
 * @return 信号最强的设备对象引用，如果未找到返回 NULL
 */
lwl_eend_dev_t *find_eend_dev_closest(void);

/**
 * @brief Add an event handler callback
 * @param handler Pointer to the handler function to add
 */
void lwl_event_handler_add(lwl_event_handler_cb_t *handler);

/**
 * @brief Remove an event handler callback
 * @param handler Pointer to the handler function to remove
 */
void lwl_event_handler_remove(lwl_event_handler_cb_t *handler);

/**
 * @brief Notify the system of a new event
 * @param event Pointer to the event data
 * @note This will queue the event for processing by all registered handlers
 */
void lwl_event_notify(lwl_event_t *event);

#endif