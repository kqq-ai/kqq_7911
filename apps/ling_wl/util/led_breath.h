#ifndef LED_BREATH_H
#define LED_BREATH_H

/**
 * @file    led_breath.h
 * @brief   LED 呼吸灯模块接口定义
 */

#include <stdint.h>

/**
 * @brief 初始化 LED 呼吸灯模块
 * @note  在使用 led_breath_start/led_set_color 前必须调用
 */
void led_breath_init(void);

/**
 * @brief 反初始化 LED 呼吸灯模块
 * @note  停止当前呼吸效果并释放资源
 */
void led_breath_deinit(void);

/**
 * @brief 启动呼吸灯效果，在两个颜色之间平滑过渡
 * 
 * @param color1_r 起始颜色 R 分量 (0-255)
 * @param color1_g 起始颜色 G 分量 (0-255)
 * @param color1_b 起始颜色 B 分量 (0-255)
 * @param color2_r 目标颜色 R 分量 (0-255)
 * @param color2_g 目标颜色 G 分量 (0-255)
 * @param color2_b 目标颜色 B 分量 (0-255)
 * @param cycle_count 循环次数 (0 = 无限循环)
 * @param transition_time_ms 单向过渡时间 (毫秒)
 * @param step_delay_ms 每步延时 (毫秒，影响平滑度)
 * @return 0: 成功, -1: 失败
 * 
 * @note 如果当前正在呼吸，会平滑切换到新效果
 */
int led_breath_start(uint16_t color1_r, uint16_t color1_g, uint16_t color1_b,
                     uint16_t color2_r, uint16_t color2_g, uint16_t color2_b,
                     uint16_t cycle_count, uint16_t transition_time_ms, 
                     uint8_t step_delay_ms);

/**
 * @brief 停止呼吸并设为固定色
 * 
 * @param r R 分量 (0-255)
 * @param g G 分量 (0-255)
 * @param b B 分量 (0-255)
 * 
 * @note 会停止呼吸线程并清理优先级条目
 */
void led_set_color(uint8_t r, uint8_t g, uint8_t b);

#endif /* LED_BREATH_H */
