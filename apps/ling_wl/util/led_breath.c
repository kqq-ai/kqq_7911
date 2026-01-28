/**
 * @file    led_breath.c
 * @brief   LED 呼吸灯效果模块
 * @details 支持两色间平滑呼吸过渡，使用正弦缓动和伽马校正优化视觉效果
 */

#include "log.h"
#include <stdbool.h>
#include <stdlib.h>
#include "led_breath.h"
#include "os/os_api.h"
#include "os/pthread.h"
#include "LED/LED.h"

/*============================================================================
 *                              宏定义
 *===========================================================================*/

#define LOG_TAG            "[breath]"
#define BREATH_PRIORITY    5
#define MIN_BRIGHTNESS     2      /* 最小亮度，避免完全熄灭闪烁 */
#define GAMMA_SCALE        256    /* 伽马表值域 */

/*============================================================================
 *                              类型定义
 *===========================================================================*/

/**
 * @brief 呼吸灯模块私有上下文
 */
typedef struct {
    OS_MUTEX       mutex;           /**< 互斥锁 */
    pthread_t      thread;          /**< 呼吸线程 */
    volatile bool  is_active;       /**< 呼吸运行标志 */
    volatile bool  thread_created;  /**< 线程已创建标志 */
} breath_context_t;

/**
 * @brief 呼吸线程参数
 */
typedef struct {
    uint16_t color1_r, color1_g, color1_b;
    uint16_t color2_r, color2_g, color2_b;
    uint16_t cycle_count;
    uint16_t transition_time_ms;
    uint16_t step_delay_ms;
} breath_params_t;

/*============================================================================
 *                              查找表
 *===========================================================================*/

/**
 * @brief 伽马校正查找表 (gamma = 2.2)
 * @note  将线性 RGB 值映射到感知线性亮度
 *        公式: output = (input / 255)^2.2 * 255
 */
static const uint8_t gamma_table[256] = {
      0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   1,
      1,   1,   1,   1,   1,   1,   1,   1,   1,   2,   2,   2,   2,   2,   2,   2,
      3,   3,   3,   3,   3,   4,   4,   4,   4,   5,   5,   5,   5,   6,   6,   6,
      6,   7,   7,   7,   8,   8,   8,   9,   9,   9,  10,  10,  11,  11,  11,  12,
     12,  13,  13,  13,  14,  14,  15,  15,  16,  16,  17,  17,  18,  18,  19,  19,
     20,  20,  21,  22,  22,  23,  23,  24,  25,  25,  26,  26,  27,  28,  28,  29,
     30,  30,  31,  32,  33,  33,  34,  35,  35,  36,  37,  38,  39,  39,  40,  41,
     42,  43,  43,  44,  45,  46,  47,  48,  49,  49,  50,  51,  52,  53,  54,  55,
     56,  57,  58,  59,  60,  61,  62,  63,  64,  65,  66,  67,  68,  69,  70,  71,
     73,  74,  75,  76,  77,  78,  79,  81,  82,  83,  84,  85,  87,  88,  89,  90,
     91,  93,  94,  95,  97,  98,  99, 100, 102, 103, 105, 106, 107, 109, 110, 111,
    113, 114, 116, 117, 119, 120, 121, 123, 124, 126, 127, 129, 130, 132, 133, 135,
    137, 138, 140, 141, 143, 145, 146, 148, 149, 151, 153, 154, 156, 158, 159, 161,
    163, 165, 166, 168, 170, 172, 173, 175, 177, 179, 181, 182, 184, 186, 188, 190,
    192, 194, 196, 197, 199, 201, 203, 205, 207, 209, 211, 213, 215, 217, 219, 221,
    223, 225, 227, 229, 231, 234, 236, 238, 240, 242, 244, 246, 249, 251, 253, 255
};

/**
 * @brief 正弦缓动查找表 (65点，覆盖 0 到 π)
 * @note  值域 0-206，对应 sin(0) 到 sin(π/2) 的平滑过渡
 */
static const uint8_t sine_table[65] = {
    0,   6,  13,  19,  25,  31,  37,  44,  50,  56,
   62,  68,  74,  80,  86,  91,  97, 103, 108, 113,
  119, 124, 129, 134, 139, 143, 148, 152, 156, 160,
  164, 168, 171, 175, 178, 181, 184, 186, 189, 191,
  193, 195, 197, 199, 200, 201, 202, 203, 204, 205,
  205, 206, 206, 206, 206, 206, 205, 205, 204, 203,
  202, 201, 200, 199, 197
};

/*============================================================================
 *                              静态变量
 *===========================================================================*/

static breath_context_t g_ctx = {0};

/*============================================================================
 *                            私有辅助函数
 *===========================================================================*/

/**
 * @brief 正弦缓动插值
 * @param i     当前步数
 * @param steps 总步数
 * @return 缓动比例值 (0-206)
 */
static uint16_t sine_ease(uint16_t i, uint16_t steps) {
    if (steps == 0) return 0;
    uint16_t idx = (i * 64) / steps;
    if (idx > 64) idx = 64;
    return sine_table[idx];
}

/**
 * @brief 应用伽马校正
 * @param value 原始 RGB 分量 (0-255)
 * @return 伽马校正后的值 (0-255)
 */
static inline uint8_t apply_gamma(uint8_t value) {
    return gamma_table[value];
}

/**
 * @brief 颜色分量插值 (带最小亮度保护)
 * @param from  起始颜色分量
 * @param to    目标颜色分量
 * @param ratio 插值比例 (0-206)
 * @return 插值后的颜色分量
 */
