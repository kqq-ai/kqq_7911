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
#include "power_on.h"
#include "app_mode/lwl_app_framework.h"
#include "app_mode/lwl_app_core.h"
#include "log.h"
#include "app_lfs.h"

int app_power_on_event_handler(struct application *app, struct sys_event *event)
{
    return false;
}

bool has_saved_wifi()
{
// TODO
    return true;
}

static int power_on_init(void)
{
    if (has_saved_wifi()) {
        lwl_app_mode_switch_to(APP_NET_CONNECTING_MODE);
    } else {
        lwl_app_mode_switch_to(APP_DEFAULT_MODE);
    }
    return 0;
}

static int power_on_unint(void)
{
    return 0;
}

void app_power_on_mode(void)
{
    power_on_init();
    while (1) {
        // 应用代码
        mdelay(10);
        if (lwl_app_mode_exitting()) {
            power_on_unint();
            return;
        }
    }
}