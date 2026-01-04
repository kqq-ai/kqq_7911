#ifndef __LWL_APP_CORE_H__
#define __LWL_APP_CORE_H__

#include "cpu.h"
#include "audio/recorder.h"
#include "net_music_play_my.h"
#include "os/os_api.h"
#include "util/threadpool.h"
#include "lwl_eend_device.h"
#include "app_lfs.h"
#include "util/cqueue.h"
#include "util/tree_map.h"
#include "app_config.h"

#define EEND_MAX_CNT 3 /**< Maximum number of EEND devices supported */

#define LWL_EVENT_QUEUE_CAPACITY 20 /**< Capacity of the event queue */
#define LWL_EVENT_HANDLER_MAX_CNT 10 /**< Maximum number of event handlers */

#define LWL_ADV_QUEUE_CAPACITY 60 /**< Capacity of the adv queue */

// 三个距离的宏定义
#define BLUE_DISTANCE_TWO_METER (2.0)
#define BLUE_DISTANCE_FIVE_METER (3.5)
#define MAX_BLUE_DISTANCE (5.0)

#define NOTIFY_APP_TIME 1000 // 1s
#define RSSI_EXPIRE_TIME_MS (2000) // RSSI样本过期时间：2秒
#define EDEV_EXPIRE_TIME_MS (10000) // 设备过期时间：10秒
#define LWL_EDEV_EXPIRE_TIME_MS (15000) // 设备过期时间：15秒

typedef enum {
    BDPCTRL_TURNON_FLAG = BIT(0),
    BDPCTRL_PLAY_FLAG = BIT(1),
} bdp_ctrl_mask_e;

typedef enum {
    BDPAS_OFF = 0,
    BDPAS_CONNECTING,
    BDPAS_CONNECTED,
} bdp_adv_stat_e;

typedef enum {
    BDPDOS_ACTIVE_CONNECTING = 0,
    BDPDOS_ACTIVE_SENDING_DATA,
    BDPDOS_ACTIVE_DISCONNECTING,
    BDPDOS_TURNON_CONNECTING,
    BDPDOS_TURNON_DISCONNECTING,
    BDPDOS_TURNON_WAITING_E_ADV_CONNECTED,
    BDPDOS_TURNOFF_WAITING_E_ADV_DISCONNECTD,
} bdp_dev_op_stat_e;

typedef enum {
    BDPDOT_ACTIVE = 0,
    BDPDOT_TURNON,
    BDPDOT_TURNOFF,
} bdp_dev_op_type_e;

const char *adv_stat_to_text(bdp_dev_op_type_e type);


/**
 * @brief Enumeration of LWL event types
 */
typedef enum {
    LWL_EVENT_TYPE_UART_SYNC,                  /**< UART synchronization event */
    // LWL_EVENT_TYPE_UART_IO_KEY_SHORT_CLICK,    /**< Short click on IO key */
    // LWL_EVENT_TYPE_UART_TOUCH_KEY_DOWN,        /**< Touch key pressed event */
    // LWL_EVENT_TYPE_UART_SHUTDOWN,              /**< System shutdown event */
    // LWL_EVENT_TYPE_UART_MOVE_MORE_THAN_ONE_METER, /**< Movement detection event */

    LWL_EVENT_TYPE_EEND_DEV_DISTANCE_CHANGE,   /**< EEND device distance change event */
    LWL_EVENT_TYPE_EEND_DEV_CLOSEST_CHANGE,    /**< Closest EEND device change event */
    LWL_EVENT_TYPE_EEND_DEV_NEW_IN,            /**< New EEND device in */
    LWL_EVENT_TYPE_NEARBY_UPDATE,              /**< 附近设备更新事件(每500ms触发) */

    LWL_EVENT_TYPE_NET_DISCONNECTED,           /**< Network disconnected event */
    LWL_EVENT_TYPE_BACKEND_DISCONNECTED,       /**< Backend service disconnected event */

    LWL_EVENT_TYPE_UART_CLEAR_WIFI_SSID,      /**< Clear stored WiFi SSID event */
    // LWL_EVENT_TYPE_UART_NOTIFY_GSENSOR_OFF,   /**< Notify GSensor off event */
    
    /* C_V2_02 C_V2_01 */
    LWL_EVENT_TYPE_RST_KEY,                    /**< Reset key event */
    LWL_EVENT_TYPE_TOUCH_KEY,                  /**< Touch key event */
    LWL_EVENT_TYPE_KEY,                        /**< Key event */
    LWL_EVENT_TYPE_GSENSOR_IDLE,                    /**< GSensor event */
    LWL_EVENT_TYPE_CHARGE,                     /**< Charge event */
    LWL_EVENT_TYPE_LOW_BATTERY,                /**< Low battery event */
    LWL_EVENT_TYPE_SHUTDOWN,                   /**< System shutdown event */
    LWL_EVENT_TYPE_SLEEP,                      /**< System shutdown event */
} LWL_EVENT_TYPE;

