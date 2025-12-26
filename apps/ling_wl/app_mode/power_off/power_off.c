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
#include "power_off.h"
#include "app_mode/lwl_app_framework.h"
#include "app_mode/lwl_app_core.h"
#include "app_mode/lwl_peri_dev.h"
#include "log.h"
#include "app_lfs.h"
#include "app_cookie.h"
#include "gpio.h"
#include "LED/LED.h"

#define KEY_PRESS_POWER_OFF_TIME_MS 1000 // 4s
#define WAIT_FOR_POWER_OFF_TIME_MS 5000 // 8s
static u32 init_time = 0;
static void set_led_power_off(void)     { set_rgb_ON(COLOR_RED, 20); }   /* 控制 led : 关机红色 */
static void set_led_bye(void)           { set_rgb_ON(COLOR_BLACK, 20); }

int led_off = 0;

static void power_off_init(void)
{
    log_i("Power off mode init\r\n");
    init_time = timer_get_ms();
    g_sensor_reset_alive_timer();
    
    /* 关闭 7016 */
    log_i("7016 OFF\n");
    lwl_peri_dev_7016_on(false);
    disable_gsensor();

    led_off = 0;
    
    u16 voltage = get_vbat_level();
    if (voltage < LOW_BATT_VOLT) {
        set_led_power_off();
        log_i("Battery low, skip PowerOff prompt and shutdown immediately\r\n");
        return;
    }
    int err = -1;
    lwl_app_context_t *context = lwl_app_core_context_get_instance();
    if (context && context->music_player) {
        log_i("Try play PowerOff prompt before shutdown\r\n");
        play_system_music("PowerOff.mp3");
    }
}

static void power_off_unint(void)
{
    log_i("Power off mode uninit\r\n");
}

void power_off_loop()
{
    g_sensor_reset_alive_timer();
    if (timer_get_ms() - init_time >= KEY_PRESS_POWER_OFF_TIME_MS) {
        if (!led_off) {
            led_off = 1;
            set_led_bye();
        }
    }
    if (!lwl_app_mode_playing()) {
        power_set_soft_poweroff();
    } else if (timer_get_ms() - init_time >= WAIT_FOR_POWER_OFF_TIME_MS) {
        power_set_soft_poweroff();
    }
}

void app_power_off_mode(void)
{
    power_off_init();
    while (1) {
        lwl_app_mode_play_loop();
        power_off_loop();
        if (lwl_app_mode_exitting()) {
            power_off_unint();
            return;
        }
        // 应用代码
        mdelay(10);
    }
}