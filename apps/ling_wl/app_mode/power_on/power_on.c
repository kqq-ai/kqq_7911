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
#include "gpio.h"
#include "../lwl_peri_dev.h"
#include "LED/LED.h"

#define MODE_TIMEOUT_MS 120000 // 120s
#define OTA7016_KEY_PRESS_TIME_MS 8000 // 8+2s
static u32 init_time = 0;
static bool c_7016_power_on = false;
static void set_led_power_on(void)         { set_rgb_color(COLOR_WHITE); }
static void set_led_ota_on(void)           { set_rgb_ON(COLOR_PURPLE, 20); }
static void set_led_ota_off(void)          { set_rgb_OFF(COLOR_PURPLE, 20); }

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
    }
}

static int power_on_init(void)
{
    init_time = timer_get_ms();
    g_sensor_reset_alive_timer();
    g_sensor_set_alive_timeout(MODE_TIMEOUT_MS);
    set_led_power_on();

    u16 voltage = get_vbat_level();
    c_7016_power_on = 0;
    
    int charge_in = gpio_read(CHARGE_IN_IO);
    int charge_full = gpio_read(CHARGE_FULL_IO);
    log_i("[power_on]voltage=%d, charge_in=%d, charge_full=%d", voltage, charge_in, charge_full);
    if (voltage > LOW_BATT_VOLT) {
        play_system_music("poweron.mp3");
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

void power_on_loop()
{
    uint8_t btnCurrState = gpio_read(KEY_IO_PORT);
    if (btnCurrState == 0) {
        if (c_7016_power_on == false && timer_get_ms() - init_time >= OTA7016_KEY_PRESS_TIME_MS - 2000) {
            c_7016_power_on = true;
            lwl_peri_dev_7016_on(true);  // 开启 7016
            set_led_ota_on();
        }
        if (c_7016_power_on == true && timer_get_ms() - init_time >= OTA7016_KEY_PRESS_TIME_MS) {
            log_i("LONG PRESS KEY TO DEVICE CHECK MODE");
            lwl_app_mode_switch_to(APP_DEVICE_CHECK_MODE);
            set_led_ota_off();
        }
        g_sensor_reset_alive_timer();
        return;
    }
    if (c_7016_power_on) {
        log_i("RELEASE OTA7016 KEY TO ENTER DEVICE CHECK MODE");
        msleep(2000);           // 等待 7016 启动完成
        uart_send_audio_dev_ota();
        // set_led_ota_off();
        lwl_app_mode_switch_to(APP_DEVICE_CHECK_MODE);
        return;
    }
    u16 voltage = get_vbat_level();
    int charge_in = gpio_read(CHARGE_IN_IO);
    char sn[SN_SIZE+1];
    if (voltage < LOW_BATT_VOLT) {
        log_i("LOW BATTERY VOLTAGE, GO TO DEFAULT MODE");
        lwl_app_mode_switch_to(APP_DEFAULT_MODE);
    } else if (charge_in == 1) {
        log_i("CHARGING IN, GO TO DEFAULT MODE");
        lwl_app_mode_switch_to(APP_DEFAULT_MODE);
    } else if (get_SN(sn)) {
        log_i("have sn, go to power on mode");
        lwl_app_mode_switch_to(APP_NET_CONNECTING_MODE);
    } else {
        log_i("no sn, go to device check mode");
        lwl_app_mode_switch_to(APP_DEVICE_CHECK_MODE);
    }
}

void app_power_on_mode(void)
{
    power_on_init();
    while (1) {
        power_on_loop();
        if (lwl_app_mode_exitting()) {
            power_on_unint();
            return;
        }
        // 应用代码
        mdelay(10);
    }
}