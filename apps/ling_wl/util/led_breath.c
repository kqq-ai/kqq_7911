#include "led_breath.h"
#include "LED/LED.h"
#include "log.h"
#include "os/os_api.h" // 添加互斥锁支持
#include "os/pthread.h"
#include <stdbool.h>

/* 互斥锁 */
static OS_MUTEX g_led_breath_mutex;

static pthread_t breath_thread;
static volatile bool is_breathing = false;          // 只用一个标志位
static volatile bool breath_thread_started = false; // 线程是否启动过

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
int led_breathing_simple(uint16_t color1_r, uint16_t color1_g,
                         uint16_t color1_b, uint16_t color2_r,
                         uint16_t color2_g, uint16_t color2_b,
                         uint16_t cycle_count, uint16_t transition_time_ms,
                         uint8_t step_delay_ms) {
  os_mutex_pend(&g_led_breath_mutex, 0); // 只加锁,不创建

  if (step_delay_ms == 0) {
    step_delay_ms = 10; // 默认10ms
  }

  if (transition_time_ms == 0) {
    transition_time_ms = 1000; // 默认1秒
  }

  log_i("Starting simple breathing effect: (%d,%d,%d) -> (%d,%d,%d), "
        "cycles=%d, time=%dms, step=%dms",
        color1_r, color1_g, color1_b, color2_r, color2_g, color2_b, cycle_count,
        transition_time_ms, step_delay_ms);

  uint16_t steps = transition_time_ms / step_delay_ms;
  if (steps == 0)
    steps = 1;

  uint16_t cycles = 0;
  bool infinite = (cycle_count == 0);

  while (infinite || cycles < cycle_count) {
    // 从color1渐变到color2
    for (uint16_t i = 0; i <= steps; i++) {
      float ratio = (float)i / steps;
      uint16_t r = (uint16_t)(color1_r + (color2_r - color1_r) * ratio);
      uint16_t g = (uint16_t)(color1_g + (color2_g - color1_g) * ratio);
      uint16_t b = (uint16_t)(color1_b + (color2_b - color1_b) * ratio);

      set_rgb_color(r, g, b);
      mdelay(step_delay_ms);
    }

    // 从color2渐变回color1
    for (uint16_t i = 0; i <= steps; i++) {
      float ratio = (float)i / steps;
      uint16_t r = (uint16_t)(color2_r + (color1_r - color2_r) * ratio);
      uint16_t g = (uint16_t)(color2_g + (color1_g - color2_g) * ratio);
      uint16_t b = (uint16_t)(color2_b + (color1_b - color2_b) * ratio);

      set_rgb_color(r, g, b);
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

  os_mutex_post(&g_led_breath_mutex); // 解锁
  return 0;
}

/**
 * @brief 快速测试函数 - 使用颜色宏
 * @param cycles 循环次数
 */
void quick_test_with_macros(uint16_t cycles) {
  // 黑到绿色
  led_breathing_simple(COLOR_BLACK, COLOR_GREEN, cycles, 2000, 25);
  mdelay(1000);

  // 黑到蓝色
  led_breathing_simple(COLOR_BLACK, COLOR_BLUE, cycles, 2000, 25);
  mdelay(1000);

  // 黑色到红色
  led_breathing_simple(COLOR_BLACK, COLOR_RED, cycles, 2000, 25);
  mdelay(1000);
}

void led_breath_init(void) {
  os_mutex_create(&g_led_breath_mutex); // 只创建一次
}

/**
 * @brief 删除LED呼吸灯互斥锁
 * @note 在模块卸载或系统退出时调用
 */
void led_breath_deinit(void) {
  os_mutex_post(&g_led_breath_mutex);   // 先解锁,以防有线程还在等待
  os_mutex_delete(&g_led_breath_mutex); // 删除互斥锁
}

static void *breath_thread_func(void *arg) {
  uint16_t *params = (uint16_t *)arg;
  uint16_t color1_r = params[0], color1_g = params[1], color1_b = params[2];
  uint16_t color2_r = params[3], color2_g = params[4], color2_b = params[5];
  uint16_t cycle_count = params[6], transition_time_ms = params[7];
  uint8_t step_delay_ms = (uint8_t)params[8];
  free(params);

  uint16_t steps = transition_time_ms / step_delay_ms;
  if (steps == 0)
    steps = 1;
  uint16_t cycles = 0;
  bool infinite = (cycle_count == 0);

  while (infinite || cycles < cycle_count) {
    if (!is_breathing) {
      break;
    }
    // color1 -> color2
    for (uint16_t i = 0; i <= steps && is_breathing; i++) {
      float ratio = (float)i / steps;
      uint16_t r = (uint16_t)(color1_r + (color2_r - color1_r) * ratio);
      uint16_t g = (uint16_t)(color1_g + (color2_g - color1_g) * ratio);
      uint16_t b = (uint16_t)(color1_b + (color2_b - color1_b) * ratio);
      set_rgb_color(r, g, b);
      mdelay(step_delay_ms);
    }
    // color2 -> color1
    for (uint16_t i = 0; i <= steps && is_breathing; i++) {
      float ratio = (float)i / steps;
      uint16_t r = (uint16_t)(color2_r + (color1_r - color2_r) * ratio);
      uint16_t g = (uint16_t)(color2_g + (color1_g - color2_g) * ratio);
      uint16_t b = (uint16_t)(color2_b + (color1_b - color2_b) * ratio);
      set_rgb_color(r, g, b);
      mdelay(step_delay_ms);
    }
    cycles++;
  }
  return NULL;
}

int led_breath_start(uint16_t color1_r, uint16_t color1_g, uint16_t color1_b,
                     uint16_t color2_r, uint16_t color2_g, uint16_t color2_b,
                     uint16_t cycle_count, uint16_t transition_time_ms,
                     uint8_t step_delay_ms) {
  if (is_breathing)
    return 0;
  // 参数打包
  uint16_t *params = malloc(sizeof(uint16_t) * 9);
  params[0] = color1_r;
  params[1] = color1_g;
  params[2] = color1_b;
  params[3] = color2_r;
  params[4] = color2_g;
  params[5] = color2_b;
  params[6] = cycle_count;
  params[7] = transition_time_ms;
  params[8] = step_delay_ms;

  // 启动新线程前设置标志
  os_mutex_pend(&g_led_breath_mutex, 0);
  is_breathing = true;
  os_mutex_post(&g_led_breath_mutex);
  if (pthread_create(&breath_thread, NULL, breath_thread_func, params) == 0) {
    breath_thread_started = true;
    return 0;
  }
  free(params);
  return -1;
}

void led_set_color(uint8_t r, uint8_t g, uint8_t b) {
  // 打断呼吸灯线程
  os_mutex_pend(&g_led_breath_mutex, 0);
  is_breathing = false;
  os_mutex_post(&g_led_breath_mutex);

  // 只有线程真正启动过才等待它结束
  if (breath_thread_started) {
    pthread_join(breath_thread, NULL);
    breath_thread_started = false;
  }

  set_rgb_color(r, g, b);
}
