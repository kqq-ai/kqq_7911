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
#include "protocol/cp/c_uart_p.h"

#include "system/os/os_api.h"
#include "system/os/os_api.h"

#include "../../SC7A20/sc7a20.h"
#include "protocol/odm/odmp.h"

// static int wss_app_net_event_handler(struct net_event *event)
// {
//     if (!ASCII_StrCmp(event->arg, "net", 4)) {
//         switch (event->event) {
//         case NET_EVENT_DISCONNECTED:
//             log_i("WSS APP HANDLE DISCONNECT\n");
//             lwl_app_mode_switch_to(APP_NET_CONNECTING_MODE);
//             break;
//         default:
//             break;
//         }
//     }
//     return false;
// }

// int app_wss_conncted_event_handler(struct application *app, struct sys_event *event)
// {
//     switch (event->type) {
//     case SYS_NET_EVENT:
//         return wss_app_net_event_handler((struct net_event *)event->payload);
//     default:
//         return false;
//     }
//     return false;
// }

static int wss_connected_init(void)
{
    /* 开始RSSI测距 */
    monitor_start();
    wss_application_init();
    return 0;
}

static int wss_connected_unint(void)
{
    monitor_stop();
    wss_application_uninit();

    /* 退出wss_connected模式时，关闭WSS连接，释放资源 */
    wss_disconnect();
    return 0;
}

#define NO_E_TIMEOUT_MS      (5 * 60 * 1000) // 5分钟
#define NO_MOVE_TIMEOUT_MS   (1 * 60 * 1000) // 5分钟
// 模式5 加速度传感器相关变量


void *wss_condition_monitor_thread(void *arg)
{
    u32 last_E_found_time = 0;
    u32 last_move_time = 0;

    while (1) {
        // 检查E
        if (!is_E_nearby()) {
            if (!last_E_found_time) last_E_found_time = timer_get_ms();
        } else {
            last_E_found_time = 0;
        }

        // 检查WiFi
        if (!is_device_moving()) { // 你需要实现这个函数
            if (!last_move_time) last_move_time = timer_get_ms();
        } else {
            last_move_time = 0;
        }

        if (last_move_time&&(last_E_found_time&&(timer_get_ms() - last_E_found_time > NO_E_TIMEOUT_MS) ||
            (timer_get_ms() - last_move_time > NO_MOVE_TIMEOUT_MS))) {
            lwl_app_mode_switch_to(APP_DEFAULT_MODE);
            log_i("No E found or device not moving for a long time, switching to default mode.");
            break; // 退出线程
        }
        os_time_dly(100);
        
    }
    return NULL;
}


void app_wss_connected_mode(void)
{
    wss_connected_init();//application init 和blk RSSI测距 
    
    lwl_app_context_t *context = lwl_app_core_context_get_instance();

    // 创建检测线程
    os_task_create(wss_condition_monitor_thread, NULL, 40, 1024, 128, "wss_cond_monitor");

    while (1) {
        // 应用代码
        if (wss_application_loop()) {
            lwl_app_mode_switch_to(APP_NET_CONNECTED_MODE);
        }

        mdelay(10);

        if (lwl_app_mode_exitting()) {
            wss_connected_unint();
            return;
        }


    }
}