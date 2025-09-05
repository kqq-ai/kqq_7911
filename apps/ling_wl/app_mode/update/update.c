
#include "update.h"
#include "app_mode/lwl_app_framework.h"
#include "app_mode/lwl_app_core.h"
#include "log.h"
#include "app_lfs.h"

#define MODE_TIMEOUT_MS 600000 // 10 minutes
static u32 init_time = 0;

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
        // TODO STOP THE OTA
        break;
    case LWL_EVENT_TYPE_UART_SYNC:
        log_i("SYNC response by update_event_handler_cb (%d, 12)", timer_get_ms() - init_time < MODE_TIMEOUT_MS);
        uart_response_synch_status(timer_get_ms() - init_time < MODE_TIMEOUT_MS, 12);/// 超时后，返回idle
        break;
    }
}
static int update_init(void)
{
    init_time = timer_get_ms();
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