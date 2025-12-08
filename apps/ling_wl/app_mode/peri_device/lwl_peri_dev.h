#ifndef _LWL_PERI_DEV_H_ 
#define _LWL_PERI_DEV_H_ 

#define KEY_IO_PORT 	IO_PORTC_09
#define GSENSOR_IO 		IO_PORTC_00
#define CHARGE_IN_IO 	IO_PORTH_07
#define CHARGE_FULL_IO 	IO_PORTA_06
#define TOUCH_IO 		IO_PORTC_06
#define WAKEUP_7016_IO  IO_PORTA_05

#define BTN_SCAN_INTERVAL       100
#define BTN_PRPESS_TIME         1500
#define BTN_RELEASE_TIME        500

#define GSENSOR_CHECK_INTERVAL  5000
#define GSENSOR_ALIVE_TIMEOUT (1 * 10 * 1000)   // 10秒，上层计数更长时间
#define TOUCH_KEY_CHECK_INTERVAL 100

#define BATTERY_CHECK_INTERVAL  1000
#define POWER_OFF_BATTERY_LEVEL 330

#define LED_INTERVAL            1500

#define POWER_KEY_LONG_PRESS_MS      3000  // 3秒长按
#define POWER_KEY_DEBOUNCE_MS        50    // 消抖时间

void lwl_peri_dev_loop(uint32_t curr_time);
void lwl_peri_dev_init(void);

// 清除函数声明
void key_scan_reset(void);
void g_sensor_reset(void);
void charge_detect_reset(void);
void touch_key_reset(void);
void battery_detect_reset(void);
void led_reset(void);
void lwl_peri_dev_reset_all(void);

/* 1:满足长按条件, 0:未满足或按键已释放 */
uint32_t check_long_press_to_power_on(void);
//调用 g_sensor_set_alive_timeout(...) 设置不移动的休眠超时时间（休眠后可移动唤醒）
void g_sensor_set_alive_timeout(uint32_t timeout_ms);
void g_sensor_reset_alive_timer(void);

void lwl_peri_dev_7016_on(bool on);

#endif