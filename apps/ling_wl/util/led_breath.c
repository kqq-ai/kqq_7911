#include "log.h"
#include <stdbool.h>
#include "led_breath.h"
#include "os/os_api.h" // 添加互斥锁支持
#include "os/pthread.h"
#include "LED/LED.h"

/* 互斥锁 */
static OS_MUTEX g_led_breath_mutex;

static pthread_t breath_thread;
static volatile bool is_breathing = false; // 只用一个标志位
static volatile bool breath_thread_created = false;

/* 呼吸优先级（非0，避免与 priority==0 的特殊处理冲突） */
#define BREATH_PRIORITY 5

/* 呼吸线程参数结构体 */
typedef struct {
    uint16_t color1_r;
    uint16_t color1_g;
    uint16_t color1_b;
    uint16_t color2_r;
    uint16_t color2_g;
    uint16_t color2_b;
    uint16_t cycle_count;
    uint16_t transition_time_ms;
    uint16_t step_delay_ms;
} breath_params_t;


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


void led_breath_init(void)
{
    os_mutex_create(&g_led_breath_mutex); // 只创建一次
}

/**
 * @brief 删除LED呼吸灯互斥锁
 * @note 在模块卸载或系统退出时调用
 */
void led_breath_deinit(void)
{
    // 请求停止并等待线程退出
    os_mutex_pend(&g_led_breath_mutex, 0);
    is_breathing = false;
    os_mutex_post(&g_led_breath_mutex);
    if (breath_thread_created) {
        pthread_join(breath_thread, NULL);
        breath_thread_created = false;
    }
    // 注销可能残留的呼吸条目
    log_i("[breath] led_breath_deinit: unregistering breath entries");
    remove_entries_by_priority(BREATH_PRIORITY);
    os_mutex_delete(&g_led_breath_mutex); // 删除互斥锁
}

static void* breath_thread_func(void* arg) {
    /* 使用共享结构体传参，提升可读性与类型安全 */
    breath_params_t *p = (breath_params_t *)arg;
    uint16_t color1_r = p->color1_r;
    uint16_t color1_g = p->color1_g;
    uint16_t color1_b = p->color1_b;
    uint16_t color2_r = p->color2_r;
    uint16_t color2_g = p->color2_g;
    uint16_t color2_b = p->color2_b;
    uint16_t cycle_count = p->cycle_count;
    uint16_t transition_time_ms = p->transition_time_ms;
    uint16_t step_delay_ms = p->step_delay_ms;
    free(p);
    log_i("[breath] breath_thread_func started: color1=(%d,%d,%d) color2=(%d,%d,%d) time=%d", color1_r, color1_g, color1_b, color2_r, color2_g, color2_b, transition_time_ms);

    uint16_t steps = transition_time_ms / step_delay_ms;
    if (steps == 0) steps = 1;
    uint16_t cycles = 0;
    bool infinite = (cycle_count == 0);
    // 通用分支：在 color1 <-> color2 之间平滑过渡（或快速切换，取决于 transition_time_ms 与 step_delay_ms）
    while (infinite || cycles < cycle_count) {
        if (!is_breathing) {
            break;
        }
        // 从color1渐变到color2
        for (uint16_t i = 0; i <= steps && is_breathing; i++) {
            float ratio = (float)i / steps;
            uint16_t r = (uint16_t)(color1_r + (color2_r - color1_r) * ratio);
            uint16_t g = (uint16_t)(color1_g + (color2_g - color1_g) * ratio);
            uint16_t b = (uint16_t)(color1_b + (color2_b - color1_b) * ratio);
            set_rgb_ON(r, g, b, BREATH_PRIORITY);
            msleep(step_delay_ms);
        }
        // 从color2渐变回color1
        for (uint16_t i = 0; i <= steps && is_breathing; i++) {
            float ratio = (float)i / steps;
            uint16_t r = (uint16_t)(color2_r + (color1_r - color2_r) * ratio);
            uint16_t g = (uint16_t)(color2_g + (color1_g - color2_g) * ratio);
            uint16_t b = (uint16_t)(color2_b + (color1_b - color2_b) * ratio);
            set_rgb_ON(r, g, b, BREATH_PRIORITY);
            msleep(step_delay_ms);
        }
        cycles++;
    }
    // 退出前注销自己可能留下的条目（按 priority 批量移除，避免 exact RGB 匹配问题）
    remove_entries_by_priority(BREATH_PRIORITY);
    log_i("[breath] breath_thread_func exiting");
    return NULL;
}

int led_breath_start(uint16_t color1_r, uint16_t color1_g, uint16_t color1_b,
                     uint16_t color2_r, uint16_t color2_g, uint16_t color2_b,
                     uint16_t cycle_count, uint16_t transition_time_ms, uint8_t step_delay_ms)
{
    if (is_breathing) return 0;
    // 参数打包（使用共享结构体以提高可读性）
    breath_params_t *params = malloc(sizeof(*params));
    if (!params) {
        os_mutex_pend(&g_led_breath_mutex, 0);
        is_breathing = false;
        os_mutex_post(&g_led_breath_mutex);
        log_w("[breath] Failed to allocate params for breath thread");
        return -1;
    }
    params->color1_r = color1_r; params->color1_g = color1_g; params->color1_b = color1_b;
    params->color2_r = color2_r; params->color2_g = color2_g; params->color2_b = color2_b;
    params->cycle_count = cycle_count; params->transition_time_ms = transition_time_ms; params->step_delay_ms = step_delay_ms;

    // 启动新线程前设置标志
    os_mutex_pend(&g_led_breath_mutex, 0);
    is_breathing = true;
    os_mutex_post(&g_led_breath_mutex);
    log_i("[breath] led_breath_start: (%d,%d,%d)->(%d,%d,%d) cycles=%d time=%d step=%d", color1_r, color1_g, color1_b, color2_r, color2_g, color2_b, cycle_count, transition_time_ms, step_delay_ms);
    if (pthread_create(&breath_thread, NULL, breath_thread_func, params) == 0) {
        breath_thread_created = true;
        return 0;
    }
    // 线程创建失败，回退状态并释放资源
    os_mutex_pend(&g_led_breath_mutex, 0);
    is_breathing = false;
    os_mutex_post(&g_led_breath_mutex);
    free(params);
    return -1;
}

/** 结束呼吸并设为固定色（会打断呼吸、清理 BREATH_PRIORITY 条目后再 set_rgb_color）。 */
void led_set_color(uint8_t r, uint8_t g, uint8_t b)
{
    // 打断呼吸灯线程
    os_mutex_pend(&g_led_breath_mutex, 0);
    is_breathing = false;
    os_mutex_post(&g_led_breath_mutex);

    if (breath_thread_created) {
        pthread_join(breath_thread, NULL);
        breath_thread_created = false;
    }
    log_i("[breath] led_set_color: stopping breath and unregistering entries");
    // 注销呼吸优先级可能残留的条目，防止覆盖其他灯效
    remove_entries_by_priority(BREATH_PRIORITY);
    set_rgb_color(r, g, b);
}


