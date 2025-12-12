#include "mode_list.h"

#include "../../protocol/cp/c_uart_p.h"
#include "../../LED/LED.h"

// 使用系统预定义的颜色常量
static void set_led_red(void)        { set_rgb_ON(COLORp_RED,1); }
static void set_led_green(void)      { set_rgb_ON(COLORp_GREEN,2); }
static void set_led_blue(void)       { set_rgb_ON(COLORp_BLUE,3); }
static void set_rgb_OFF_1(void)      { set_rgb_OFF(1); }
static void set_rgb_OFF_2(void)      { set_rgb_OFF(2); }
static void set_rgb_OFF_3(void)      { set_rgb_OFF(3); }
static void set_led_cyan(void)       { set_rgb_ON(COLORp_CYAN,3); }
static void set_led_white(void)      { set_rgb_ON(COLORp_WHITE,3); }
static void set_led_black(void)      { set_rgb_ON(COLOR_BLACK,3); }
static void set_led_yellow(void)     { set_rgb_ON(COLORp_YELLOW,3); }
static void set_led_orange(void)     { uart_send_set_led_color(COLOR_ORANGE); }
static void set_led_pink(void)       { uart_send_set_led_color(COLOR_PINK); }
static void set_led_purple(void)     { set_rgb_ON(COLORp_PURPLE,3); }

static void set_led_dark_yellow(void){ uart_send_set_led_color(COLOR_DARK_YELLOW); }
static void set_led_dark_blue(void)  { uart_send_set_led_color(COLOR_DARK_BLUE); }
static void set_led_maroon(void)     { uart_send_set_led_color(COLOR_MAROON); }
static void set_led_brown(void)      { uart_send_set_led_color(COLOR_BROWN); }
static void set_led_gray(void)       { uart_send_set_led_color(COLOR_GRAY); }


static u32 g_mode_timer = 0;
static u8 g_led_color_index = 0;



void mode2_init()
{
    g_mode_timer = timer_get_ms();
    g_led_color_index = 0;
    set_led_red();
    log_i("MODE 2: All colors cycling started (0.5s interval), C speaker enabled");
    // 播放进入模式2的音频          LED
    devices_check_play_music(CONFIG_CP_VOICE_FILE_PATH"test_led.mp3");
    log_i("Playing test_led.mp3 for MODE 2");
}

void mode2_uninit()
{

}

// 模式2处理函数：所有颜色循环闪烁
void process_mode_2(void)
{
    u32 current_time = timer_get_ms();
    
    // 每0.5秒(500ms)切换一次LED颜色
    if (current_time - g_mode_timer >= 500) {
        // 使用系统预定义的颜色设置函数数组 - 准确颜色优先
        void (*all_color_functions[])(void) = {
            set_led_red,           // 红色 
            set_led_green,         // 绿色 
            set_led_blue,          // 蓝色 
            set_rgb_OFF_3,          // 关闭当前颜色
            set_rgb_OFF_1,          // 关闭当前颜色
            set_rgb_OFF_2,          // 关闭当前颜色
            set_led_purple,        // 紫色 
            set_led_yellow,        // 黄色
            set_led_cyan,          // 青色
            set_led_white,         // 白色 = 红+绿+蓝
            
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