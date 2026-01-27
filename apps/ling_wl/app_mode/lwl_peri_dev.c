#include "app_config.h"
#include "app_cookie.h"
#include "system/includes.h"
#include "device/device.h"
#include "gpio.h"
#include "uart.h"
#include "log.h"
#include "../LED/LED.h"
#include "asm/pwm.h"

#include "app_mode/lwl_peri_dev.h"
#include "app_mode/lwl_app_core.h"
#include "asm/power_interface.h"

static void set_led_touch_on(void)          { set_rgb_ON(COLOR_YELLOW,10);  }
static void set_led_touch_off(void)         { set_rgb_OFF(COLOR_YELLOW,10); }
static void set_led_normal_key_on(void)     { set_rgb_ON(COLOR_YELLOW,11);  }
static void set_led_normal_key_off(void)    { set_rgb_OFF(COLOR_YELLOW,11); }
static void set_led_rst_key_on(void)        { set_rgb_ON(COLOR_RED,15);     }
static void set_led_rst_key_off(void)       { set_rgb_OFF(COLOR_RED,15);    }

static uint32_t g_rst_key_scan_timer = 0;

static uint32_t g_key_scan_timer = 0;
static uint8_t  g_key_scan_ongoing = 0;
static uint8_t  g_key_action_start = 0;
static uint32_t g_key_press_start_time = 0;
static uint32_t g_key_release_start_time = 0;
static uint32_t g_key_latest_state = 0;             // 上一帧按键按下：0 上一帧按键未按下：1
static uint32_t g_key_click_counter = 0;

static uint32_t g_gsensor_alive_timer = 0;
static uint32_t g_gsensor_user_alive_timeout = ( 1 * 60 * 1000 );   // 由业务设置的无移动后休眠时间(ms)
static uint32_t g_gsensor_last_notify_time = 0;     // 上次通知业务的时间（用于周期性通知）

static uint8_t g_touch_key_event_active = 0;
static uint8_t g_touch_key_event_active_priv = 1;   // 上一帧按键按下：0 上一帧按键未按下：1
static uint32_t g_touch_key_timer = 0;

static bool is_initialized = false; // 开关
static uint32_t prev_charge_in = 0;
static uint32_t prev_charge_full = 0;
static uint32_t charge_timer = 0;

static void *pwm_dev_handle = NULL;
static void *pwm_dev_handle_g = NULL;


uint8_t g_disable_turn_off = 1;

//==KEY SCAN=========================================================================================
/**
 * @brief 清除按键扫描状态
 */
void lwl_peri_dev_key_scan_reset(void)
{
    g_key_scan_ongoing = 0;
    g_key_action_start = 0;
    g_key_press_start_time = 0;
    g_key_release_start_time = 0;
    g_key_latest_state = 1;
    g_key_click_counter = 0;
    g_key_scan_timer = 0;
}

//==G-SENSOR LOOP=========================================================================================
/**
 * @brief 清除G-Sensor状态
 */
void lwl_peri_dev_g_sensor_reset(void)
{
    g_gsensor_alive_timer = 0;
}

//==CHARGE LOOP=========================================================================================
/**
 * @brief 清除充电检测状态
 */
void lwl_peri_dev_charge_detect_reset(void)
{
    is_initialized = false;
    prev_charge_in = 0;
    prev_charge_full = 0;
    charge_timer = 0;
}

//==Touch-key=========================================================================================
/**
 * @brief 清除触摸按键状态
 */
void lwl_peri_dev_touch_key_reset(void)
{
    g_touch_key_event_active = 0;
    g_touch_key_event_active_priv = 1;
    g_touch_key_timer = 0;
}

//==battery detection LOOP=========================================================================================
/**
 * @brief 清除电量检测状态
 */
void lwl_peri_dev_battery_detect_reset(void)
{
    // 电量检测模块主要使用静态局部变量
    // 重置函数用于未来扩展
}

//==模块管理器=========================================================================================
/**
 * @brief 清除所有外设模块状态
 */