/**
 * @brief SCAN cb process adv data
 */
typedef struct {
    uint8_t adv_data[32];
    uint32_t adv_data_length;
    u8 address_type;  //对方地址类型：0--public address,1--random address
    u8 address[6];    //peer_address
    s8 rssi;
} lwl_cqueue_adv_data_t;

/**
 * @brief UART sync command data structure
 */
typedef struct {
    u8 battery;     /**< Current battery level */
    u8 charging;    /**< Charging status */
} lwl_event_uart_cmd_sync_t;

/**
 * @brief touch_key command data structure
 */
typedef struct {
    u8 touch_key_event_type;     /**< Current touch_key event type */   /* 0: touch_key_down, 1: touch_key_up*/
} lwl_event_touch_key_cmd_t;

/**
 * @brief UART touch_key command data structure
 */
typedef struct {
    u8 touch_key_event_type;     /**< Current touch_key event type */   /* 0: touch_key_down, 1: touch_key_up*/
} lwl_event_uart_cmd_touch_t;
/**
 * @brief key_event_type enum
 */
typedef enum {
    KEY_EVENT_TYPE_SHORT_CLICK = 0, /* 短按 */
    KEY_EVENT_TYPE_DOUBLE_CLICK,     /* 双击 */
    KEY_EVENT_TYPE_TRIPLE_CLICK,     /* 三击 */
    KEY_EVENT_TYPE_LONG_PRESS,      /* 长按 */
} key_event_type_t;
/**
 * @brief _key command data structure
 */
typedef struct {
    key_event_type_t key_event_type;     
} lwl_event_key_cmd_t;

/**
 * @brief charge_event_type enum
 */
typedef enum {
    CHARGE_EVENT_TYPE_CHARGE_IN = 0, /* 充电插入 */
    CHARGE_EVENT_TYPE_CHARGE_OUT,    /* 充电拔出 */
    CHARGE_EVENT_TYPE_CHARGE_FULL,   /* 充电完成 */
} charge_event_type_t;

/**
 * @brief charge command data structure
 */
typedef struct {
    charge_event_type_t charge_event_type;
} lwl_event_charge_cmd_t;


/**
 * @brief EEND device distance change event data structure
 */
typedef struct {
    lwl_eend_dev_t *pdev; /**< Pointer to the EEND device */
} lwl_event_eend_dev_distance_change_t;

/**
 * @brief EEND device new in event data structure
 */
typedef struct {
    lwl_eend_dev_t *pdev; /**< Pointer to the EEND device */
} lwl_event_eend_dev_new_in_t;

/**
 * @brief 附近设备更新事件数据结构
 */
typedef struct {
    // 不需要额外数据,触发时会遍历所有设备
} lwl_event_nearby_update_t;

/**
 * @brief Generic LWL event structure
 */
