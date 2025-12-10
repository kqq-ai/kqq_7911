
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
static u16 g_red___LED;
static u16 g_green_LED;
static u16 g_blue__LED;

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
            break;
        case 2:
            gpio_direction_output(LED_R_IO, 1);
            gpio_direction_output(LED_B_IO, 1);
            if( g_green_LED > 50 ){ // 绿色
                gpio_direction_output(LED_G_IO, 0);
            }else {
                gpio_direction_output(LED_G_IO, 1);
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
            break;
        }
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
    timer_set(1 * 1000); //初始化1ms进一次中断
#else
    //timer_freq_set(10000);//设置10K频率进中断
#endif
    set_rgb_color(100, 100, 0); // 蓝色
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