void lwl_peri_dev_reset_all(void)
{
    lwl_peri_dev_key_scan_reset();
    lwl_peri_dev_g_sensor_reset();
    lwl_peri_dev_charge_detect_reset();
    lwl_peri_dev_touch_key_reset();
    lwl_peri_dev_battery_detect_reset();
}

//==KEY SCAN=========================================================================================
/**
 * @brief 当发生按键按下的回调
 */
static void on_key_down(void)
{
    log_i("[KEY SCAN]key down");
}

/**
 * @brief 当发生按键抬起的回调
 */
static void on_key_up(void)
{
    log_i("[KEY SCAN]key up");
}

/**
 * @brief 当发生按键短按时的回调
 */
static void on_key_short_press(void)
{
    // gpio_set_direction(WAKEUP_7016_IO, 0); // 输出模式
    // gpio_set_pull_down(WAKEUP_7016_IO, 0);
    // gpio_set_pull_up(WAKEUP_7016_IO, 0);   
    // gpio_direction_output(WAKEUP_7016_IO, 1);
    // gpio_write(WAKEUP_7016_IO, 0);
    // log_i("[KEY SCAN]Short press");
}

/**
 * @brief 当发生按键长按时的回调
 */
static void on_key_long_press(void)
{
    // log_i("[KEY SCAN]Long press -> 7911 soft poweroff!\n");
    lwl_peri_dev_7016_on(false);
    disable_gsensor();
    set_rgb_ON(COLOR_BLACK, 20);
    power_set_soft_poweroff(0);
    
}

/**
 * @brief 当发生按键双击的回调
 */
static void on_key_double_click(void)
{
    // log_i("[KEY SCAN]Double press");
}

/**
 * @brief 当发生按键三击的回调
 */
static void on_key_triple_click(void)
{
    gpio_set_direction(WAKEUP_7016_IO, 0); // 输出模式
    gpio_set_pull_down(WAKEUP_7016_IO, 0);
    gpio_set_pull_up(WAKEUP_7016_IO, 0);   
    gpio_direction_output(WAKEUP_7016_IO, 1);
    gpio_direction_output(WAKEUP_7016_IO, 0);
    log_i("[KEY SCAN]Triple press");
}

/**
 * @brief 当发生按键四击的回调
 */
static void on_key_quadruple_click(void)
{
    log_i("[KEY SCAN]Quadruple press");
}

/**
 * @brief 启用按键扫描
 */
static void key_scan_start(void)
{
    g_key_scan_ongoing = 0;
    g_key_press_start_time = 0;
    g_key_latest_state = 0;       // 默认为按下开机
    g_key_scan_ongoing = true;
}

