
#include "asm/pwm.h"
#include "app_config.h"
#include "system/includes.h"
#include "device/device.h"
#include "gpio.h"
#include "uart.h"
#include "log.h"
#include "LED.h"
#include "app_cookie.h"



#include "asm/clock.h"
#include "os/os_api.h"
static JL_TIMER_TypeDef *TMR = JL_TIMER4;//选择定时器4
static u8 timer_irq = IRQ_TIMER4_IDX;//选择定时器4

static const u8 timer_index[16] =  {0, 4, 1, 5, 2,  6,  3,  7,   8,   12,  9,    13,   10,   14,   11,    15};
static const u32 timer_table[16] = {1, 2, 4, 8, 16, 32, 64, 128, 256, 512, 1024, 2048, 4096, 8192, 16384, 32768};
static u32 timer_clk = 0;

#define AWINLINE   __attribute__((always_inline))

#define TSEC SEC_USED(.volatile_ram_code) //当中断1ms一下，建议使用函数放在内部ram
//#define TSEC
// 蓝色，0-100，值越大越亮

static u16 g_priority_LED = 0; // 当前LED颜色优先级，初始为0

/* 新实现：按 (priority desc, seq desc) 排序的活动 LED 列表。
 * priority 越大越靠前；相同 priority 按后开启（seq 较大）靠前。
 * set_rgb_ON 插入一条记录，set_rgb_OFF 删除与 (r,g,b,priority) 完全匹配的最新一条记录。
 */
#define LED_MAX_ENTRIES 32
struct led_entry {
    u16 red;
    u16 green;
    u16 blue;
    u16 priority;
    u32 seq; /* 单调递增，用于判定后开启 */
    u8  used;
};
static struct led_entry led_entries[LED_MAX_ENTRIES];
static u32 led_seq_counter = 0;
/* 保护 led_entries 的互斥锁 */
static OS_MUTEX g_led_entries_mutex;

void *pwm_dev_handl_r = NULL;
struct pwm_platform_data pwm_r = {0};
void *pwm_dev_handl_g = NULL;
struct pwm_platform_data pwm_g = {0};
void *pwm_dev_handl_b = NULL;
struct pwm_platform_data pwm_b = {0};

/* helper: find best entry index according to priority desc, seq desc */
static int led_find_best_index(void)
{
    int best = -1;
    for (int i = 0; i < LED_MAX_ENTRIES; i++) {
        if (!led_entries[i].used) continue;
        if (best == -1) {
            best = i;
            continue;
        }
        if (led_entries[i].priority > led_entries[best].priority) {
            best = i;
        } else if (led_entries[i].priority == led_entries[best].priority) {
            if (led_entries[i].seq > led_entries[best].seq) {
                best = i;
            }
        }
    }
    return best;
}

/* helper: insert a new entry (returns 0 on success, -1 on full) */
static int led_insert_entry(u16 red, u16 green, u16 blue, u16 priority)
{
    if (priority == 0) {
        led_entries[0].used = 1;
        led_entries[0].red = red;
        led_entries[0].green = green;
        led_entries[0].blue = blue;
        led_entries[0].priority = priority;
        return 0;
    }
    /* If an entry with same priority exists, update it (avoid duplicating entries) */
    for (int i = 0; i < LED_MAX_ENTRIES; i++) {
        if (led_entries[i].used && led_entries[i].priority == priority) {
            led_entries[i].red = red;
            led_entries[i].green = green;
            led_entries[i].blue = blue;
            led_entries[i].seq = ++led_seq_counter;
            // log_d("LED: updated existing entry idx=%d priority=%d", i, priority);
            return 0;
        }
    }
    /* Find empty slot */
    for (int i = 0; i < LED_MAX_ENTRIES; i++) {
        if (!led_entries[i].used) {
            led_entries[i].used = 1;
            led_entries[i].red = red;
            led_entries[i].green = green;
            led_entries[i].blue = blue;
            led_entries[i].priority = priority;
            led_entries[i].seq = ++led_seq_counter;
            // log_d("LED: inserted entry idx=%d priority=%d", i, priority);
            return 0;
        }
    }
    /* Table full: try to replace the lowest-priority (and oldest) entry if new priority is higher */
    int cand = -1;
    u16 min_pri = 0xFFFF;
    u32 min_seq = 0xFFFFFFFF;
    for (int i = 0; i < LED_MAX_ENTRIES; i++) {
        if (!led_entries[i].used) continue;
        if (led_entries[i].priority < min_pri || (led_entries[i].priority == min_pri && led_entries[i].seq < min_seq)) {
            min_pri = led_entries[i].priority;
            min_seq = led_entries[i].seq;
            cand = i;
        }
    }
    if (cand != -1 && priority > min_pri) {
        log_w("LED entries full, replacing idx=%d (pri=%d) with new pri=%d", cand, min_pri, priority);
        led_entries[cand].red = red;
        led_entries[cand].green = green;
        led_entries[cand].blue = blue;
        led_entries[cand].priority = priority;
        led_entries[cand].seq = ++led_seq_counter;
        return 0;
    }
    return -1; /* full and not replaceable */
}

