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
#include "app_cookie.h"
#include "net_connecting.h"
#include "app_mode/lwl_app_framework.h"
#include "app_mode/lwl_app_core.h"
#include "os/os_api.h"
#include "net/wifi/wifi_connect.h"
#include "net_music_play_my.h"
#include "log.h"
#include "LED/LED.h"

#define MODE_TIMEOUT_MS 180000 // 180s
#define MOVE_TIMEOUT_MS 60000 // 60s
#define MODE_BLE_TIMEOUT_MS 10000 // 10s
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

static void set_led_connecting(void)        { set_rgb_color(COLOR_CYAN); }
static void set_led_connected(void)         { set_rgb_color(COLOR_BLUE); }
static void set_led_confirm_on(void)        { set_rgb_ON(COLOR_GREEN,2); }
static void set_led_confirm_off(void)       { set_rgb_OFF(COLOR_GREEN,2); }

int app_net_connecting_event_handler(struct application *app, struct sys_event *event)
{
    return false;
}

u32 get_key_times_in_net_connecting(){
    return ble_connecting;
} 

static void start_ble_net(){
    if (wifi_manager_state) {
        key_press_time_ms = 0;
    }
} 
static void reset_ble_net(){
    if (wifi_manager_state) {
        set_led_confirm_off();
        key_press_time_ms = 0;
        ble_simple_server_name_enable(0);
        ble_connecting = false;
        ble_connected = false;
        printf("BLE timeout,restart ble\n");
    }
} 
//=================================================================================================
// LWL event callback
//=================================================================================================
static void net_connecting_event_handler_cb(lwl_event_t *e)
{
    static int en = 1;
    lwl_eend_dev_t *p_eend_dev = e->distance_change.pdev;

    switch (e->event_type) {
    case LWL_EVENT_TYPE_TOUCH_KEY:
        if (e->touch_key_cmd.touch_key_event_type == 0) {
            log_i("Touch key pressed down");
            reset_ble_net();
            lwl_app_mode_play(CONFIG_CP_VOICE_FILE_PATH "t_key.mp3", start_ble_net);
        } else {
            log_i("Touch key released");
        }
        break;
    case LWL_EVENT_TYPE_LOW_BATTERY:
        u16 voltage = get_vbat_level();
        if (voltage < LOW_BATT_VERY_SHUTDOWN) {
            lwl_app_mode_switch_to(APP_DEFAULT_MODE);
        }
        break;
    }
}

static int net_connecting_init(void)
{
    init_time = timer_get_ms();
    set_led_connecting();
    g_sensor_reset_alive_timer();
    g_sensor_set_alive_timeout(MOVE_TIMEOUT_MS);
    lwl_event_handler_add(net_connecting_event_handler_cb);
    wifi_manager_state = false;
    ble_connecting = false;
    ble_connected = false;
    key_press_time_ms = 0;
    log_i("【debug】wifi开始连接: enter NET_CONNECTING mode");
    ble_simple_server_init();

    
    while (lwl_app_mode_playing()) {
        msleep(10);
        if (WIFI_STA_NETWORK_STACK_DHCP_SUCC == wifi_get_sta_connect_state()) {
            break;
        }
    }

    return 0;
}

static int net_connecting_unint(void)
{
    lwl_event_handler_remove(net_connecting_event_handler_cb);
    if (wifi_manager_state) {
        wifi_manager_state = false;
    }
    ble_simple_server_exit();
    return 0;
}

/* 处理BLE连接状态的辅助函数 */
static void handle_ble_connection_state()
{
    /* BLE连接超时处理 */
    if (timer_get_ms() - key_press_time_ms > MODE_BLE_TIMEOUT_MS && get_ble_app_disconnect_flag()) {
        reset_ble_net();
        printf("BLE timeout,restart ble\n");
        return;
    }

    /* BLE连接尝试 */
    if (!ble_connecting && key_press_time_ms > 0) {
        set_led_confirm_on();
        ble_connecting = true;
        ble_simple_server_name_enable(1);
        printf("try to connect ble\n");
    }

    /* BLE连接成功处理 */
    if (!get_ble_app_disconnect_flag() && !ble_connected) {
        set_led_confirm_off();
        set_led_connected();
        play_system_music("bt_conneted.mp3");
        ble_connected = true;
        printf("BLE connected\n");
    }
}

void app_net_connecting_loop()
{
    enum wifi_sta_connect_state state = wifi_get_sta_connect_state();

    /* 网络连接成功处理 */
    if (WIFI_STA_NETWORK_STACK_DHCP_SUCC == state) {
        ///if( wifi_manager_state )         /// 等待配网的BLE通讯完成
        ///if( !NET_NTP_GET_TIME_SUCC )     /// 等待获取时间，才能https通讯，NET_NTP_GET_TIME_SUCC
        mdelay(200);
        log_i("【debug】wifi连接成功: DHCP_SUCC, switching to NET_CONNECTED");
        printf("Network connection is successful!\n");
        switch_rf_coexistence_config_table(3);
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
        log_i("【debug】wifi连接失败: first failure event=%d, start BLE advertising for provisioning", state);
        printf("Network connection is failed!\n");

        wifi_manager_state = true;
        key_press_time_ms = timer_get_ms();
        ble_connecting = true;
        ble_connected = false;
        ble_simple_server_name_enable(1);

        play_system_music("NetCfgEnter.mp3");
            
        return;
    }

    /* BLE连接处理 */
    if (wifi_manager_state) {
        handle_ble_connection_state();
        g_sensor_reset_alive_timer();
    }

    /* 超时处理 */
    if (timer_get_ms() - init_time > MODE_TIMEOUT_MS) {
        printf("Net connecting mode timeout, switch to POWER_OFF mode\n");
        disable_gsensor();// 无联网避免晃动反复唤醒设备，需要关机，而不是休眠
        lwl_app_mode_switch_to(APP_POWER_OFF_MODE);
        return;
    }
}


void app_net_connecting_mode(void)
{
    net_connecting_init();

    while (1) {
        lwl_app_mode_play_loop();
        if (lwl_app_mode_exitting()) {
            net_connecting_unint();
            return;
        }
        // 应用代码
        app_net_connecting_loop();
        mdelay(10);
    }
}