static void key_scan_loop(uint32_t current_time_ms)
{
    uint32_t g_curr_time = current_time_ms;
    if (g_curr_time - g_key_scan_timer > BTN_SCAN_INTERVAL) {
        if (!g_key_scan_ongoing) {
            return;
        }
        
        uint8_t btnCurrState = gpio_read(KEY_IO_PORT);

        uint8_t hasFlip = g_key_latest_state != btnCurrState;

        // 按电源键开机忽略第一次按键
        if (g_disable_turn_off) {
            if (btnCurrState == 0) {
                g_key_scan_timer = g_curr_time;
            } else {
                g_disable_turn_off = 0;
                g_key_latest_state = btnCurrState;
            }
            return;
        }
        if (hasFlip) {
            if (btnCurrState == 0) {
                // on_key_down();
                set_led_normal_key_on();
            } else {
                // on_key_up();
                set_led_normal_key_off();
            }  
        }
        
        if (btnCurrState == 0) {
            if (hasFlip && !g_key_action_start) {
                g_key_action_start = true;          // 开始
                g_key_press_start_time = 0;
                g_key_release_start_time = 0;
                g_key_click_counter = 0;
            }

            if (g_key_action_start) {
                if (hasFlip) {
                    g_key_press_start_time = g_curr_time;
                    g_key_release_start_time = 0;
                }

                /* 检测按下时间 */
                uint32_t duration = g_curr_time - g_key_press_start_time;
                if (duration >= BTN_PRPESS_TIME)
                { 
                    // 按下时间较长
                    log_i("[KEY SCAN]long press\n");
                    lwl_event_t e;
                    // e.event_type = LWL_EVENT_TYPE_KEY;
                    // e.key_cmd.key_event_type = KEY_EVENT_TYPE_LONG_PRESS;
                    e.event_type = LWL_EVENT_TYPE_SHUTDOWN;
                    lwl_event_notify(&e);
                    g_key_action_start = false;     // 结束
                }
            }

            if (g_curr_time - g_key_press_start_time > BTN_PRPESS_TIME * 3) {
                on_key_long_press();
            }
        } else {    // if (btnCurrState == 1)
            if (g_key_action_start) {
                if (hasFlip) {
                    g_key_release_start_time = g_curr_time;
                    g_key_press_start_time = 0;
                    g_key_click_counter++;
                }

                /* 检测松开时间 */
                uint32_t duration = g_curr_time - g_key_release_start_time;
                if (duration >= BTN_RELEASE_TIME)
                { // 松开时间较长
                    if (g_key_click_counter == 1)
                    {
                        log_i("[KEY SCAN]short press\n");
                        lwl_event_t e;
                        e.event_type = LWL_EVENT_TYPE_KEY;
                        e.key_cmd.key_event_type = KEY_EVENT_TYPE_SHORT_CLICK;
                        lwl_event_notify(&e);
                        on_key_short_press();
                    }
                    else if (g_key_click_counter == 2)
                    {
                        log_i("[KEY SCAN]double press\n");
                        lwl_event_t e;
                        e.event_type = LWL_EVENT_TYPE_KEY;
                        e.key_cmd.key_event_type = KEY_EVENT_TYPE_DOUBLE_CLICK;
                        lwl_event_notify(&e);
                        on_key_double_click();
                    }
                    else if (g_key_click_counter == 3)
                    {
                        log_i("[KEY SCAN]triple press\n");
                        lwl_event_t e;
                        e.event_type = LWL_EVENT_TYPE_KEY;
                        e.key_cmd.key_event_type = KEY_EVENT_TYPE_TRIPLE_CLICK;
                        lwl_event_notify(&e);
                        on_key_triple_click();
                    }
                    else if (g_key_click_counter == 4)
                    {
                        on_key_quadruple_click();
                    }
                    g_key_action_start = false;     // 结束
                }
            }
        }
        g_key_latest_state = btnCurrState;
        
        g_key_scan_timer = g_curr_time;
    }

}

static void rst_key_loop(uint32_t current_time_ms)
{
    static uint8_t rst_key_latest_state = 1;
    static uint8_t flip_times_rst_key = 0;
    if (current_time_ms - g_rst_key_scan_timer > BTN_SCAN_INTERVAL) {
        g_rst_key_scan_timer = current_time_ms;
        
        uint8_t rstbtnCurrState = gpio_read(RST_KEY_PORT);
        //对rstbtnCurrState去抖动，然后再判断按下和释放
        
        uint8_t hasFlip = rst_key_latest_state != rstbtnCurrState;
        if( hasFlip ) {
            flip_times_rst_key++;
            //log_i("[KEY SCAN] flip_times_rst_key \n");
            if( flip_times_rst_key <= 2 ) {
                return;
            }
        } else {
            flip_times_rst_key = 0;
            return;
        }

        //连续三次，确认状态变化才认为有效
        flip_times_rst_key = 0; //清零
        if ( rstbtnCurrState == 0) {
            set_led_rst_key_on();
            log_i("[KEY SCAN]RST key pressed\n");
            lwl_event_t e;
            e.event_type = LWL_EVENT_TYPE_RST_KEY;
            e.key_cmd.key_event_type = 0; // RST key pressed
            if (g_disable_turn_off == 1) g_disable_turn_off = 2;//短按进入设备管理或7016OTA，只适用于开机8s内阶段
            lwl_event_notify(&e);
        } else {
            set_led_rst_key_off();
            log_i("[KEY SCAN]RST key released\n");
            lwl_event_t e;
            e.event_type = LWL_EVENT_TYPE_RST_KEY;
            e.key_cmd.key_event_type = 1; // RST key released
            lwl_event_notify(&e);
        }
        rst_key_latest_state = rstbtnCurrState;
    }
}

