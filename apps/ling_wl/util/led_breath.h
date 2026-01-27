#ifndef LED_BREATH_H
#define LED_BREATH_H

// 这里可以添加你的声明
#include "protocol/cp/c_uart_p.h"

int led_breath_start(uint16_t color1_r, uint16_t color1_g, uint16_t color1_b,
                     uint16_t color2_r, uint16_t color2_g, uint16_t color2_b,
                     uint16_t cycle_count, uint16_t transition_time_ms, uint8_t step_delay_ms);

void led_set_color_and_break(uint8_t r, uint8_t g, uint8_t b);

void quick_test_with_macros(uint16_t cycles);

void led_breath_init(void); // 初始化LED呼吸灯

void led_breath_deinit(void); // 删除LED呼吸灯互斥锁

void led_set_color(uint8_t r, uint8_t g, uint8_t b);

#endif /* LED_BREATH_H */