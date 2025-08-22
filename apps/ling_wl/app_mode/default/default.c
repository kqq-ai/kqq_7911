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
#include "default.h"
#include "../lwl_app_framework.h"
#include "log.h"
#include "app_mode/lwl_app_core.h"
#include "protocol/cp/c_uart_p.h"

#include "./bt_ble/app_wifi_manage.h"
#include "net_music_play_my.h"
#define CONFIG_VOICE_PROMPT_FILE_PATH       "mnt/sdfile/res/audlogo/"
#define MODE_TIMEOUT_MS 1000 // 1s
static u32 init_time = 0;
static void set_led_idel(void)      { uart_send_set_led_color(COLOR_WHITE); }   //音频下载时灯光变白色
//=================================================================================================
// LWL event callback
//=================================================================================================
static void default_input_event_handler_cb(lwl_event_t *e)
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
        lwl_app_mode_switch_to(APP_NET_CONNECTED_MODE);
        break;
    case LWL_EVENT_TYPE_UART_CLEAR_WIFI_SSID:
        // Reset WiFi SSID
        app_del_stored_wifi_ssid(); // 清除设备保存过的所有wifi
        uart_response_clear_wifi_ssid();
        break;
    case LWL_EVENT_TYPE_UART_SYNC:
        log_i("SYNC response by default_input_event_handler_cb (%d, 12)", timer_get_ms() - init_time < MODE_TIMEOUT_MS);
        uart_response_synch_status(timer_get_ms() - init_time < MODE_TIMEOUT_MS, 12);/// 超时后，返回idle
        break;
    }
}

static int default_init(void)
{
    /* 注册lwl事件处理回调 */
    lwl_event_handler_add(default_input_event_handler_cb);
    set_led_idel(); // 设置LED为白色
    init_time = timer_get_ms();
    return 0;
}

static int default_unint(void)
{
    /* 取消注册lwl事件处理回调 */
    lwl_event_handler_remove(default_input_event_handler_cb);
    return 0;
}

void app_default_mode(void)
{
    default_init();
    lwl_app_context_t *context = lwl_app_core_context_get_instance();
    if(get_wifi_connect_status(NULL) != WIFI_CONNECT_SUCC){
        int err = app_music_play_local_music_with_callback(
            context->music_player,
            CONFIG_VOICE_PROMPT_FILE_PATH "NetCfgFail.mp3",
            NULL,
            NULL);
    }
    //mdelay(2000);
    while (1) {
        mdelay(10);
        if (lwl_app_mode_exitting()) {
            default_unint();
            return;
        }
    }
}