//==KEY SCAN=========================================================================================

//==G-SENSOR LOOP=========================================================================================

//调用 g_sensor_set_alive_timeout(...) 设置真正的休眠超时时间
void g_sensor_set_alive_timeout(uint32_t timeout_ms)
{
    g_gsensor_user_alive_timeout = timeout_ms;
    g_gsensor_alive_timer = 0;
    log_i("G-Sensor: user alive timeout set to %u ms\n", timeout_ms);
}
static uint32_t g_gsensor_timer = 0;

//清除活跃计时，重新计算时间
void g_sensor_reset_alive_timer(void)
{
    g_gsensor_alive_timer = 0;
}

static void g_sensor_loop(uint32_t curr_time)
{
    const uint32_t GSENSOR_CHECK_MS = 10; // 每10ms检查一次

    if (curr_time - g_gsensor_timer < GSENSOR_CHECK_MS) {
        return;
    }
    g_gsensor_timer = curr_time;

    int r = gpio_read(GSENSOR_IO);
    // r == 0 表示有中断/移动
    if (!r) {
        // 发现移动，重置活跃计时
        g_gsensor_alive_timer = curr_time;
        g_gsensor_last_notify_time = 0; // 重置周期通知，下一次从0重新计时
        // 可选打印
        // log_i("G-Sensor: movement detected, reset alive timer\n");
        return;
    }

    // 如果还未初始化过 alive_timer，先设置为当前时间并返回，避免启动/重置后立即触发超时
    if (g_gsensor_alive_timer == 0) {
        g_gsensor_alive_timer = curr_time;
        return;
    }

    // 没有移动时，分两种通知策略：
    // 1) 业务层未设置超时（g_gsensor_user_alive_timeout == 0）：每隔 GSENSOR_ALIVE_TIMEOUT 向业务层发送一次事件，提示业务设置休眠时间
    // 2) 业务层设置了超时：当从最后一次移动算起超过业务设置的超时后，发送一次事件触发休眠
    uint32_t elapsed_since_move = curr_time - g_gsensor_alive_timer;

    // 周期性通知业务，间隔使用 GSENSOR_ALIVE_TIMEOUT 常量
    if (g_gsensor_last_notify_time == 0 || (curr_time - g_gsensor_last_notify_time) >= GSENSOR_ALIVE_TIMEOUT) {
        log_i("G-Sensor: no movement, notify application (elapsed %u ms)\n", elapsed_since_move);
        lwl_event_t e;
        e.event_type = LWL_EVENT_TYPE_GSENSOR_IDLE;
        // 业务层收到该事件后，可调用 g_sensor_set_alive_timeout(...) 设置真正的休眠超时时间
        lwl_event_notify(&e);
        g_gsensor_last_notify_time = curr_time;
    }
    // 判断是否应该发送通知（避免频繁重复发送）
    if (g_gsensor_user_alive_timeout > 0) {
        // 按业务设置的超时通知一次并且避免重复
        if (elapsed_since_move >= g_gsensor_user_alive_timeout) {
            // 发送关机通知（仅发送一次）
            log_i("❌ G-Sensor: no movement, trigger sleep (elapsed %u ms)\n", elapsed_since_move);
            lwl_event_t e;
            e.event_type = LWL_EVENT_TYPE_SLEEP;
            lwl_event_notify(&e);
            // 避免重复发送：记录上次通知时间
            g_gsensor_last_notify_time = curr_time;
            g_sensor_reset_alive_timer(); // 重置计时，避免重复触发
        }
    }
}

//==CHARGE LOOP=========================================================================================

typedef enum {
    CHARGE_STATE_IDLE,
    CHARGE_STATE_CHARGING,
    CHARGE_STATE_FULL
} ChargeState;

static ChargeState detect_charge_state(int charge_in, int charge_full)
{
    if (charge_in == 1 && charge_full == 0) {
        return CHARGE_STATE_CHARGING;
    } else if (charge_in == 1 && charge_full == 1) {
        return CHARGE_STATE_FULL;
    } else {
        return CHARGE_STATE_IDLE;
    }
}

