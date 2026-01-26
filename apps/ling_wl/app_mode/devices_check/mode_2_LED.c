#include "mode_list.h"

#include "../../LED/LED.h"

// 使用系统预定义的颜色常量
static void set_led_white(void)      { set_rgb_color(COLOR_WHITE); }
static void set_led_black(void)      { set_rgb_color(COLOR_BLACK); }
static void set_led_red(void)        { set_rgb_color(COLOR_RED); }
static void set_led_red1(void)        { set_rgb_color(5,0,0); }
static void set_led_red2(void)        { set_rgb_color(10,0,0); }
static void set_led_red3(void)        { set_rgb_color(15,0,0); }
static void set_led_red4(void)        { set_rgb_color(25,0,0); }
static void set_led_red5(void)        { set_rgb_color(50,0,0); }
static void set_led_red6(void)        { set_rgb_color(75,0,0); }
static void set_led_green(void)      { set_rgb_color(COLOR_GREEN); }
static void set_led_green1(void)        { set_rgb_color(0,5,0); }
static void set_led_green2(void)        { set_rgb_color(0,10,0); }
static void set_led_green3(void)        { set_rgb_color(0,15,0); }
static void set_led_green4(void)        { set_rgb_color(0,25,0); }
static void set_led_green5(void)        { set_rgb_color(0,50,0); }

static void set_led_blue(void)       { set_rgb_color(COLOR_BLUE); }
static void set_led_blue1(void)        { set_rgb_color(0,0,5); }
static void set_led_blue2(void)        { set_rgb_color(0,0,10); }
static void set_led_blue3(void)        { set_rgb_color(0,0,15); }
static void set_led_blue4(void)        { set_rgb_color(0,0,25); }
static void set_led_blue5(void)        { set_rgb_color(0,0,50); }

static void set_led_cyan(void)       { set_rgb_color(COLOR_CYAN); }
static void set_led_yellow(void)     { set_rgb_color(COLOR_YELLOW); }
static void set_led_purple(void)     { set_rgb_color(COLOR_PURPLE); }


static void set_led_on_red(void)        { set_rgb_ON(COLOR_RED,1); }
static void set_led_on_green(void)      { set_rgb_ON(COLOR_GREEN,2); }
static void set_led_on_blue(void)       { set_rgb_ON(COLOR_BLUE,3); }
static void set_led_on_cyan(void)       { set_rgb_ON(COLOR_CYAN,3); }
static void set_led_on_yellow(void)     { set_rgb_ON(COLOR_YELLOW,2); }
static void set_led_on_purple(void)     { set_rgb_ON(COLOR_PURPLE,1); }

static void set_led_off_red(void)        { set_rgb_OFF(COLOR_RED,1); }
static void set_led_off_green(void)      { set_rgb_OFF(COLOR_GREEN,2); }
static void set_led_off_blue(void)       { set_rgb_OFF(COLOR_BLUE,3); }
static void set_led_off_cyan(void)       { set_rgb_OFF(COLOR_CYAN,3); }
static void set_led_off_yellow(void)     { set_rgb_OFF(COLOR_YELLOW,2); }
static void set_led_off_purple(void)     { set_rgb_OFF(COLOR_PURPLE,1); }


static u32 g_mode_timer = 0;
static u8 g_led_color_index = 0;



void mode2_init()
{
    g_mode_timer = timer_get_ms();
    g_led_color_index = 0;
    set_led_white();
    log_i("MODE 2: All colors cycling started (0.5s interval), C speaker enabled");
    // 播放进入模式2的音频          LED
    play_system_music("test_led.mp3");
    log_i("Playing test_led.mp3 for MODE 2");
}

void mode2_uninit()
{
    set_led_off_red();
    set_led_off_green();
    set_led_off_blue();
    set_led_off_cyan();
    set_led_off_yellow();
    set_led_off_purple();
    set_led_green();
}

// 模式2处理函数：所有颜色循环闪烁
void process_mode_2(void)
{
    u32 current_time = timer_get_ms();
    
    // 每0.5秒(500ms)切换一次LED颜色
    if (current_time - g_mode_timer >= 500) {
        // 使用系统预定义的颜色设置函数数组 - 准确颜色优先
        void (*all_color_functions[])(void) = {
            set_led_white,         // 白色 = 红+绿+蓝
            set_led_red,           // 红色 
            set_led_red1,           // 红色 
            set_led_red2,           // 红色 
            set_led_red3,           // 红色 
            set_led_red4,           // 红色 
            set_led_red5,           // 红色 
            set_led_green,         // 绿色 
            set_led_green1,           // 红色 
            set_led_green2,           // 红色 
            set_led_green3,           // 红色 
            set_led_green4,           // 红色 
            set_led_green5,           // 红色 
            set_led_blue,          // 蓝色 
            set_led_blue1,           // 红色 
            set_led_blue2,           // 红色 
            set_led_blue3,           // 红色 
            set_led_blue4,           // 红色 
            set_led_blue5,           // 红色 
            set_led_cyan,          // 青色
            set_led_yellow,        // 黄色
            set_led_purple,        // 紫色 

            // set_led_on_red,           // 红色 
            // set_led_on_green,         // 绿色 
            // set_led_on_blue,          // 蓝色 
            // set_led_off_red,          // 关闭当前颜色
            // set_led_off_green,          // 关闭当前颜色
            // set_led_off_blue,          // 关闭当前颜色

            // set_led_on_cyan,          // 青色
            // set_led_on_yellow,        // 黄色
            // set_led_on_purple,        // 紫色 
            // set_led_off_cyan,          // 关闭当前颜色
            // set_led_off_yellow,          // 关闭当前颜色
            // set_led_off_purple,          // 关闭当前颜色

            set_led_black,          // 关闭当前颜色
            
        };
        
        // 对应的颜色名称
        const char *color_names[] = {
            "RED", "GREEN", "BLUE", "GREEN", "GREEN", "PURPLE", "WHITE", "ORANGE", "ORANGE", "GREEN", "BLUE"
        };
        
        int total_colors = sizeof(all_color_functions) / sizeof(all_color_functions[0]);
        
        all_color_functions[g_led_color_index]();
        log_i("MODE 2: All Colors LED -> %s (%d/%d)", 
              color_names[g_led_color_index], 
              g_led_color_index + 1, 
              total_colors);
        
        g_led_color_index = (g_led_color_index + 1) % total_colors;
        g_mode_timer = current_time;
    }
}