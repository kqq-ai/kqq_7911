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
#include "gpio.h"
#include "protocol/cp/c_uart_p.h"

static void set_led_connected(void)         { uart_send_set_led_color(COLOR_WHITE); }  /* 控制 led : 联网成功 白色 */
static void set_led_connect_error(void)     { uart_send_set_led_color(COLOR_BLUE); }    /* 控制 led : 网络连接失败 红色 */

static int wss_connected_init(void)
{
    /* 开始RSSI测距 */
    wss_application_init();
    return 0;
}

static int wss_connected_unint(void)
{
    wss_application_uninit();//clear all pdev

    /* 退出wss_connected模式时，关闭WSS连接，释放资源 */
    wss_disconnect();
    return 0;
}


void app_wss_connected_mode(void)
{
    lwl_app_context_t *context = lwl_app_core_context_get_instance();
    wss_connected_init();//application init 和blk RSSI测距 
    
    while (1) {
        // 应用代码
        if (wss_application_loop()) {
            set_led_connect_error();// WSS ERROR
            if (wifi_get_sta_connect_state() == WIFI_STA_DISCONNECT) {
                lwl_app_mode_switch_to(APP_NET_CONNECTING_MODE);
                printf("Network connection is WIFI_STA_DISCONNECT!\n");
                int err = app_music_play_local_music_with_callback(
                    context->music_player,
                    CONFIG_CP_VOICE_FILE_PATH "NetCfgFail.mp3",
                    NULL,
                    NULL);
            } else {
                if (!wss_reconnect()) {
                    lwl_app_mode_switch_to(APP_NET_CONNECTED_MODE);
                } else {
                    set_led_connected();
                }
            }
        }

        mdelay(10);

        if (lwl_app_mode_exitting()) {
            wss_connected_unint();
            return;
        }


    }
}