static void charge_loop(uint32_t curr_time)
{
   if (curr_time - charge_timer < GSENSOR_CHECK_INTERVAL) {
        return;
    }
    
    charge_timer = curr_time;

    int charge_in = gpio_read(CHARGE_IN_IO);
    int charge_full = gpio_read(CHARGE_FULL_IO);
    
    // 使用文件级的 is_initialized（不要在函数内重新声明）
    if (!is_initialized) {
        prev_charge_in = charge_in;
        prev_charge_full = charge_full;
        is_initialized = true;
        return;
    }

    // 检测状态变化
    bool charge_in_changed = (prev_charge_in != charge_in);
    bool charge_full_changed = (prev_charge_full != charge_full);
    
    // 如果没有变化则提前返回
    if (!charge_in_changed && !charge_full_changed) {
        return;
    }
    ChargeState prev_state = detect_charge_state(prev_charge_in, prev_charge_full);
    ChargeState curr_state = detect_charge_state(charge_in, charge_full);

    // 状态转移处理
    switch (prev_state)
    {
    case CHARGE_STATE_IDLE:
        if (curr_state == CHARGE_STATE_CHARGING)
        {
            log_i("start charging!\n");
            // 发送充电开始事件通知
            lwl_event_t e;
            e.event_type = LWL_EVENT_TYPE_CHARGE;
            e.charge_cmd.charge_event_type = CHARGE_EVENT_TYPE_CHARGE_IN;
            lwl_event_notify(&e);
        }
        if (curr_state == CHARGE_STATE_FULL)
        {
            log_i("charge_in & _full!\n");
            // 充电器但满事件
        }
        break;
    case CHARGE_STATE_CHARGING:
        if (curr_state == CHARGE_STATE_IDLE)
        {
            log_i("charging stopped!\n");
            // 发送充电停止事件通知
            lwl_event_t e;
            e.event_type = LWL_EVENT_TYPE_CHARGE;
            e.charge_cmd.charge_event_type = CHARGE_EVENT_TYPE_CHARGE_OUT;
            lwl_event_notify(&e);
        }
        else if (curr_state == CHARGE_STATE_FULL)
        {
            log_i("charge full!\n");
            // 发送充电满事件通知
            lwl_event_t e;
            e.event_type = LWL_EVENT_TYPE_CHARGE;
            e.charge_cmd.charge_event_type = CHARGE_EVENT_TYPE_CHARGE_FULL;
            lwl_event_notify(&e);
        }
        break;
    case CHARGE_STATE_FULL:
        if (curr_state == CHARGE_STATE_IDLE)
        {
            log_i("battery removed from full charge\n");
            // 可选：发送相应事件
        }
        break;
    }
    // 更新前一个状态值
    prev_charge_in = charge_in;
    prev_charge_full = charge_full;
}

//==battery detection LOOP=========================================================================================
static void battery_detection_loop(uint32_t curr_time)
{
    static uint32_t battery_timer = 0;
    if (curr_time - battery_timer >= BATTERY_CHECK_INTERVAL) {
        battery_timer = curr_time;
        int charge_in = gpio_read(CHARGE_IN_IO);
        u16 voltage = adc_get_voltage(AD_CH_PA07) * 2 / 10;//
        u16 vBat_level = get_vbat_level();
        log_i("voltage chargeIn: %d ,  IO07: %d, vBat: %d\n",gpio_read(CHARGE_IN_IO), voltage, vBat_level);
        if (!charge_in && voltage < LOW_BATT_VERY_SHUTDOWN)
        {
            log_i("low battery!\n");
            // 发送低电压事件通知
            lwl_event_t e;
            e.event_type = LWL_EVENT_TYPE_LOW_BATTERY;
            lwl_event_notify(&e);
        }
    }
}


