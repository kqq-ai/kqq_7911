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
#include "app_cookie.h"
#include "protocol/cp/c_uart_p.h"

#define MODE_TIMEOUT_MS 10000 // 10s
static u32 init_time = 0;
static void set_led_power_on(void)      { uart_send_set_led_color(COLOR_BLUE); }   /* 控制 led : 上电 蓝色 */

int app_power_on_event_handler(struct application *app, struct sys_event *event)
{
    return false;
}

bool has_saved_wifi()
{
// TODO
    return true;
}
//=================================================================================================
// LWL event callback
//=================================================================================================
static void power_on_input_event_handler_cb(lwl_event_t *e)
{
    static int en = 1;
    lwl_eend_dev_t *p_eend_dev = e->distance_change.pdev;

    switch (e->event_type) {
    case LWL_EVENT_TYPE_TOUCH_KEY:
        // play 提示音
        break;
    case LWL_EVENT_TYPE_KEY:
        break;
    case LWL_EVENT_TYPE_GSENSOR_IDLE:
        g_sensor_set_alive_timeout(MODE_TIMEOUT_MS);
        break;
    }
}

static int power_on_init(void)
{
    init_time = timer_get_ms();
    set_led_power_on();

    
extern u32 g_KQQ_c_voltage;
    char sn[SN_SIZE+1];
    if (g_KQQ_c_voltage < LOW_BATT_VOLT) {
        lwl_app_mode_switch_to(APP_DEFAULT_MODE);
    } else if (get_SN(sn)) {
        log_i("have sn, go to power on mode");
        play_system_music("poweron.mp3");
        lwl_app_mode_switch_to(APP_NET_CONNECTING_MODE);
    } else {
        log_i("no sn, go to device check mode");
        lwl_app_mode_switch_to(APP_DEVICE_CHECK_MODE);
    }
    /* 注册lwl事件处理回调 */
    lwl_event_handler_add(power_on_input_event_handler_cb);
    return 0;
}

static int power_on_unint(void)
{
    lwl_event_handler_remove(power_on_input_event_handler_cb);
    return 0;
}

void app_power_on_mode(void)
{
    power_on_init();
    while (1) {
        if (lwl_app_mode_exitting()) {
            power_on_unint();
            return;
        }
        // 应用代码
        mdelay(10);
    }
}