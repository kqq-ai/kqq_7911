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

//=================================================================================================
// WSS response callback
//=================================================================================================
static int odmp_rsp_cbk(int type, odmp_package_t *pkg)
{
    switch (type) {
    case OR_INITIAL_SETUP_RESPONSE:
        log_i("INITIAL_SETUP_RESPONSE\n");
        log_i("setup_complete: %d\n", pkg->initial_setup_response.setup_complete);
        break;
    case OR_DEVICE_SWITCH_RESPONSE:
        log_i("DEVICE_SWITCH_RESPONSE\n");
        log_i("device_id: %s\n", pkg->device_switch_response.device_id);
        log_i("status: %s\n", pkg->device_switch_response.status);
        break;
    case OR_TTS_BIND: {
        log_i("TTS_BIND\n");
        log_i("c_id: %s\n", pkg->tts_bind.c_id);
        log_i("send_id: %s\n", pkg->tts_bind.send_id);
        log_i("wait_time: %d\n", pkg->tts_bind.wait_time);

    }   break;
    case OR_AUDIO_STATUS:
        log_i("AUDIO_STATUS\n");
        log_i("c_id: %s\n", pkg->audio_status.c_id);
        log_i("type: %s\n", odmp_ast_to_text(pkg->audio_status.type));
        log_i("event: %s\n", odmp_ase_to_text(pkg->audio_status.event));
        log_i("format: %s\n", pkg->audio_status.audio_param.format);
        log_i("sample_rate: %d\n", pkg->audio_status.audio_param.sample_rate);
        log_i("channels: %d\n", pkg->audio_status.audio_param.channels);
        log_i("frame_duration: %d\n", pkg->audio_status.audio_param.frame_duration);
        
        if (pkg->audio_status.type == OAST_ASR) {
            if (pkg->audio_status.event == OASE_ABNORMAL) {
                log_i("AUDIO_STATUS: ASR OASE_ABNORMAL\n");
            }
        } else if (pkg->audio_status.type == OAST_TTS) {
            if (pkg->audio_status.event == OASE_START) {
                log_i("AUDIO_STATUS: OASE_START\n");
            } else if (pkg->audio_status.event == OASE_END) {
                log_i("AUDIO_STATUS: OASE_END\n");
            } else if (pkg->audio_status.event == OASE_ABNORMAL) {
                log_i("AUDIO_STATUS: OASE_ABNORMAL\n");
            } else if (pkg->audio_status.event == OASE_SENTENCE_START) {
                log_i("AUDIO_STATUS: OASE_ABNORMAL\n");
            } else if (pkg->audio_status.event == OASE_SENTENCE_END) {
                log_i("AUDIO_STATUS: OASE_SENTENCE_END\n");
            }
        }
        break;
    case OR_MESSAGE_TEXT:
        if (pkg->message_text.type == 1)
            log_i("ASR_TEXT %d %s\n", pkg->message_text.text, pkg->message_text.text);
        if (pkg->message_text.type == 2)
            log_i("TTS_TEXT %d %s\n", pkg->message_text.c_id, pkg->message_text.text);
        break;
    case OR_EXECUTION_TIME:
        log_i("EXECUTION_TIME %s %d\n", pkg->execution_time.event, pkg->execution_time.execution_time);
        break;
    case OR_PONG:
        log_i("LR_PONG\n");
        break;
    }
}

static int wss_connected_init(void)
{
    /* 开始RSSI测距 */
    monitor_start();
    wss_application_init();
    /* 注册odmp事件回调 */
    odmp_register_rsp_cbk(odmp_rsp_cbk);
    return 0;
}

static int wss_connected_unint(void)
{
    monitor_stop();
    wss_application_uninit();

    /* 退出wss_connected模式时，关闭WSS连接，释放资源 */
    lwl_app_context_t *context = lwl_app_core_context_get_instance();
    if (context->websockets_info != NULL) {
        wss_server_disconnect(context->websockets_info);
        context->websockets_info = NULL;
    }
    /* 取消注册loswssp事件回调 */
    odmp_register_rsp_cbk(NULL);
    return 0;
}

void app_wss_connected_mode(void)
{
    wss_connected_init();
    
    lwl_app_context_t *context = lwl_app_core_context_get_instance();

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