/* helper: remove the most-recent entry that exactly matches r,g,b,priority. Return 0 if removed, -1 if not found */
static int led_remove_matching_entry(u16 red, u16 green, u16 blue, u16 priority)
{
    int found = -1;
    u32 max_seq = 0;
    for (int i = 0; i < LED_MAX_ENTRIES; i++) {
        if (!led_entries[i].used) continue;
        if (led_entries[i].red == red && led_entries[i].green == green && led_entries[i].blue == blue && led_entries[i].priority == priority) {
            if (led_entries[i].seq >= max_seq) {
                max_seq = led_entries[i].seq;
                found = i;
            }
        }
    }
    if (found != -1) {
        led_entries[found].used = 0;
        return 0;
    }
    return -1;
}

/* Remove all entries that match the given priority */
void remove_entries_by_priority(u16 priority)
{
    int removed = 0;
    os_mutex_pend(&g_led_entries_mutex, 0);
    for (int i = 0; i < LED_MAX_ENTRIES; i++) {
        if (led_entries[i].used && led_entries[i].priority == priority) {
            led_entries[i].used = 0;
            removed++;
        }
    }
    os_mutex_post(&g_led_entries_mutex);
    if (removed) {
        log_i("Removed %d LED entries with priority %d", removed, priority);
    } else {
        log_d("No LED entries found with priority %d to remove", priority);
    }
}

static AWINLINE TSEC void timer_cfg(u32 freq, u32 us)
{
    u32 clock = timer_clk;
    u8 psc = 0;
    u8 tmp = 0;
    u32 prd = 0;
    u32 ts = us / (1000 * 1000);//计算秒
    u32 tu = us % (1000 * 1000);//计算秒剩余us
    u8 i;
    float tp = 0;

    if (freq >= clock) {
        freq = clock;
    } else if (freq <= 1) {
        freq = 1;
        if (ts) {
            tp = (float)tu / (1000 * 1000);
        }
    }
    /*求prd值*/
    prd = clock / freq;
    if (prd > 65535) {
        for (psc = 0; psc < 16; psc++) {
            prd = (u32)(clock / (timer_table[psc]) / freq);
            if (prd <= 65535) {
                break;
            } else if (psc == 15) {
                prd = 65535;
                break;
            }
        }
    }
    prd = ts ? (prd * ts + tp * prd) : prd;
    psc = timer_index[psc];
    TMR->CON = 0;
    TMR->CNT = 0;
    TMR->CON |= BIT(14);
    TMR->PRD = prd;
    TMR->CON |= psc << 4; //lsb_clk分频
    TMR->CON |= BIT(0);
}
static void TSEC timer_set(u32 us)//设置时间，该函数可以在中断调用重新设置定时器值
{
    u32 freq = 1000000 / us;
    timer_cfg(freq, us);
}
static void TSEC timer_freq_set(u32 freq)//设置频率，该函数可以在中断调用重新设置定时器值
{
    timer_cfg(freq, 0);
}
static ___interrupt TSEC void timer_isr(void)//中断
{
    static u8 ms_count = 3;
    if (TMR->CON & BIT(15)) {
        TMR->CON |= BIT(14);
        ///putchar('@');
        //todo，中断函数执行程序...
        ms_count ++;
        if( ms_count >= 5 ){
            ms_count = 0;
        }
        
        switch (ms_count)
        {
        case 0:
        case 1:
            break;
        case 2:
            break;
        case 3:
        case 4:
            break;
        }
    }
}

// 控制RGB颜色
void set_rgb_LED(u16 red, u16 green, u16 blue) {
    // log_i("Event set_rgb_color: %d  : %d  : %d \n", red, green, blue);
    
    if (pwm_dev_handl_r == NULL || pwm_dev_handl_g == NULL || pwm_dev_handl_b == NULL) {
        return;
    } else{
        pwm_r.pwm_ch = PWMCH1_L;
        pwm_r.duty = 100 - (red / 3);
        dev_write(pwm_dev_handl_r, (void *)&pwm_r, 0);
        pwm_g.pwm_ch = PWM_TIMER2_OPCH2;
        pwm_g.duty = 100 - (green / 3);
        dev_write(pwm_dev_handl_g, (void *)&pwm_g, 0);
        pwm_b.pwm_ch = PWM_TIMER3_OPCH3;
        pwm_b.duty = 100 - (blue / 3);
        dev_write(pwm_dev_handl_b, (void *)&pwm_b, 0);
    }
}

