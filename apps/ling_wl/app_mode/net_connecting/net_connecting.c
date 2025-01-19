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
#include "log.h"

int app_net_connecting_event_handler(struct application *app, struct sys_event *event)
{
    return false;
}

static void connect_success()
{
    lwl_app_mode_switch_to(APP_NET_CONNECTED_MODE);
}

static void connect_failed()
{
    lwl_app_mode_switch_to(APP_DEFAULT_MODE);
}

static void connect_wifi(char *ssid, char *passwd, void *success_cb, void *failed_cb) 
{
    enum wifi_sta_connect_state state;

    wifi_enter_sta_mode(ssid, passwd);

    while (1) {
        log_i("Connecting to the network...\n");
        state = wifi_get_sta_connect_state();
        if (WIFI_STA_NETWORK_STACK_DHCP_SUCC == state) {
            log_i("Network connection is successful!\n");
            connect_success();
            break;
        }
        os_time_dly(500);
    }
}

static int net_connecting_init(void)
{
    lwl_app_context_t *pcontext = lwl_app_core_context_get_instance();

    connect_wifi("vivo", "h116741927", connect_success, connect_failed);

    return 0;
}

static int net_connecting_unint(void)
{
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

