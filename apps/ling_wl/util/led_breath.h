#ifndef LED_BREATH_H
#define LED_BREATH_H

#include \u003cstdint.h\u003e
#include \u003cstdbool.h\u003e

// 这里可以添加你的声明
#include "protocol/cp/c_uart_p.h"

int led_breath_start(uint16_t color1_r, uint16_t color1_g, uint16_t color1_b,
                     uint16_t color2_r, uint16_t color2_g, uint16_t color2_b,
                     uint16_t cycle_count, uint16_t transition_time_ms,
                     uint8_t step_delay_ms);

void led_set_color_and_break(uint8_t r, uint8_t g, uint8_t b);

void quick_test_with_macros(uint16_t cycles);

void led_breath_init(void); // 初始化LED呼吸灯

void led_breath_deinit(void); // 删除LED呼吸灯互斥锁

void led_set_color(uint8_t r, uint8_t g, uint8_t b);

int led_breath_start(uint16_t color1_r, uint16_t color1_g, uint16_t color1_b,
                     uint16_t color2_r, uint16_t color2_g, uint16_t color2_b,
                     uint16_t cycle_count, uint16_t transition_time_ms,
                     uint8_t step_delay_ms);

// ========== 新增API（业界标准呼吸灯） ==========

// 业界标准呼吸灯参数
#define LED_BREATH_PERIOD_MS 3000   // 3秒完整周期
#define LED_BREATH_FADE_IN_MS 1500  // 1.5秒渐亮
#define LED_BREATH_FADE_OUT_MS 1500 // 1.5秒渐暗
#define LED_BREATH_MIN_DUTY 10      // 最暗10%
#define LED_BREATH_MAX_DUTY 100     // 最亮100%
#define LED_BREATH_STEP_MS 25       // 每步延时25ms

/**
 * @brief 启动无限循环呼吸灯（异步）
 * @param r 目标颜色R值（0-100）
 * @param g 目标颜色G值（0-100）
 * @param b 目标颜色B值（0-100）
 * @return 0=成功, -1=失败
 * @note 呼吸灯在独立线程中运行，在10%-100%亮度之间渐变，3秒周期
 */
int led_breath_start_infinite(uint16_t r, uint16_t g, uint16_t b);

/**
 * @brief 停止呼吸灯
 * @note 停止后LED保持当前颜色
 */
void led_breath_stop(void);

/**
 * @brief 查询呼吸灯是否正在运行
 * @return true=正在运行, false=已停止
 */
bool is_led_breathing(void);

#endif /* LED_BREATH_H */