//==Touch-key=========================================================================================
void lwl_set_touch_key_active() 
{
    g_touch_key_event_active = 1;
}
void touch_key_loop(uint32_t curr_time)
{
    if (curr_time - g_touch_key_timer >= TOUCH_KEY_CHECK_INTERVAL) {
        g_touch_key_timer = curr_time;
        int ret = gpio_read(TOUCH_IO);
        // printf("Touch key: %d\r\n",ret);
        g_touch_key_event_active = ret;
        /* 如果上一帧为0，下一帧为1，表示触发touch_key按下 */
        if (g_touch_key_event_active_priv == 0 && g_touch_key_event_active == 1) {
            log_i("Touch key release!");
            set_led_touch_off();
            lwl_event_t e;
            e.event_type = LWL_EVENT_TYPE_TOUCH_KEY;
            e.touch_key_cmd.touch_key_event_type = 0x01;
            lwl_event_notify(&e);
        }
        else if (g_touch_key_event_active_priv == 1 && g_touch_key_event_active == 0)
        {
            log_i("Touch key press!");
            set_led_touch_on();
            lwl_event_t e;
            e.event_type = LWL_EVENT_TYPE_TOUCH_KEY;
            e.touch_key_cmd.touch_key_event_type = 0x00;
            lwl_event_notify(&e);
        }
        g_touch_key_event_active_priv = g_touch_key_event_active;
        g_touch_key_event_active = 0;
    }
}
//==Touch-key=========================================================================================

/*== 开机 检测 ===================================================================================*/
/**
 * @brief 检查是否满足长按开机条件
 * @return 1:满足长按条件, 0:未满足或按键已释放
 * @note 
 */
uint32_t check_long_press_to_power_on(void)
{
    u32 start_time;
    u32 current_time;
    u32 press_duration = 0;
    u32 last_print_time = 0;
    const u32 LONG_PRESS_INTERVAL_MS = 500;     // 信息打印间隔
    const u32 LOOP_CHECK_INTERVAL_MS = 50;      // 循环检测间隔
    const u32 DEBOUNCE_MS = 50;                 // 消抖时间
    /* 按键初始化 */
    gpio_set_direction(KEY_IO_PORT, 1); // 默认输入模式
    gpio_set_pull_down(KEY_IO_PORT, 0);
    gpio_set_pull_up(KEY_IO_PORT, 1);   // 默认上拉
    gpio_set_die(KEY_IO_PORT, 1);
    gpio_set_dieh(KEY_IO_PORT, 1);
    printf("[Power] Checking long press for power on (%dms required)...\n", POWER_KEY_LONG_PRESS_MS);
    
    // 初始消抖，确认按键确实被按下
    start_time = timer_get_ms();
    while ((timer_get_ms() - start_time) < DEBOUNCE_MS) {
        if (gpio_read(KEY_IO_PORT)) {  // 按键释放了
            printf("[Power] Key release during debounce, cancelled.\n");
            return 0;
        }
        mdelay(10);
    }
    
    // 重新计时正式开始的长按检测
    start_time = timer_get_ms();
    
    while (1) {
        // 检查按键是否仍然按下
        if (gpio_read(KEY_IO_PORT)) {
            printf("[Power] Key released after %dms, power on cancelled.\n", press_duration);
            return 0;
        }
        
        // 计算按压时长
        current_time = timer_get_ms();
        press_duration = current_time - start_time;
        
        // 检查是否达到长按时间
        if (press_duration >= POWER_KEY_LONG_PRESS_MS) {
            printf("[Power] Long press for %dms detected, powering on.\n", press_duration);
            return 1;
        }
        
        // 周期性打印进度（减少打印频率）
        if (current_time - last_print_time >= LONG_PRESS_INTERVAL_MS) {
            last_print_time = current_time;
            printf("[Power] Pressing: %dms/%dms (%d%%)\n", 
                   press_duration, 
                   POWER_KEY_LONG_PRESS_MS,
                   (press_duration * 100) / POWER_KEY_LONG_PRESS_MS);
        }
        
        // 适当延时，降低CPU占用率
        mdelay(LOOP_CHECK_INTERVAL_MS);
    }
    
    return 0; // 正常情况下不会执行到这里
}


