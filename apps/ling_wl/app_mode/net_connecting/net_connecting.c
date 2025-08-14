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
#include "os/os_api.h"
#include "net/wifi/wifi_connect.h"
#include "bt_ble/app_wifi_manage.h"
#include "log.h"

int app_net_connecting_event_handler(struct application *app, struct sys_event *event)
{
    return false;
}

//=================================================================================================
// LWL event callback
//=================================================================================================
static void net_connecting_event_handler_cb(lwl_event_t *e)
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
        log_i("SYNC response by power_on_input_event_handler_cb (1, 12)");
        uart_response_synch_status(1, 12);/// 0是idle 1是busy，必须回复
        break;
    }
}

static int net_connecting_init(void)
{
    lwl_event_handler_add(net_connecting_event_handler_cb);
    if (wifi_auto_reconnect() == WIFI_CONNECT_SUCC) {
        lwl_app_mode_switch_to(APP_NET_CONNECTED_MODE);
    } else {
        lwl_app_mode_switch_to(APP_DEFAULT_MODE);
    }
    return 0;
}

static int net_connecting_unint(void)
{
    lwl_event_handler_remove(net_connecting_event_handler_cb);
    return 0;
}

void app_net_connecting_mode(void)
{
    net_connecting_init();

    while (1) {
        // 应用代码
        if (lwl_app_mode_exitting()) {
            net_connecting_unint();
            return;
        }
        mdelay(10);
    }
}

