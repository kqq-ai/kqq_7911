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
#include "net_connecting.h"
#include "app_mode/lwl_app_framework.h"
#include "app_mode/lwl_app_core.h"
#include "protocol/cp/c_uart_p.h"
#include "os/os_api.h"
#include "net/wifi/wifi_connect.h"
#include "net_music_play_my.h"
#include "log.h"

#define MODE_TIMEOUT_MS 600000 // 600s
#define MODE_BLE_TIMEOUT_MS 15000 // 15s
static u32 init_time = 0;
static u32 key_press_time_ms = 0;
bool wifi_manager_state = false; // wifi manager 状态
bool ble_connecting = false; // wifi manager 状态
bool ble_connected = false; // wifi manager 状态

void clear_key_press_time_ms(){
    
    ble_connecting = false;
    ble_connected = false;
    key_press_time_ms = 0;
}
// static void set_led_connecting(void)        { uart_send_set_led_color(COLOR_BLUE); }   /* 控制 led : 正在连接网络 蓝色 */
static void set_led_connect_error(void)     { uart_send_set_led_color(COLOR_BLUE); }    /* 控制 led : 网络连接失败 红色 */
static void set_led_connected(void)         { uart_send_set_led_color(COLOR_WHITE); }  /* 控制 led : 联网成功 白色 */
static void set_led_confirm(void)        { uart_send_set_led_color(COLOR_ORANGE); }  //确认配网后灯光变橙色

int app_net_connecting_event_handler(struct application *app, struct sys_event *event)
{
    return false;
}
u32 get_key_times_in_net_connecting(){
    return ble_connecting;
} 
//=================================================================================================
// LWL event callback
//=================================================================================================
static void net_connecting_event_handler_cb(lwl_event_t *e)
{
    static int en = 1;
    lwl_eend_dev_t *p_eend_dev = e->distance_change.pdev;
    lwl_app_context_t *context = lwl_app_core_context_get_instance();

    switch (e->event_type) {
    case LWL_EVENT_TYPE_UART_TOUCH_KEY_DOWN:
        if (e->uart_cmd_touch.touch_key_event_type == 0) {
            log_i("Touch key pressed down");
        } else {
            log_i("Touch key released");
        }
        break;
    case LWL_EVENT_TYPE_UART_IO_KEY_SHORT_CLICK:
        int err = app_music_play_local_music_with_callback(
            context->music_player,
            CONFIG_CP_VOICE_FILE_PATH "t_key.mp3",
            NULL,
            NULL);
        // on_user_key_confirm();
        // 确认连接小程序
        log_i("PRESS NORMAL KEY in net_connecting mode");
        key_press_time_ms = timer_get_ms();
        break;
    case LWL_EVENT_TYPE_UART_SYNC:
        log_i("SYNC response by net_connecting_event_handler_cb (%d, 12)", timer_get_ms() - init_time < MODE_TIMEOUT_MS);
        uart_response_synch_status(timer_get_ms() - init_time < MODE_TIMEOUT_MS, MAX_LE_AUDIO_VOLUME);/// 超时后，返回idle
        break;
    }
}

static int net_connecting_init(void)
{
    init_time = timer_get_ms();
    lwl_event_handler_add(net_connecting_event_handler_cb);
    wifi_manager_state = false;
    ble_connecting = false;
    ble_connected = false;
    key_press_time_ms = 0;

    return 0;
}

static int net_connecting_unint(void)
{
    lwl_event_handler_remove(net_connecting_event_handler_cb);
    if (wifi_manager_state) {
        wifi_manager_state = false;
    }
    ble_simple_server_name_enable(0);
    return 0;
}

/* 处理BLE连接状态的辅助函数 */
static void handle_ble_connection_state(lwl_app_context_t *context)
{
    /* BLE连接超时处理 */
    if (timer_get_ms() - key_press_time_ms > MODE_BLE_TIMEOUT_MS && get_ble_app_disconnect_flag()) {
        set_led_connect_error();
        key_press_time_ms = 0;
        ble_simple_server_name_enable(0);
        ble_connecting = false;
        ble_connected = false;
        printf("BLE timeout,restart ble\n");
        return;
    }

    /* BLE连接尝试 */
    if (!ble_connecting && key_press_time_ms > 0) {
        set_led_confirm();
        ble_connecting = true;
        ble_simple_server_name_enable(1);
        printf("try to connect ble\n");
    }

    /* BLE连接成功处理 */
    if (!get_ble_app_disconnect_flag() && !ble_connected) {
        set_led_connected();
        
        app_music_play_local_music_with_callback(
            context->music_player,
            CONFIG_CP_VOICE_FILE_PATH "bt_conneted.mp3",
            NULL,
            NULL);
            
        ble_connected = true;
        printf("BLE connected\n");
    }
}

void app_net_connecting_loop()
{
    lwl_app_context_t *context = lwl_app_core_context_get_instance();
    enum wifi_sta_connect_state state = wifi_get_sta_connect_state();

    /* 网络连接成功处理 */
    if (WIFI_STA_NETWORK_STACK_DHCP_SUCC == state) {
        printf("Network connection is successful!\n");
        set_led_connected();  // 设置连接成功LED状态
        
        app_music_play_local_music_with_callback(
            context->music_player,
            CONFIG_CP_VOICE_FILE_PATH "NetCfgSucc.mp3",
            NULL,
            NULL);
            
        lwl_app_mode_switch_to(APP_NET_CONNECTED_MODE);
        return;
    }

    /* 网络连接失败处理 */
    bool is_connection_failed = (
        state == WIFI_STA_CONNECT_TIMEOUT_NOT_FOUND_SSID ||
        state == WIFI_STA_CONNECT_ASSOCIAT_FAIL ||
        state == WIFI_STA_DISCONNECT ||
        state == WIFI_STA_CONNECT_ASSOCIAT_TIMEOUT ||
        state == WIFI_STA_NETWORK_STACK_DHCP_TIMEOUT
    );

    if (!wifi_manager_state && is_connection_failed) {
        printf("Network connection is failed!\n");
        set_led_connect_error();  // 设置连接错误LED状态
        
        ble_simple_server_init();
        wifi_manager_state = true;
        
        app_music_play_local_music_with_callback(
            context->music_player,
            CONFIG_CP_VOICE_FILE_PATH "NetCfgEnter.mp3",
            NULL,
            NULL);
            
        return;
    }

    /* BLE连接处理 */
    if (wifi_manager_state && key_press_time_ms > 0) {
        handle_ble_connection_state(context);
    }
}


void app_net_connecting_mode(void)
{
    net_connecting_init();

    while (1) {
        app_net_connecting_loop();
        // 应用代码
        if (lwl_app_mode_exitting()) {
            net_connecting_unint();
            return;
        }
        mdelay(10);
    }
}
