#include "wss_device.h"
#include "wss_request.h"
#include "cpu.h"

#include "../lwl_app_framework.h"
#include "util/cqueue.h"
#include "util/ringbuffer.h"
#include "bt_ble/client/ble_distance.h"
#include "async_msg_loop.h"
#include "app_mode/lwl_app_core.h"
#include "device.h"
#include "util/threadpool_tasks.h"

#include "ota/http_ota.h"
#include "uart.h"

#include "protocol/odm/odmp.h"

#include "protocol/cp/c_uart_p.h"

#define log_d log_i // Debug log macro

static lwl_app_context_t *g_app_ctx = NULL;
static lwl_eend_dev_t *g_edev_closest = NULL; // 服务器维护

static bool is_edev_far_away(lwl_eend_dev_t *pdev)
{
    return (pdev->distance_info.distance_type == DISTANCE_2) || (pdev->distance_info.distance_type == DISTANCE_3);
}

//=================================================================================================
// LWL event callback
//=================================================================================================
static void device_event_handler_cb(lwl_event_t *e)
{
    static int en = 1;
    lwl_eend_dev_t *p_eend_dev = e->distance_change.pdev;

    switch (e->event_type) {
    case LWL_EVENT_TYPE_EEND_DEV_CLOSEST_CHANGE:
        break;
    case LWL_EVENT_TYPE_EEND_DEV_DISTANCE_CHANGE:
        char sn_string[SN_SIZE * 2 + 1] = "(NULL)";
        p_eend_dev->get_string(p_eend_dev, sn_string, sizeof(sn_string));
        log_i("[MEASDIST] %s: %d %d\r\n", sn_string, p_eend_dev->distance_info.rssi, p_eend_dev->distance_info.distance_type);

        switch (p_eend_dev->distance_info.distance_type) {
        case DISTANCE_0:
            log_d("[MEASDIST] distance < 2m : do things1 here --- %s\r\n", sn_string);

            if (p_eend_dev != g_edev_closest) {
                g_edev_closest = p_eend_dev;
                p_eend_dev->handshake_info.need_handshake=true; // 设置握手标志
                wss_request_send_event(p_eend_dev, ORMET_NEAR_TRIGGER);
            }

            break;
        case DISTANCE_1:
            log_d("[MEASDIST] distance > 2m  < 5m do things2 here ---%s\r\n", sn_string);
            break;
        case DISTANCE_2:
            log_d("[MEASDIST] distance > 5m do things3 here ---%s\r\n", sn_string);
            break;
        case DISTANCE_3:
            log_d("[MEASDIST] not found ---%s\r\n", sn_string);
            break;
        }
        break;
    case LWL_EVENT_TYPE_EEND_DEV_NEW_IN:
        log_i("ORMET_NEW_DEVICE_IN");
        break;
    }
}


void wss_device_init()
{
    log_i("Init WSS device\n");
    g_app_ctx = lwl_app_core_context_get_instance();
    ble_simple_server_module_enable(0);
    ble_simple_client_module_enable(1);

    /* 注册lwl事件处理回调 */
    lwl_event_handler_add(device_event_handler_cb);

}

void wss_device_uninit()
{
    log_i("Uninit WSS device\n");

    /* 取消注册lwl事件处理回调 */
    lwl_event_handler_remove(device_event_handler_cb);
}

bool wss_device_loop()
{
    return false;
}