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

static int net_connecting_init(void)
{
    if (wifi_connect_block("hello", "88888888", 10) == WIFI_CONNECT_SUCC) {
        lwl_app_mode_switch_to(APP_NET_CONNECTED_MODE);
    } else {
        lwl_app_mode_switch_to(APP_DEFAULT_MODE);
    }

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