typedef struct {
    u8 event_type;  /**< Type of the event (from LWL_EVENT_TYPE) */
    union {
        lwl_event_uart_cmd_sync_t uart_cmd_sync; /**< UART sync command data */
        lwl_event_uart_cmd_touch_t uart_cmd_touch; /**< UART touch_key command data */
        lwl_event_touch_key_cmd_t touch_key_cmd; /**< touch_key command data */
        lwl_event_key_cmd_t key_cmd;             /**< Key command data */
        lwl_event_eend_dev_distance_change_t distance_change;   /**< EEND device distance change data */
        lwl_event_charge_cmd_t charge_cmd;
        lwl_event_eend_dev_new_in_t new_in;
        lwl_event_nearby_update_t nearby_update; /**< 附近设备更新事件数据 */
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
    uint8_t mac[6];                  /**< MAC */
    struct websocket_struct *websockets_info; /**< WebSocket connection info */
    recorder_hdl *recorder;          /**< Audio recorder handle */
    music_player *music_player;      /**< Music player instance */
    threadpool_t *threadpool;        /**< Thread pool for asynchronous tasks */
    void *uartdev;                   /**< UART device handle */
    lfs_t *lfs_hdl;                  /**< LittleFS filesystem handle */

    Map *dev_map;                    /**< Map<char *sn, lwl_eend_dev_t *dev> */
    OS_SEM dev_map_mutex;

    MyBlockingQueue event_queue;     /**< Event queue (queue<lwl_event_t>) */
    lwl_event_handler_cb_t event_handlers[LWL_EVENT_HANDLER_MAX_CNT]; /**< Registered event handlers */
    MyBlockingQueue adv_queue;     /**< Adv queue (queue<lwl_cqueue_adv_data_t>) */
    OS_SEM event_handlers_mutex;
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
 * @brief 打印所有E端设备信息
 */
void dump_all_eend_info();

/**
 * @brief 清除过期设备
 */
void clean_expired_eend();

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
 * @brief 获取2米范围内的E端设备数量
 * @return 2米范围内的E端设备数量
 */
int get_2m_edev_count(void);

/**
 * @brief 获取5米范围内的E端设备数量
 * @return 5米范围内的E端设备数量
 */
int get_5m_edev_count();

/**
 * @brief 获取5米范围内的E端设备序列号列表
 * @param [out] targets 序列号列表
 * @param [out] targets_num 序列号数量
 */
void make_5m_edev_targets(char ***targets, int *targets_num);

/**
 * @brief 释放5米范围内的E端设备序列号列表
 * @param [in] targets 序列号列表
 * @param [in] targets_num 序列号数量
 */
void free_5m_edev_targets(char **targets, int targets_num);

/**
 * @brief Add an event handler callback
 * @param handler The handler function to add
 */
void lwl_event_handler_add(lwl_event_handler_cb_t handler);

/**
 * @brief Remove an event handler callback
 * @param handler The handler function to remove
 */
void lwl_event_handler_remove(lwl_event_handler_cb_t handler);

/**
 * @brief Notify the system of a new event
 * @param event Pointer to the event data
 * @note This will queue the event for processing by all registered handlers
 */
void lwl_event_notify(lwl_event_t *event);

/**
 * @brief 清空并释放所有E端设备对象
 *
 * 该函数会遍历设备信息表，释放所有lwl_eend_dev_t对象的内存，并将设备表清空。
 * 调用后，所有已注册的E端设备对象都将被销毁，dev_map也会被清空。
 * 注意：调用后，所有通过add_new_eend_dev分配的pdev指针都将失效。
 */
void clear_all_eend_dev(void);

/**
 * @brief 获取 RTC 时间戳
 */
u32 lwl_rtc_timer_get_sec();

void lwl_rtc_timer_update();

/**
 * @brief 获取ISO8601格式的时间字符串
 * @param buffer 输出缓冲区
 * @param buffer_size 缓冲区大小
 * @note 格式: YYYY-MM-DDTHH:MM:SS.mmmZ
 */
void lwl_rtc_get_iso8601_time(char *buffer, int buffer_size);

#endif