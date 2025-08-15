/**
 * @author chuxuanhuang
 * @brief 
 *   +----> IDLE
 *   |
 * POWER_ON -------> NET_CONNECTING <-------> NET_CONNECTED
 *   |                   ^    ^                     ^
 *   |                   |    |                     |
 *   +----> DEFAULT <----+    +---- WSS_CONNECTED <-+
 * 
 * POWER 上电模式
 * IDLE 空闲模式（无任务不代表低功耗）
 * DEFAULT 默认模式：
 *     默认开启BLE ADV，提供配网功能
 * NET_CONNECTING 连接模式
 *     尝试连接WIFI
 * NET_CONNECTED 已连接WIFI的模式
 *     尝试连接后端服务器
 * WSS_CONNECTED 后端服务器连接成功模式
 *     可以进行应用逻辑处理
 */
#include "net_connected.h"
#include "../lwl_app_framework.h"
#include "../lwl_app_core.h"
#include "../wss_connected/wss_application.h"
#include "log.h"
#include "ota/http_ota.h"
#include "log/log_mqtt.h"
#include "bt_ble/app_wifi_manage.h"
#include "protocol/cp/c_uart_p.h"

#include "protocol/odm/odm_secure.h"

#define MODE_TIMEOUT_MS 10000 // 10s
static u32 init_time = 0;

static void set_led_connect_error(void)     { uart_send_set_led_color(COLOR_RED); }    /* 控制 led : 网络连接失败 红色 */

//=================================================================================================
// LWL event callback
//=================================================================================================
static void net_connected_event_handler_cb(lwl_event_t *e)
{
    static int en = 1;
    lwl_eend_dev_t *p_eend_dev = e->distance_change.pdev;

    switch (e->event_type) {
    case LWL_EVENT_TYPE_UART_TOUCH_KEY_DOWN:
        // play 提示音
        break;
    case LWL_EVENT_TYPE_UART_TOUCH_KEY_UP:
        // 处理触摸按键抬起事件
        break;
    case LWL_EVENT_TYPE_UART_IO_KEY_SHORT_CLICK:
        break;
    case LWL_EVENT_TYPE_UART_CLEAR_WIFI_SSID:
        // Reset WiFi SSID
        break;
    case LWL_EVENT_TYPE_UART_SYNC:
        log_i("SYNC response by net_connected_event_handler_cb (%d, 12)", timer_get_ms() - init_time < MODE_TIMEOUT_MS);
        uart_response_synch_status(timer_get_ms() - init_time < MODE_TIMEOUT_MS, 12);/// 超时后，返回idle
        break;
    }
}

static int net_connected_init(void)
{
    init_time = timer_get_ms();
    lwl_app_context_t *context = lwl_app_core_context_get_instance();
    lwl_event_handler_add(net_connected_event_handler_cb);
    int ret = log_mqtt_start();
    
    // if (ret==0) {
    //     log_i("log_mqtt_start init\n");
    //     ret = mqtt_subscribe_reset();
    // }
    // OTA check version
    do_check_and_ota();
    context->websockets_info = NULL;
    return 0;
}


static int net_connected_unint(void)
{
    lwl_app_context_t *context = lwl_app_core_context_get_instance();
    lwl_event_handler_remove(net_connected_event_handler_cb);
    if (context->websockets_info != NULL) {
        //TODO 同步设备状态 IoT Cload
        //不能关 websocket !!!
        //APP_WSS_CONNECTED_MODE 模式退出时关闭
#if 0
        if (!get_ble_app_disconnect_flag()) {
            lwl_app_context_t *ctx = lwl_app_core_context_get_instance();
            /* 通知 APP(小程序) 我已经与wss握手成功 */
            if (!threadpool_add_task(ctx->threadpool, notify_app_wifi_mqtt_succ, NULL)) {
                printf("Failed to add the notify_app_wifi_mqtt_succ task !\r\n");
            }
        }
        ble_simple_server_module_enable(0);
        ble_simple_client_module_enable(1);
        /* 等待ble与小程序断开 再进入小程序 */
        while(1) {
            if (get_ble_app_disconnect_flag()) {
                wss_application_init();
                break;
            }
        }
        printf("wss_connected_init exit ! \r\n");
#endif

    }
    return 0;
}

void app_net_connected_loop()
{
    log_i("websockets init\n");
    if (WIFI_CONNECT_SUCC != get_wifi_connect_status(NULL)) {
        lwl_app_mode_switch_to(APP_DEFAULT_MODE);
        // wifi manager
        return;
    }
    extern int OTA_download_running;
    if( OTA_download_running > 0){
        lwl_app_mode_switch_to(APP_UPDATE_MODE);
        return;
    }
    if (wss_connect()) {
        lwl_app_mode_switch_to(APP_WSS_CONNECTED_MODE);
    } else {
        log_i("websockets init failed\n");
        set_led_connect_error();// WSS ERROR
        
        // 无网络超时10s，进入配网
        if (timer_get_ms() - init_time >= 10000) {
            lwl_app_mode_switch_to(APP_DEFAULT_MODE);
        }
    }
    // TODO 尝试重连WSS
}

void app_net_connected_mode(void)
{
    net_connected_init();

    while (1) {
        app_net_connected_loop();
        // 应用代码
        mdelay(10);
        if (lwl_app_mode_exitting()) {
            net_connected_unint();
            return;
        }
    }
}