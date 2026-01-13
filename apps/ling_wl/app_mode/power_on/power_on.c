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
#include "app_cookie.h"
#include "gpio.h"
#include "app_mode/lwl_peri_dev.h"
#include "LED/LED.h"

#define MOVE_TIMEOUT_MS 60000 // 60s
#define OTA7016_KEY_PRESS_TIME_MS 8000 // 8+2s
static u32 init_time = 0;
static bool c_7016_power_on = false;
static void set_led_power_on(void)         { set_rgb_color(COLOR_CYAN); }
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

static int power_on_init(void)
{
    init_time = timer_get_ms();
    g_sensor_reset_alive_timer();
    g_sensor_set_alive_timeout(MOVE_TIMEOUT_MS);
    set_led_power_on();

    u16 voltage = get_vbat_level();
    c_7016_power_on = 0;

    
    if(system_reset_reason_get() == 0x80) { // SYS_RST_LONG_PRESS,长按复位清除wifi并再次复位
        log_i("Long press reset detected, clear wifi and  Reset.\n");
        // Reset WiFi SSID
        app_del_stored_wifi_ssid(); // 清除设备保存过的所有wifi
        //增加音效提示
        play_system_music("reset_wifi.mp3");
        lwl_app_audio_play_loop();
        mdelay(3000);
        system_reset(); //restart
        return 0;
    }
    
    int charge_in = gpio_read(CHARGE_IN_IO);
    int charge_full = gpio_read(CHARGE_FULL_IO);
    log_i("[power_on]voltage=%d, charge_in=%d, charge_full=%d", voltage, charge_in, charge_full);
    if (charge_in == 1) {
        log_i("CHARGING IN, GO TO DEFAULT MODE");
        lwl_app_mode_switch_to(APP_DEFAULT_MODE);
    } else if (voltage > LOW_BATT_VOLT) {
        play_system_music("poweron.mp3");
    }
    return 0;
}

static int power_on_unint(void)
{
    return 0;
}

void power_on_loop()
{
    uint8_t btnCurrState = gpio_read(KEY_IO_PORT);
    extern uint8_t g_disable_turn_off;
    if (btnCurrState == 0 && g_disable_turn_off == 2) {
        // 提前2s打开7016，转紫灯提示准备OTA模式，只有2s窗口期
        if (c_7016_power_on == false && timer_get_ms() - init_time >= OTA7016_KEY_PRESS_TIME_MS - 2000) {
            c_7016_power_on = true;
            lwl_peri_dev_7016_on(true);  // 开启 7016
            set_led_ota_on();
        }
        // 保持长按8s，进入device check
        if (c_7016_power_on == true && timer_get_ms() - init_time >= OTA7016_KEY_PRESS_TIME_MS) {
            log_i("LONG PRESS KEY TO DEVICE CHECK MODE");
            lwl_app_mode_switch_to(APP_DEVICE_CHECK_MODE);
            set_led_ota_off();
        }
        g_sensor_reset_alive_timer();
    }
    if (btnCurrState == 0) return;
    // 7016在6s内已经开机（亮紫灯），但未到8s进入device check模式，此时保持紫灯，进行7016C的OTA
    if (c_7016_power_on) {
        log_i("RELEASE OTA7016 KEY TO ENTER DEVICE CHECK MODE");
        msleep(2000);           // 等待 7016 启动完成
        uart_send_audio_dev_ota();
        lwl_app_mode_switch_to(APP_DEVICE_CHECK_MODE);
        return;
    }
    if (c_7016_power_on) set_led_ota_off();
    u16 voltage = get_vbat_level();
    char sn[SN_SIZE+1];
    // 低电压进入低电量和红灯提醒，有SN进入应用，没有SN进入device check获取SN
    if (voltage < LOW_BATT_VOLT) {
        log_i("LOW BATTERY VOLTAGE, GO TO DEFAULT MODE");
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
        lwl_app_audio_play_loop();
        if (lwl_app_mode_exitting()) {
            power_on_unint();
            return;
        }
        power_on_loop();
        // 应用代码
        mdelay(10);
    }
}