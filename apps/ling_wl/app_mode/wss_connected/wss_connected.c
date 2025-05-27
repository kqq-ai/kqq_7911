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
#include "wss_connected.h"
#include "../lwl_app_framework.h"
#include "../lwl_app_core.h"
#include "wss_application.h"
#include "test/eend_battery_live_test.h"
#include "net_event.h"
#include "log.h"
#include "bt_ble/ble_combined.h"
#include "bt_ble/server/ble_simple_server.h"
#include "bt_ble/client/ble_simple_client.h"

static int wss_app_net_event_handler(struct net_event *event)
{
    if (!ASCII_StrCmp(event->arg, "net", 4)) {
        switch (event->event) {
        case NET_EVENT_DISCONNECTED:
            log_i("WSS APP HANDLE DISCONNECT\n");
            lwl_app_mode_switch_to(APP_NET_CONNECTING_MODE);
            break;
        default:
            break;
        }
    }
    return false;
}

int app_wss_conncted_event_handler(struct application *app, struct sys_event *event)
{
    switch (event->type) {
    case SYS_NET_EVENT:
        return wss_app_net_event_handler((struct net_event *)event->payload);
    default:
        return false;
    }
    return false;
}

static int wss_connected_init(void)
{
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
#else
    ble_simple_server_module_enable(0);
    ble_simple_client_module_enable(1);
    wss_application_init();
#endif
    return 0;
}

static int wss_connected_unint(void)
{
    wss_application_uninit();

    /* 退出wss_connected模式时，关闭WSS连接，释放资源 */
    lwl_app_context_t *context = lwl_app_core_context_get_instance();
    if (context->websockets_info != NULL) {
        wss_server_disconnect(context->websockets_info);
        context->websockets_info = NULL;
    }
    return 0;
}

void app_wss_connected_mode(void)
{
    wss_connected_init();
    
    lwl_app_context_t *context = lwl_app_core_context_get_instance();

    while (1) {
        // 应用代码
        if (wss_application_loop(context->websockets_info)) {
            lwl_app_mode_switch_to(APP_DEFAULT_MODE);
        }
        mdelay(100);
        if (lwl_app_mode_exitting()) {
            wss_connected_unint();
            return;
        }
    }
}