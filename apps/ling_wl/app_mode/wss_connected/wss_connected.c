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
#include "LED/LED.h"

static void set_led_connecting_on(void)     { set_rgb_ON(COLOR_CYAN, 5); }
static void set_led_connecting_off(void)    { set_rgb_OFF(COLOR_CYAN,5); }
bool play_prompt_flag = 0;

static int wss_connected_init(void)
{
    log_i("[wss_connected]enter mode");
    /* 开始RSSI测距 */
    wss_application_init();
    ble_dev_task_scheduler_start();
    play_prompt_flag = 1;
    return 0;
}

static int wss_connected_unint(void)
{
    log_i("[wss_connected]leave mode");
    wss_application_uninit();//clear all pdev
    if (play_prompt_flag == 0) {
        set_led_connecting_off();
        play_prompt_flag = 1;
    }

    /* 退出wss_connected模式时，关闭WSS连接，释放资源 */
    wss_disconnect();
    return 0;
}

#define HEAT_TIMEOUT 1000

void app_wss_connected_mode(void)
{
    wss_connected_init();//application init 和blk RSSI测距 
    u32 g_heat_timer = 0;
    
    while (1) {
        lwl_app_mode_play_loop();
        if (lwl_app_mode_exitting()) {
            log_i("[wss_connected]mode exiting requested");
            wss_connected_unint();
            return;
        }
        lwl_app_context_t *ctx = lwl_app_core_context_get_instance();
        if (ctx->websockets_info) {
            wss_send_pong(ctx->websockets_info);
        }
        mdelay(10);
        if (timer_get_ms() - g_heat_timer >= HEAT_TIMEOUT) {
            dump_all_eend_info();
            g_heat_timer = timer_get_ms();
        }
        if (lwl_app_mode_playing()) continue;
        // 应用代码
        if (wss_application_loop()) {
            if (wifi_get_sta_connect_state() == WIFI_STA_DISCONNECT) {
                log_i("[wss_connected]wifi disconnect detected while in WSS, switch to NET_CONNECTING");
                lwl_app_mode_switch_to(APP_NET_CONNECTING_MODE);
                printf("Network connection is WIFI_STA_DISCONNECT!\n");
                play_system_music("NetCfgFail.mp3");
            } else if (wss_application_check_power_key()) {
                log_i("[wss_connected]power key retry requested, try reconnect");
                if (!wss_reconnect()) {
                    log_i("[wss_connected]reconnect failed, fallback to NET_CONNECTED");
                    lwl_app_mode_switch_to(APP_NET_CONNECTED_MODE);
                } else {
                    set_led_connecting_off();
                    wss_application_reset();
                    play_prompt_flag = 1;
                }
            } else if (play_prompt_flag) {
                log_i("[wss_connected]❌❌❌ play disconnect prompt because application loop requested exit");
                set_led_connecting_on();// WSS ERROR
                play_prompt_flag = 0;
                play_system_music("NetDisc.mp3");
            }
        }


    }
}