void set_rgb_ON(u16 red, u16 green, u16 blue, u16 priority){
    // 新行为：插入一条记录并按 (priority desc, seq desc) 选取第一位显示
    // log_i("Event set_rgb_ON: %d  : %d  : %d , priority: %d\n", red, green, blue, priority);
    os_mutex_pend(&g_led_entries_mutex, 0);
    if (led_insert_entry(red, green, blue, priority) != 0) {
        log_w("LED entries full, cannot insert new entry\n");
        os_mutex_post(&g_led_entries_mutex);
        return;
    }

    /* 更新显示为当前最佳 */
    int best = led_find_best_index();
    if (best >= 0) {
        set_rgb_LED(led_entries[best].red, led_entries[best].green, led_entries[best].blue);
        g_priority_LED = led_entries[best].priority;
    }
    os_mutex_post(&g_led_entries_mutex);
}

void set_rgb_OFF(u16 red, u16 green, u16 blue, u16 priority) {
    // 新行为：删除与 (r,g,b,priority) 完全匹配的最新一条记录
    // log_i("Event set_rgb_OFF: %d  : %d  : %d , priority: %d\n", red, green, blue, priority);
    os_mutex_pend(&g_led_entries_mutex, 0);
    if (led_remove_matching_entry(red, green, blue, priority) != 0) {
        log_w("No matching LED entry found to remove\n");
        os_mutex_post(&g_led_entries_mutex);
        return;
    }

    /* 更新显示为当前最佳 */
    int best = led_find_best_index();
    if (best >= 0) {
        set_rgb_LED(led_entries[best].red, led_entries[best].green, led_entries[best].blue);
        g_priority_LED = led_entries[best].priority;
    } else {
        /* 无记录，关闭LED */
        set_rgb_LED(0, 0, 0);
        g_priority_LED = 0;
    }
    os_mutex_post(&g_led_entries_mutex);
}

void set_rgb_color(u16 red, u16 green, u16 blue) {
    set_rgb_ON(red, green, blue, 0);
}


//==LED LOOP=========================================================================================
void led_init(void)
{
    timer_clk = clk_get("timer");//先获取定时器的时钟源
    request_irq(timer_irq, 3, timer_isr, 0);//注册中断函数定和中断优先级
#if 1
    timer_set(1 * 10000); //初始化1ms进一次中断
#else
    //timer_freq_set(10000);//设置10K频率进中断
#endif
    //set_rgb_color(10000, 10000, 0); // 黄色！这里，与外部定义不同
    // struct pwm_platform_data pwm = {0};
    // struct pwm_platform_data pwm1 = {0};
    // // 打开PWM设备
    pwm_dev_handl_r = dev_open("pwm0", &pwm_r);
    pwm_dev_handl_g = dev_open("pwm1", &pwm_g);
    pwm_dev_handl_b = dev_open("pwm2", &pwm_b);
    if (pwm_dev_handl_r == NULL || pwm_dev_handl_g == NULL || pwm_dev_handl_b == NULL) {
        log_i("pwm open failed\n");
        return;
    }
    /* 初始化 led_entries 互斥锁 */
    os_mutex_create(&g_led_entries_mutex);

    // // 添加三个LED通道
    // pwm.pwm_ch = PWM_TIMER2_OPCH2;
    // pwm.port = IO_PORTC_09; // 红色LED
    // pwm.freq = 1000;
    // pwm.duty = 100;
    // dev_ioctl(pwm_dev_handle, PWM_RUN, (u32)&pwm);

    // pwm.pwm_ch = PWM_TIMER3_OPCH3;
    // pwm.port = IO_PORTA_10; // 绿色LED
    // pwm.freq = 1000;
    // pwm.duty = 100;
    // dev_ioctl(pwm_dev_handle, PWM_RUN, (u32)&pwm);

    // pwm1.pwm_ch = PWM_TIMER3_OPCH3;
    // pwm.port = IO_PORTA_09; // 绿色LED
    // pwm1.freq = 1000;
    // pwm1.duty = 100;
    // dev_ioctl(pwm_dev_handle, PWM_RUN, (u32)&pwm);

    // pwm.pwm_ch = PWMCH7_L;
    // pwm.port = IO_PORTA_09; // 蓝色LED
    // dev_ioctl(pwm_dev_handle, PWM_ADD_CHANNEL, (u32)&pwm);
    // dev_ioctl(pwm_dev_handle, PWM_RUN, (u32)&pwm1);
}

