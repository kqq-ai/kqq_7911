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
#include "default.h"
#include "../lwl_app_framework.h"
#include "log.h"
#include "app_cookie.h"
#include "app_mode/lwl_app_core.h"
#include "LED/LED.h"
#include "gpio.h"
#include "app_mode/lwl_peri_dev.h"

#include "./bt_ble/app_wifi_manage.h"
#include "net_music_play_my.h"
#define MODE_TIMEOUT_MS 5000 // 5s
#define MOVE_TIMEOUT_MS 3000 // 3s
static u32 init_time = 0;
bool wait_for_button = false;

// 低电量显示红灯
// 充电中显示黄灯（<80%），充电完成显示绿灯（>=80%）
// 正常使用中显示白灯
static void set_led_idel(void)
{
    set_rgb_color(COLOR_WHITE); //空闲时灯光变白色
}

static void set_led_low_batt(void)  { set_rgb_color(COLOR_RED); }      //低电量时灯光变红色 

static void set_led_charging(void)  { set_rgb_color(COLOR_YELLOW); }   //充电中时灯光变黄色
static void set_led_charged(void)   { set_rgb_color(COLOR_GREEN); }    //充电完成时灯光变绿色
static void set_led_idle(void)      { set_rgb_color(COLOR_WHITE); }    //空闲时灯光变白色
//=================================================================================================
// LWL event callback
//=================================================================================================
static void default_input_event_handler_cb(lwl_event_t *e)
{
    static int en = 1;
    lwl_eend_dev_t *p_eend_dev = e->distance_change.pdev;

    switch (e->event_type) {
    case LWL_EVENT_TYPE_TOUCH_KEY:
        // play 提示音
        if (e->touch_key_cmd.touch_key_event_type == 0) {
            log_i("Touch key pressed down");
            wait_for_button = false;
        }
        break;
    case LWL_EVENT_TYPE_KEY:
        log_i("Power key event received");
        wait_for_button = false;
        break;
    case LWL_EVENT_TYPE_GSENSOR_IDLE:
        break;
    case LWL_EVENT_TYPE_CHARGE:
        // 根据事件更新充电指示灯状态
        if (e->charge_cmd.charge_event_type == CHARGE_EVENT_TYPE_CHARGE_IN) {
            set_led_charging();
        }
        else if (e->charge_cmd.charge_event_type == CHARGE_EVENT_TYPE_CHARGE_OUT) {
            set_led_idle();
            // u16 voltage = get_vbat_level();
            // if (voltage < LOW_BATT_VOLT) {
            //     set_led_low_batt();
            // } else {
            //     set_led_idle();
            // }
        }
        else if (e->charge_cmd.charge_event_type == CHARGE_EVENT_TYPE_CHARGE_FULL) {
            set_led_charged();
        }
        break;
    }
}

static int default_init(void)
{
    lwl_app_context_t *context = lwl_app_core_context_get_instance();
    /* 注册lwl事件处理回调 */
    init_time = timer_get_ms();
    g_sensor_set_alive_timeout(MOVE_TIMEOUT_MS);
    lwl_event_handler_add(default_input_event_handler_cb);

    u16 voltage = get_vbat_level();
    
    int charge_in = gpio_read(CHARGE_IN_IO);
    int charge_full = gpio_read(CHARGE_FULL_IO);
    log_i("[default_mode]voltage=%d, charge_in=%d, charge_full=%d", voltage, charge_in, charge_full);
    if (charge_in) {
        if (charge_full) {
            set_led_charged();
        } else {
            set_led_charging();
        }
        play_system_music("Charging.mp3");
    } else {
        if (voltage < LOW_BATT_VERY_SHUTDOWN) {
            set_led_idle();
            wifi_and_network_off();
            // play_system_music("LowBattery.mp3");
        } else {
            set_led_idle();
        }
    }

    wait_for_button = true;
    return 0;
}

static int default_unint(void)
{
    /* 取消注册lwl事件处理回调 */
    lwl_event_handler_remove(default_input_event_handler_cb);
    return 0;
}

void app_default_loop()
{
    int charge_in = gpio_read(CHARGE_IN_IO);
    u16 voltage = get_vbat_level();
    if (!wait_for_button) {
        if (charge_in || voltage > LOW_BATT_VOLT) {
            play_system_music("poweron.mp3");
            lwl_app_mode_switch_to(APP_NET_CONNECTING_MODE);
        } else {
            play_system_music("LowBattery.mp3");
        }
        wait_for_button = true;
    }
    if (charge_in) {
        g_sensor_reset_alive_timer();
    } else { // 超时关机，低电量避免反复关机
        if (timer_get_ms() - init_time >= MODE_TIMEOUT_MS) {
            if (voltage < LOW_BATT_VERY_SHUTDOWN) {
                log_i("Low battery timeout, shutdown immediately\r\n");
                disable_gsensor();// 低电量避免晃动反复唤醒设备，需要关机，而不是休眠
            }
            lwl_app_mode_switch_to(APP_POWER_OFF_MODE);
        }
    }
}

void app_default_mode(void)
{
    default_init();
    //mdelay(2000);
    while (1) {
        if (lwl_app_mode_exitting()) {
            default_unint();
            return;
        }
        app_default_loop();
        mdelay(100);
    }
}