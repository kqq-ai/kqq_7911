
#include "update.h"
#include "app_mode/lwl_app_framework.h"
#include "app_mode/lwl_app_core.h"
#include "log.h"
#include "app_lfs.h"

//=================================================================================================
// LWL event callback
//=================================================================================================
static void update_event_handler_cb(lwl_event_t *e)
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
static int update_init(void)
{
    lwl_event_handler_add(update_event_handler_cb);
    return 0;
}

static int update_unint(void)
{
    lwl_event_handler_remove(update_event_handler_cb);
    return 0;
}

void app_update_mode(void)
{
    update_init();

    while (1) {
        // 应用代码
        mdelay(10);
        if (lwl_app_mode_exitting()) {
            update_unint();
            return;
        }
    }
}