void lwl_IO_key_active() 
{
    int g_io_key_event_active = 1;
    log_i("IO_key_active key press!\n");
}
void lwl_peri_dev_init(void)
{
    
    //port_wakeup_reg(EVENT_PA8, IO_PORTA_08, EDGE_NEGATIVE, lwl_IO_key_active);
    port_wakeup_reg(EVENT_PH7, IO_PORTH_07, EDGE_NEGATIVE, lwl_IO_key_active); // CHG_IN
    

    printf("lwl_peri_dev_init !\n");
    /* 0. 按键初始化 */
    gpio_set_direction(KEY_IO_PORT, 1); // 默认输入模式
    gpio_set_pull_down(KEY_IO_PORT, 0);
    gpio_set_pull_up(KEY_IO_PORT, 1);   // 默认上拉
    gpio_set_die(KEY_IO_PORT, 1);
    gpio_set_dieh(KEY_IO_PORT, 1);
    key_scan_start();

    gpio_set_direction(RST_KEY_PORT, 1); // 复位按键输入模式
    gpio_set_pull_down(RST_KEY_PORT, 0);
    gpio_set_pull_up(RST_KEY_PORT, 1);   // 默认上拉
    gpio_set_die(RST_KEY_PORT, 1);
    gpio_set_dieh(RST_KEY_PORT, 1);
    /* 1. 触摸初始化 */
    gpio_set_direction(TOUCH_IO, 1); // 默认输入模式
    gpio_set_pull_down(TOUCH_IO, 0);
    gpio_set_pull_up(TOUCH_IO, 0);   
    gpio_set_die(TOUCH_IO, 1);
    gpio_set_dieh(TOUCH_IO, 1);
    lwl_set_touch_key_active();
    /* 2. 唤醒7016C IO初始化 */

    /* 3. 充电芯片IO初始化 */
    gpio_set_direction(CHARGE_IN_IO, 1);
    gpio_set_pull_down(CHARGE_IN_IO, 0);
    gpio_set_pull_up(CHARGE_IN_IO, 0);
    gpio_set_die(CHARGE_IN_IO, 1);
    gpio_set_dieh(CHARGE_IN_IO, 1);

    gpio_set_direction(CHARGE_FULL_IO, 1);
    gpio_set_pull_down(CHARGE_FULL_IO, 0);
    gpio_set_pull_up(CHARGE_FULL_IO, 0);
    gpio_set_die(CHARGE_FULL_IO, 1);
    gpio_set_dieh(CHARGE_FULL_IO, 1);
    /* 4. LED初始化 */
    led_init();
    /* 5. g-sensor INT 初始化 */
    gpio_set_direction(GSENSOR_IO, 1);
    gpio_set_pull_down(GSENSOR_IO, 0);
    gpio_set_pull_up(GSENSOR_IO, 0);
    gpio_set_die(GSENSOR_IO, 1);
    gpio_set_dieh(GSENSOR_IO, 1);
    /* 6. 电量检测初始化 */
    gpio_set_direction(IO_PORTA_07, 1);
    gpio_set_pull_down(IO_PORTA_07, 0);
    gpio_set_pull_up(IO_PORTA_07, 0);
    adc_add_sample_ch(AD_CH_PA07);//添加通道
}


void lwl_peri_dev_loop(uint32_t curr_time)
{
    // IO按键检测
    key_scan_loop(curr_time);
    rst_key_loop( curr_time);
    // 加速度传感器检测 sc7a20
    g_sensor_loop(curr_time);

    charge_loop(curr_time);

    touch_key_loop(curr_time);

    battery_detection_loop(curr_time);

}

void lwl_peri_dev_7016_on(bool on)
{
    log_i("lwl_peri_dev_7016_on %d\n",on);
    /* 1. 拉IO将 7016音频设备关闭 睡眠 */
    if (on == false) {
        uart_send_audio_dev_close();
        // msleep(1000);
    } else {
        /* 3. 拉IO将 7016音频设备打开 唤醒 */
        gpio_set_direction(IO_PORTA_05, 0); // 输出模式
        gpio_set_pull_down(IO_PORTA_05, 0);
        gpio_set_pull_up(IO_PORTA_05, 0);
        gpio_direction_output(IO_PORTA_05, 1);
        mdelay(2);
        gpio_direction_output(IO_PORTA_05, 0);
        // msleep(1000);
    }
}