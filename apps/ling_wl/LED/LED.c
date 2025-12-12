
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
static JL_TIMER_TypeDef *TMR = JL_TIMER4;//选择定时器4
static u8 timer_irq = IRQ_TIMER4_IDX;//选择定时器4

static const u8 timer_index[16] =  {0, 4, 1, 5, 2,  6,  3,  7,   8,   12,  9,    13,   10,   14,   11,    15};
static const u32 timer_table[16] = {1, 2, 4, 8, 16, 32, 64, 128, 256, 512, 1024, 2048, 4096, 8192, 16384, 32768};
static u32 timer_clk = 0;

#define AWINLINE   __attribute__((always_inline))

#define TSEC SEC_USED(.volatile_ram_code) //当中断1ms一下，建议使用函数放在内部ram
//#define TSEC
static u16 g_red___LED;     // 红色，0-10000，值越大越亮，外部进入时，做了10000-处理
static u16 g_green_LED;     // 绿色，0-10000，值越大越亮
static u16 g_blue__LED;     // 蓝色，0-10000，值越大越亮

static u16 g_priority_LED = 0; // 当前LED颜色优先级，初始为0
u16 priority_RED[10] = {0};   // 红色优先级数组
u16 priority_GREEN[10] = {0}; // 绿色优先级数组
u16 priority_BLUE[10] = {0};  // 蓝色优先级数组

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
            gpio_direction_output(LED_G_IO, 1);
            gpio_direction_output(LED_B_IO, 1);
            if( g_red___LED > 50 ){ // 红色   <     >
                gpio_direction_output(LED_R_IO, 0);
            }else {
                gpio_direction_output(LED_R_IO, 1);
            }
            //判断是否只有红色
            if( (g_green_LED <= 50) && (g_blue__LED <= 50) ){
                ms_count = 0;
            }
            break;
        case 2:
            gpio_direction_output(LED_R_IO, 1);
            gpio_direction_output(LED_B_IO, 1);
            if( g_green_LED > 50 ){ // 绿色
                gpio_direction_output(LED_G_IO, 0);
            }else {
                gpio_direction_output(LED_G_IO, 1);
            }
            //判断是否只有绿色
            if( (g_red___LED <= 50) && (g_blue__LED <= 50) ){
                ms_count = 1;
            }
            break;
        case 3:
        case 4:
            gpio_direction_output(LED_R_IO, 1);
            gpio_direction_output(LED_G_IO, 1);
            if( g_blue__LED > 50 ){ // 蓝色
                gpio_direction_output(LED_B_IO, 0);
            }else {
                gpio_direction_output(LED_B_IO, 1);
            }
            //判断是否只有蓝色
            if( (g_red___LED <= 50) && (g_green_LED <= 50) ){
                ms_count = 3;
            }
            break;
        }
    }
}

void set_rgb_ON(u16 red, u16 green, u16 blue, u16 priority){
    //10个优先级，都存储下来，高优先级生效，直到同优先级set_rgb_OFF或者更高优先级覆盖
    log_i("Event set_rgb_ON: %d  : %d  : %d , priority: %d\n", red, green, blue, priority);
    if( priority < 10 ){
        priority_RED[priority] = red;
        priority_GREEN[priority] = green;
        priority_BLUE[priority] = blue;
    }
    if( priority >= g_priority_LED ){
        g_priority_LED = priority;
        set_rgb_color(red, green, blue);
    }
}
void set_rgb_OFF(u16 priority){
    //关闭某个优先级的颜色显示，并且恢复到下一个最高优先级颜色显示
    if( priority < 10 ){
        priority_RED[priority] = 0;
        priority_GREEN[priority] = 0;
        priority_BLUE[priority] = 0;
    }
    if( priority == g_priority_LED ){
         /// 如果是当前优先级，则降低优先级，直到0；其他优先级，仅清空颜色
        if( priority > 0 ){
            //如果当前优先级颜色已经被清空，则继续降低优先级，直到找到有颜色的优先级
            while( g_priority_LED > 0 ){
                if( priority_RED[g_priority_LED] + priority_GREEN[g_priority_LED] + priority_BLUE[g_priority_LED] > 0 ){
                    break;
                }
                g_priority_LED --;
            }
        }
        set_rgb_color(priority_RED[g_priority_LED], priority_GREEN[g_priority_LED], priority_BLUE[g_priority_LED]);
    }
}

// 控制RGB颜色
void set_rgb_color(u16 red, u16 green, u16 blue) {
    g_red___LED = red;
    g_green_LED = green;
    g_blue__LED = blue;
    log_i("Event set_rgb_color: %d  : %d  : %d \n", red, green, blue);
    
    ///port_wakeup_reg(EVENT_PA8, IO_PORTA_08, EDGE_NEGATIVE, lwl_IO_key_active);
    // struct pwm_platform_data pwm = {0};
    // pwm.pwm_ch = PWM_TIMER2_OPCH2;
    // pwm.duty = red;
    // dev_ioctl(pwm_dev_handle, PWM_SET_DUTY, (u32)&pwm);
    
    // pwm.pwm_ch = PWM_TIMER3_OPCH3;
    // pwm.duty = green;
    // dev_ioctl(pwm_dev_handle, PWM_SET_DUTY, (u32)&pwm);
    
    // pwm.pwm_ch = PWM_TIMER3_OPCH3;
    // pwm.duty = blue;
    // dev_ioctl(pwm_dev_handle, PWM_SET_DUTY, (u32)&pwm);
}


//==LED LOOP=========================================================================================
void led_init(void)
{
    
    timer_clk = clk_get("timer");//先获取定时器的时钟源
    request_irq(timer_irq, 3, timer_isr, 0);//注册中断函数定和中断优先级
#if 1
    timer_set(1 * 100); //初始化1ms进一次中断
#else
    //timer_freq_set(10000);//设置10K频率进中断
#endif
    set_rgb_color(10000, 10000, 0); // 黄色！这里，与外部定义不同
    // struct pwm_platform_data pwm = {0};
    // struct pwm_platform_data pwm1 = {0};
    // // 打开PWM设备
    // pwm_dev_handle = dev_open("pwm0", &pwm);
    // // pwm_dev_handle_g = dev_open("pwm1", &pwm);
    // if (pwm_dev_handle == NULL) {
    //     log_i("pwm open failed\n");
    //     return;
    // }

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

