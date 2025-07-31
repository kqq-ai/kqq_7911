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

#include "protocol/cp/c_uart_p.h"
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
    if (has_saved_wifi()) {
        lwl_app_mode_switch_to(APP_NET_CONNECTING_MODE);
    } else {
        lwl_app_mode_switch_to(APP_DEFAULT_MODE);
    }
    return 0;
}

static int power_on_unint(void)
{
    return 0;
}


/**
 * @brief 简化版呼吸灯效果 - 在两个颜色之间平滑过渡
 * @param color1_r 起始颜色R值
 * @param color1_g 起始颜色G值  
 * @param color1_b 起始颜色B值
 * @param color2_r 目标颜色R值
 * @param color2_g 目标颜色G值
 * @param color2_b 目标颜色B值
 * @param cycle_count 循环次数 (0表示无限循环)
 * @param transition_time_ms 单次过渡时间(毫秒)
 * @param step_delay_ms 每步延时(毫秒)
 * @return 0:成功, -1:参数错误
 */
int led_breathing_simple(uint16_t color1_r, uint16_t color1_g, uint16_t color1_b,
                        uint16_t color2_r, uint16_t color2_g, uint16_t color2_b,
                        uint16_t cycle_count, uint16_t transition_time_ms, uint8_t step_delay_ms)
{
    if (step_delay_ms == 0) {
        step_delay_ms = 10; // 默认10ms
    }
    
    if (transition_time_ms == 0) {
        transition_time_ms = 1000; // 默认1秒
    }
    
    log_i("Starting simple breathing effect: (%d,%d,%d) -> (%d,%d,%d), cycles=%d, time=%dms, step=%dms",
          color1_r, color1_g, color1_b, color2_r, color2_g, color2_b, 
          cycle_count, transition_time_ms, step_delay_ms);
    
    uint16_t steps = transition_time_ms / step_delay_ms;
    if (steps == 0) steps = 1;
    
    uint16_t cycles = 0;
    bool infinite = (cycle_count == 0);
    
    while (infinite || cycles < cycle_count) {
        // 从color1渐变到color2
        for (uint16_t i = 0; i <= steps; i++) {
            float ratio = (float)i / steps;
            uint16_t r = (uint16_t)(color1_r + (color2_r - color1_r) * ratio);
            uint16_t g = (uint16_t)(color1_g + (color2_g - color1_g) * ratio);
            uint16_t b = (uint16_t)(color1_b + (color2_b - color1_b) * ratio);
            
            uart_send_set_led_color(r, g, b);
            mdelay(step_delay_ms);
        }
        
        // 从color2渐变回color1
        for (uint16_t i = 0; i <= steps; i++) {
            float ratio = (float)i / steps;
            uint16_t r = (uint16_t)(color2_r + (color1_r - color2_r) * ratio);
            uint16_t g = (uint16_t)(color2_g + (color1_g - color2_g) * ratio);
            uint16_t b = (uint16_t)(color2_b + (color1_b - color2_b) * ratio);
            
            uart_send_set_led_color(r, g, b);
            mdelay(step_delay_ms);
        }
        
        cycles++;
        if (!infinite) {
            log_d("Simple breathing cycle %d/%d completed", cycles, cycle_count);
        } else {
            log_d("Simple breathing cycle %d completed (infinite mode)", cycles);
        }
    }
    
    log_i("Simple breathing effect completed: %d cycles", cycles);
    return 0;
}

/**
 * @brief 使用颜色宏的呼吸灯效果示例
 */
void example_breathing_with_macros(void)
{
    log_i("=== 使用颜色宏的呼吸灯效果示例 ===");
    
    // 示例1: 红色到绿色呼吸灯，循环5次
    log_i("示例1: 红色到绿色呼吸灯");
    led_breathing_simple(COLOR_RED, COLOR_GREEN, 5, 1000, 15);
    
    // 示例2: 蓝色到白色呼吸灯，循环3次
    log_i("示例2: 蓝色到白色呼吸灯");
    led_breathing_simple(COLOR_BLUE, COLOR_WHITE, 3, 1500, 20);
    
    // 示例3: 紫色到橙色呼吸灯，循环2次
    log_i("示例3: 紫色到橙色呼吸灯");
    led_breathing_simple(COLOR_PURPLE, COLOR_ORANGE, 2, 2000, 25);
    
    log_i("所有颜色宏呼吸灯效果示例完成");
}

/**
 * @brief 快速测试函数 - 使用颜色宏
 * @param cycles 循环次数
 */
void quick_test_with_macros(uint16_t cycles)
{
    // 黑到绿色
    led_breathing_simple(COLOR_BLACK, COLOR_GREEN, cycles, 2000, 25);
    mdelay(1000);
    
    // 黑到蓝色
    led_breathing_simple(COLOR_BLACK, COLOR_BLUE, cycles, 2000, 25);
    mdelay(1000);
    
    // 黑色到红色
    led_breathing_simple(COLOR_BLACK, COLOR_RED, cycles, 2000, 25);
    mdelay(1000);

    /*
    // 快速呼吸效果
    led_breathing_simple(COLOR_RED, COLOR_GREEN, 5, 300, 10);  // 0.3秒过渡

    // 标准呼吸效果  
    led_breathing_simple(COLOR_RED, COLOR_GREEN, 5, 800, 15);  // 0.8秒过渡

    // 慢速呼吸效果
    led_breathing_simple(COLOR_RED, COLOR_GREEN, 5, 2000, 25); // 2秒过渡
    */

    log_i("快速测试完成");
}




void app_power_on_mode(void)
{
    
    //quick_test_with_macros(2);
    power_on_init();
    while (1) {
        // 应用代码
        mdelay(10);
        if (lwl_app_mode_exitting()) {
            power_on_unint();
            return;
        }
    }
}