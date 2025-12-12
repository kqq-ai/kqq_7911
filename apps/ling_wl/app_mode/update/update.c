
#include "update.h"
#include "app_mode/lwl_app_framework.h"
#include "app_mode/lwl_app_core.h"
#include "log.h"
#include "app_lfs.h"
#include "protocol/cp/c_uart_p.h"
#include "bt_ble/app_wifi_manage.h"

#define MODE_TIMEOUT_MS 600000 // 10 minutes
static u32 init_time = 0;
int led_color = 0;

static void set_led_OTA_on(void)       { uart_send_set_led_color(COLOR_RED); } /* 控制 led : 正在升级 红色 */
static void set_led_OTA_10(void)       { uart_send_set_led_color(COLOR_BLUE); } /* 控制 led : 正在升级 蓝色 */
static void set_led_OTA_40(void)       { uart_send_set_led_color(COLOR_BLUE); } /* 控制 led : 正在升级 蓝色 */
static void set_led_OTA_70(void)       { uart_send_set_led_color(COLOR_GREEN); } /* 控制 led : 正在升级 绿色 */
static void set_led_connect_error(void)     { uart_send_set_led_color(COLOR_RED); }    /* 控制 led : 网络连接失败 红色 */

//=================================================================================================
// LWL event callback
//=================================================================================================
static void update_event_handler_cb(lwl_event_t *e)
{
    static int en = 1;
    lwl_eend_dev_t *p_eend_dev = e->distance_change.pdev;

    switch (e->event_type) {
    case LWL_EVENT_TYPE_TOUCH_KEY:
        // play 提示音
        break;
    case LWL_EVENT_TYPE_KEY:
        break;
    case LWL_EVENT_TYPE_UART_CLEAR_WIFI_SSID:
        // TODO STOP THE OTA
        break;
    }
}
static int update_init(void)
{
    init_time = timer_get_ms();
    g_sensor_reset_alive_timer();
    g_sensor_set_alive_timeout(MODE_TIMEOUT_MS);
    lwl_event_handler_add(update_event_handler_cb);
    set_led_OTA_on();
    led_color = 0;
    play_system_music("OtaInUpdate.mp3");
    
    while (lwl_app_mode_playing()) {
        msleep(10);
    }
    do_OTA();
    return 0;
}

static int update_unint(void)
{
    system_reset(); //restart
    lwl_event_handler_remove(update_event_handler_cb);
    return 0;
}
void update_loop()
{
    log_i("update_loop init\n");
    if (WIFI_CONNECT_SUCC != get_wifi_connect_status(NULL)) {
        lwl_app_mode_switch_to(APP_NET_CONNECTING_MODE);
        set_led_connect_error();// WIFI CONNECT ERROR
        printf("Network connection is WIFI_STA_DISCONNECT!\n");
        play_system_music("NetCfgFail.mp3");
        return;
    }
    int progress = get_download_progress();
    log_i("update_loop downloading >> %d%%\n", progress);
    if (progress > 10 && progress < 40 && led_color < 1) {
        set_led_OTA_10();// OTA IN PROGRESS
        led_color = 1;
    } else if (progress >= 40 && progress < 70 && led_color < 2) {
        set_led_OTA_40();// OTA IN PROGRESS
        led_color = 2;
    } else if (progress >= 70 && progress < 100 && led_color < 3) {
        set_led_OTA_70();// OTA IN PROGRESS
        led_color = 3;
    }
    msleep(1000);
}

void app_update_mode(void)
{
    update_init();

    while (1) {
        if (lwl_app_mode_exitting()) {
            update_unint();
            return;
        }
        // 应用代码
        update_loop();
        mdelay(10);
    }
}