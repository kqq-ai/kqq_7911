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
#include "net_connected.h"
#include "../lwl_app_framework.h"
#include "../lwl_app_core.h"
#include "../wss_connected/wss_application.h"
#include "log.h"
#include "ota/http_ota.h"
#include "log/log_mqtt.h"
#include "bt_ble/app_wifi_manage.h"
#include "LED/LED.h"

#include "protocol/odm/odm_secure.h"

#define MOVE_TIMEOUT_MS 60000 // 60s
static u32 init_time = 0;

static void set_led_connected(void)         { set_rgb_color(COLOR_WHITE); }  /* 控制 led : 联网成功 白色 */
static void set_led_connect_error(void)     { set_rgb_color(COLOR_CYAN); }

//=================================================================================================
// LWL event callback
//=================================================================================================
static void net_connected_event_handler_cb(lwl_event_t *e)
{
    static int en = 1;
    lwl_eend_dev_t *p_eend_dev = e->distance_change.pdev;

    switch (e->event_type) {
    case LWL_EVENT_TYPE_TOUCH_KEY:
        // play 提示音
        break;
    case LWL_EVENT_TYPE_KEY:
        break;
    case LWL_EVENT_TYPE_GSENSOR_IDLE:
        break;
    case LWL_EVENT_TYPE_LOW_BATTERY:
        u16 voltage = get_vbat_level();
        if (voltage < LOW_BATT_VERY_SHUTDOWN) {
            lwl_app_mode_switch_to(APP_DEFAULT_MODE);
        }
        break;
    }
}

static int net_connected_init(void)
{
    g_sensor_set_alive_timeout(MOVE_TIMEOUT_MS);
    ble_simple_client_module_enable(1);
    
    init_time = timer_get_ms();
    lwl_app_context_t *context = lwl_app_core_context_get_instance();
    lwl_event_handler_add(net_connected_event_handler_cb);
    set_led_connected();
    // OTA check version

    msleep(200);
    if (check_ota_info()==1) {
        lwl_app_mode_switch_to(APP_UPDATE_MODE);
        return 1;
    }
    int ret = log_mqtt_start();
    if (wss_connect()) {
        lwl_app_mode_switch_to(APP_WSS_CONNECTED_MODE);
    } else {
        log_mqtt("❌ websockets init failed\n");
        set_led_connect_error();// WSS ERROR
    }
    return 0;
}


static int net_connected_unint(void)
{
    lwl_event_handler_remove(net_connected_event_handler_cb);
    set_led_connected();
    return 0;
}

void app_net_connected_loop()
{
    log_i("websockets init\n");
    if (WIFI_CONNECT_SUCC != get_wifi_connect_status(NULL)) {
        lwl_app_mode_switch_to(APP_NET_CONNECTING_MODE);
        printf("Network connection is WIFI_STA_DISCONNECT!\n");
        play_system_music("NetCfgFail.mp3");
        return;
    }
    if (wss_connect()) {
        lwl_app_mode_switch_to(APP_WSS_CONNECTED_MODE);
    } else {
        log_mqtt("❌❌❌ websockets init failed\n");
        set_led_connect_error();// WSS ERROR
        msleep(1000);
        
        // 无网络超时10s，进入配网
        // if (timer_get_ms() - init_time >= 10000) {
        //     lwl_app_mode_switch_to(APP_NET_CONNECTING_MODE);
        // }
    }
}

void app_net_connected_mode(void)
{
    net_connected_init();

    while (1) {
        if (lwl_app_mode_exitting()) {
            net_connected_unint();
            return;
        }
        // 应用代码
        app_net_connected_loop();
        mdelay(10);
    }
}