static inline uint16_t interpolate_color(uint16_t from, uint16_t to, uint16_t ratio) {
    int16_t result = from + ((int16_t)(to - from) * ratio) / 206;
    /* 最小亮度保护：如果非零色分量计算结果过小，保持最小亮度 */
    if (from > 0 || to > 0) {
        if (result < MIN_BRIGHTNESS && result > 0) {
            result = MIN_BRIGHTNESS;
        }
    }
    return (uint16_t)result;
}

/**
 * @brief 同步停止呼吸线程
 * @note  阻塞等待线程退出并清理资源
 */
static void breath_stop_sync(void) {
    os_mutex_pend(&g_ctx.mutex, 0);
    if (!g_ctx.is_active) {
        os_mutex_post(&g_ctx.mutex);
        return;
    }
    g_ctx.is_active = false;
    os_mutex_post(&g_ctx.mutex);

    if (g_ctx.thread_created) {
        pthread_join(g_ctx.thread, NULL);
        g_ctx.thread_created = false;
    }
    remove_entries_by_priority(BREATH_PRIORITY);
}

/*============================================================================
 *                              线程函数
 *===========================================================================*/

static void* breath_thread_func(void* arg) {
    breath_params_t *p = (breath_params_t *)arg;
    
    /* 复制参数到局部变量 */
    uint16_t c1_r = p->color1_r, c1_g = p->color1_g, c1_b = p->color1_b;
    uint16_t c2_r = p->color2_r, c2_g = p->color2_g, c2_b = p->color2_b;
    uint16_t cycle_count = p->cycle_count;
    uint16_t transition_ms = p->transition_time_ms;
    uint16_t step_ms = p->step_delay_ms;
    free(p);
    
    log_i("%s started: (%d,%d,%d)->(%d,%d,%d) time=%dms", 
          LOG_TAG, c1_r, c1_g, c1_b, c2_r, c2_g, c2_b, transition_ms);

    uint16_t steps = transition_ms / step_ms;
    if (steps == 0) steps = 1;
    
    bool infinite = (cycle_count == 0);
    uint16_t cycles = 0;

    while ((infinite || cycles < cycle_count) && g_ctx.is_active) {
        /* 渐变: color1 -> color2 */
        for (uint16_t i = 0; i <= steps && g_ctx.is_active; i++) {
            uint16_t ratio = sine_ease(i, steps);
            uint8_t r = apply_gamma((uint8_t)interpolate_color(c1_r, c2_r, ratio));
            uint8_t g = apply_gamma((uint8_t)interpolate_color(c1_g, c2_g, ratio));
            uint8_t b = apply_gamma((uint8_t)interpolate_color(c1_b, c2_b, ratio));
            set_rgb_ON(r, g, b, BREATH_PRIORITY);
            msleep(step_ms);
        }
        
        /* 渐变: color2 -> color1 */
        for (uint16_t i = 0; i <= steps && g_ctx.is_active; i++) {
            uint16_t ratio = sine_ease(i, steps);
            uint8_t r = apply_gamma((uint8_t)interpolate_color(c2_r, c1_r, ratio));
            uint8_t g = apply_gamma((uint8_t)interpolate_color(c2_g, c1_g, ratio));
            uint8_t b = apply_gamma((uint8_t)interpolate_color(c2_b, c1_b, ratio));
            set_rgb_ON(r, g, b, BREATH_PRIORITY);
            msleep(step_ms);
        }
        cycles++;
    }

    remove_entries_by_priority(BREATH_PRIORITY);
    log_i("%s exiting", LOG_TAG);
    return NULL;
}

/*============================================================================
 *                              公共 API
 *===========================================================================*/

void led_breath_init(void) {
    os_mutex_create(&g_ctx.mutex);
}

void led_breath_deinit(void) {
    breath_stop_sync();
    os_mutex_delete(&g_ctx.mutex);
    log_i("%s deinit complete", LOG_TAG);
}

int led_breath_start(uint16_t color1_r, uint16_t color1_g, uint16_t color1_b,
                     uint16_t color2_r, uint16_t color2_g, uint16_t color2_b,
                     uint16_t cycle_count, uint16_t transition_time_ms, 
                     uint8_t step_delay_ms)
{
    /* 如果正在呼吸，先停止 */
    breath_stop_sync();

    /* 分配参数结构体 */
    breath_params_t *params = malloc(sizeof(*params));
    if (!params) {
        log_w("%s failed to allocate params", LOG_TAG);
        return -1;
    }

    params->color1_r = color1_r;
    params->color1_g = color1_g;
    params->color1_b = color1_b;
    params->color2_r = color2_r;
    params->color2_g = color2_g;
    params->color2_b = color2_b;
    params->cycle_count = cycle_count;
    params->transition_time_ms = transition_time_ms;
    params->step_delay_ms = step_delay_ms;

    /* 设置运行标志并启动线程 */
    os_mutex_pend(&g_ctx.mutex, 0);
    g_ctx.is_active = true;
    os_mutex_post(&g_ctx.mutex);

    log_i("%s start: (%d,%d,%d)->(%d,%d,%d) cycles=%d time=%d step=%d",
          LOG_TAG, color1_r, color1_g, color1_b, 
          color2_r, color2_g, color2_b,
          cycle_count, transition_time_ms, step_delay_ms);

    if (pthread_create(&g_ctx.thread, NULL, breath_thread_func, params) == 0) {
        g_ctx.thread_created = true;
        return 0;
    }

    /* 线程创建失败，回退状态 */
    os_mutex_pend(&g_ctx.mutex, 0);
    g_ctx.is_active = false;
    os_mutex_post(&g_ctx.mutex);
    free(params);
    return -1;
}

void led_set_color(uint8_t r, uint8_t g, uint8_t b) {
    breath_stop_sync();
    log_i("%s set_color: (%d,%d,%d)", LOG_TAG, r, g, b);
    set_rgb_color(r, g, b);
}
