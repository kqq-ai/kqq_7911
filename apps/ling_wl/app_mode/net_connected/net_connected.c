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


static void set_led_connect_error(void)     { uart_send_set_led_color(COLOR_RED); }    /* 控制 led : 网络连接失败 红色 */

static int net_connected_init(void)
{
    lwl_app_context_t *context = lwl_app_core_context_get_instance();
    int ret = log_mqtt_start();
    log_i("log_mqtt_start init\n");
    // OTA check version
    ret = do_check_and_ota();
    if (ret == 1) {
        lwl_app_mode_switch_to(APP_UPDATE_MODE);
        return 0;
    }
    context->websockets_info = NULL;
    return 0;
}


static int net_connected_unint(void)
{
    lwl_app_context_t *context = lwl_app_core_context_get_instance();
    if (context->websockets_info != NULL) {
        //TODO 同步设备状态
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
    if (wss_connect()) {
        lwl_app_mode_switch_to(APP_WSS_CONNECTED_MODE);
    } else {
        log_i("websockets init failed\n");
        set_led_connect_error();// WSS ERROR
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