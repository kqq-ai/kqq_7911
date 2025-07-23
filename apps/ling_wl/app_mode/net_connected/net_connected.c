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
#include "net_connected.h"
#include "../lwl_app_framework.h"
#include "../lwl_app_core.h"
#include "../wss_connected/wss_application.h"
#include "log.h"
#include "ota/http_ota.h"
#include "log/log_mqtt.h"

// #include "protocol/odm/odmp.h"
// #include "protocol/odm/odm_util.h"

#include "wss/wss_client.h"

#include "protocol/odm/odm_secure.h"

// #define TEST_LOCOL_MODE

int app_net_connected_event_handler(struct application *app, struct sys_event *event)
{
    return false;
}

static int net_connected_init(void)
{
    // OTA check version
    int ret = do_check_and_ota();
    if (ret == 1) {
        lwl_app_mode_switch_to(APP_UPDATE_MODE);
        return 0;
    }
    ret = log_mqtt_start();
    lwl_app_context_t *context = lwl_app_core_context_get_instance();

#ifdef TEST_LOCOL_MODE
    char *secure_url = get_wss_test_url(); // TODO: 释放资源
    struct websocket_info_t *websockets_info = wss_server_connect(secure_url);
#else
    char *secure_url = get_wss_connection_url(); // TODO: 释放资源
    struct websocket_info_t *websockets_info = wss_server_connect(secure_url);
#endif

    if (websockets_info == NULL) {
        log_i("websockets init failed\n");
        free(websockets_info);
        context->wss_url = secure_url;
        context->websockets_info = NULL;
        // TODO 尝试重连WSS
        // lwl_app_mode_switch_to(APP_WSS_CONNECTED_MODE);
    } else {
        /* 将websocket_info保存到全局上下文 */
        context->wss_url = secure_url;
        context->websockets_info = websockets_info;
        lwl_app_mode_switch_to(APP_WSS_CONNECTED_MODE);
    }
    return 0;
}


static int net_connected_unint(void)
{
    lwl_app_context_t *context = lwl_app_core_context_get_instance();
    free(context->wss_url);
    context->wss_url = NULL;
    return 0;
}

void app_net_connected_mode(void)
{
    net_connected_init();

    while (1) {
#if 0
        auto_test_application_loop();
#else
#endif
        // 应用代码
        mdelay(10);
        if (lwl_app_mode_exitting()) {
            net_connected_unint();
            return;
        }
    }
}