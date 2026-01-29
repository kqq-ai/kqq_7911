
#include "update.h"
#include "app_mode/lwl_app_framework.h"
#include "app_mode/lwl_app_core.h"
#include "log.h"
#include "bt_ble/app_wifi_manage.h"
#include "LED/LED.h"

#define MOVE_TIMEOUT_MS 600000 // 10 minutes
static u32 init_time = 0;
int led_color = 0;
int progress = 0;

static void set_led_OTA_0(void){
    led_breath_start(COLOR_RED, COLOR_BLACK, 0, 400, 20);
    } /* 控制 led : 正在升级 红色 */
static void set_led_OTA_10(void){
    led_breath_start(COLOR_BLUE, COLOR_BLACK, 0, 200, 20);
    } /* 控制 led : 正在升级 蓝色 */
static void set_led_OTA_80(void){
    led_breath_start(COLOR_GREEN, COLOR_BLACK, 0, 200, 20);
    } /* 控制 led : 正在升级 绿色 */
static void set_led_connect_error(void)     { set_rgb_color(COLOR_RED); }    /* 控制 led : 网络连接失败 红色 */

//=================================================================================================
// LWL event callback
//=================================================================================================
static void update_event_handler_cb(lwl_event_t *e)
{
    static int en = 1;
    lwl_eend_dev_t *p_eend_dev = e->distance_change.pdev;

    switch (e->event_type) {
    case LWL_EVENT_TYPE_KEY:
        play_system_music("OtaInUpdate.mp3");
        break;
    case LWL_EVENT_TYPE_TOUCH_KEY:
        if (e->touch_key_cmd.touch_key_event_type == 0) {
            play_system_music("t_key.mp3");
        } else {
            play_system_music("OtaInUpdate.mp3");
        }
        break;
    }
}

static int update_shutdown(void)
{
    lwl_peri_dev_7016_on(false);
    disable_gsensor();
    set_rgb_ON(COLOR_BLACK, 20);
    power_set_soft_poweroff(0);
}

static int update_init(void)
{
    init_time = timer_get_ms();
    g_sensor_set_alive_timeout(MOVE_TIMEOUT_MS);
    set_led_OTA_0();
    led_color = 0;
    progress = -1;
    play_system_music("OtaInUpdate.mp3");
    lwl_event_handler_add(update_event_handler_cb);
    return 0;
}

static int update_unint(void)
{
    lwl_event_handler_remove(update_event_handler_cb);
    lwl_app_audio_play(CONFIG_CP_VOICE_FILE_PATH"OtaFailed.mp3", update_shutdown);
    sleep(5000);
    return 0;
}

void update_loop()
{
    if (WIFI_CONNECT_SUCC != get_wifi_connect_status(NULL)) {
        lwl_app_mode_switch_to(APP_NET_CONNECTING_MODE);
        set_led_connect_error();// WIFI CONNECT ERROR
        log_i("Network connection is WIFI_STA_DISCONNECT!\n");
        play_system_music("NetCfgFail.mp3");
        return;
    }
    if (lwl_app_audio_playing()) {
        msleep(100);
        return;
    }
    if (progress == -1) {
        do_OTA();
    }
    progress = get_download_progress();
    log_i("update_loop downloading >> %d%%\n", progress);
// OTA灯效：0-10%红色，10-80%蓝色，80-100%绿色
    if (progress <= 10 && led_color != 0) {
        set_led_OTA_0();   // 红色
        led_color = 0;
    } else if (progress > 10 && progress <= 80 && led_color != 1) {
        set_led_OTA_10();   // 蓝色
        led_color = 1;
    } else if (progress > 80 && led_color != 2) {
        set_led_OTA_80();   // 绿色
        led_color = 2;
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