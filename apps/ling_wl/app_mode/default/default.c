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

int app_default_app_event_handler(struct application *app, struct sys_event *event)
{
    return false;
}

static int default_init(void)
{
    ble_simple_client_module_enable(0);
    ble_simple_server_module_enable(1);
    return 0;
}

static int default_unint(void)
{
    return 0;
}

void app_default_mode(void)
{
    default_init();

    while (1) {
        // 应用代码
        mdelay(10);
        if (lwl_app_mode_exitting()) {
            default_unint();
            return;
        }
    }
}