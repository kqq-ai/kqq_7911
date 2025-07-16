/**
 * @author chuxuanhuang
 * @brief 
 *   +----> IDLE
 *   |
 * POWER_ON -------> NET_CONNECTING <-------> NET_CONNECTED
 *   |                   ^    ^                     ^
 *   |                   |    |                     |
 *   +----> DEFAULT <----+    +---- WSS_CONNECTED <-+
 * 
 * POWER 上电模式
 * IDLE 空闲模式（无任务不代表低功耗）
 * DEFAULT 默认模式：
 *     默认开启BLE ADV，提供配网功能
 * NET_CONNECTING 连接模式
 *     尝试连接WIFI
 * NET_CONNECTED 已连接WIFI的模式
 *     尝试连接后端服务器
 * WSS_CONNECTED 后端服务器连接成功模式
 *     可以进行应用逻辑处理
 */
#include "devices_check.h"
#include "../lwl_app_framework.h"
#include "log.h"
#include "../../bt_ble/server/ble_simple_server.h"
#include "../../bt_ble/client/ble_simple_client.h"
#include "../../include/app_config.h"
#include "../../protocol/cp/c_uart_p.h"

#include "../../bt_ble/client/ble_distance.h"
#include <sys_time.h>
#include "key_event.h"

// 为模式3麦克风VAD功能添加必要的头文件
// 这些头文件可能需要根据实际的SDK结构进行调整
// #include "audio/recorder.h"     // 录音器相关头文件
// #include "audio/vad.h"          // VAD相关头文件

// 为模式4加速度传感器功能添加必要的头文件
#include "../../../common/gsensor/gSensor_manage.h"  // G-Sensor管理头文件
#include "../../SC7A20/sc7a20.h"           // SC7A20陀螺仪头文件
#include "../../SC7A20/app_sc7a20_task.h"  // SC7A20任务头文件

// 补充缺少的颜色常量定义
#ifndef COLOR_YELLOW
#define COLOR_YELLOW      (0),     (5000),  (5000)    // 黄色(255,255,0) 
#endif
#ifndef COLOR_MAGENTA
#define COLOR_MAGENTA     (0),     (10000), (0)       // 洋红(255,0,255)
#endif

// LED颜色设置辅助函数（使用系统预定义的颜色参数）
static void set_led_color_rgb(uint16_t r, uint16_t g, uint16_t b)
{
    uart_send_set_led_color(r, g, b);
}

// 使用系统预定义的颜色常量
static void set_led_red(void) { uart_send_set_led_color(COLOR_RED); }
static void set_led_green(void) { uart_send_set_led_color(COLOR_GREEN); }
static void set_led_blue(void) { uart_send_set_led_color(COLOR_BLUE); }
static void set_led_cyan(void) { uart_send_set_led_color(COLOR_CYAN); }
static void set_led_yellow(void) { uart_send_set_led_color(COLOR_YELLOW); }
static void set_led_magenta(void) { uart_send_set_led_color(COLOR_MAGENTA); }
static void set_led_white(void) { uart_send_set_led_color(COLOR_WHITE); }
static void set_led_black(void) { uart_send_set_led_color(COLOR_BLACK); }
static void set_led_orange(void) { uart_send_set_led_color(COLOR_ORANGE); }
static void set_led_pink(void) { uart_send_set_led_color(COLOR_PINK); }
static void set_led_purple(void) { uart_send_set_led_color(COLOR_PURPLE); }
static void set_led_dark_yellow(void) { uart_send_set_led_color(COLOR_DARK_YELLOW); }
static void set_led_dark_blue(void) { uart_send_set_led_color(COLOR_DARK_BLUE); }
static void set_led_maroon(void) { uart_send_set_led_color(COLOR_MAROON); }
static void set_led_brown(void) { uart_send_set_led_color(COLOR_BROWN); }
static void set_led_gray(void) { uart_send_set_led_color(COLOR_GRAY); }

// 定义工作模式枚举
typedef enum {
    WORK_MODE_INIT = 0,     // 初始模式（红灯常亮）
    WORK_MODE_1,            // 模式1（所有颜色循环闪烁）
    WORK_MODE_2,            // 模式2（循环播放音频）
    WORK_MODE_3,            // 模式3（麦克风VAD检测）
    WORK_MODE_4,            // 模式4（加速度传感器测试）
    WORK_MODE_MAX           // 模式总数
} work_mode_e;

// 全局变量
static work_mode_e g_current_mode = WORK_MODE_INIT;
static u32 g_mode_timer = 0;
static u8 g_led_color_index = 0;
static u8 g_audio_playing = 0;
static u8 g_mode2_playing_prompt = 1; // 模式2状态：1=播放提示音，0=循环播放测试音

// 模式3 VAD相关变量
static int g_vad_state = 0;          // 当前VAD状态 (0=无语音, 1=有语音)
static int g_vad_state_prev = 0;     // 前一次VAD状态
static u32 g_vad_start_time = 0;     // 语音开始时间
static u32 g_vad_duration = 0;       // 语音持续时间
static u8 g_mode3_vad_enabled = 0;   // 模式3 VAD是否已启用

// 模式4 加速度传感器相关变量
extern void *g_iic_hdl;                            // 外部IIC句柄声明（在app_sc7a20_task.c中定义）
static void *g_sc7a20_iic_hdl = NULL;              // SC7A20 IIC设备句柄
static u8 g_mode4_gsensor_enabled = 0;             // 模式4 G-Sensor是否已启用
static s16 g_accel_x = 0, g_accel_y = 0, g_accel_z = 0;  // 三轴加速度值
static s16 g_accel_x_prev = 0, g_accel_y_prev = 0, g_accel_z_prev = 0;  // 前一次三轴加速度值
static u32 g_motion_count = 0;                 // 运动检测计数
static u8 g_motion_detected = 0;               // 运动检测状态 (0=静止, 1=小幅晃动, 2=大幅晃动)
static u32 g_small_motion_count = 0;           // 小幅晃动计数
static u32 g_large_motion_count = 0;           // 大幅晃动计数

// 模式切换函数声明
static void switch_to_mode(work_mode_e new_mode);
static void process_mode_init(void);
static void process_mode_1(void);
static void process_mode_2(void);
static void process_mode_3(void);
static void process_mode_4(void);
static void devices_check_play_music(const char *fname);
static void mode2_audio_callback(void *priv);
static void mode3_audio_callback(void *priv);
static void mode4_audio_callback(void *priv);
static void check_key_status(void);

// 模式3 VAD功能声明
static void mode3_vad_callback(void* priv, int type);
static int mode3_vad_init(void);
static void mode3_vad_uninit(void);
static void mode3_vad_status_update(void);

// 模式4 加速度传感器功能声明
static int mode4_gsensor_init(void);
static void mode4_gsensor_uninit(void);
static int mode4_gsensor_read_data(void);
static void mode4_gsensor_status_update(void);
static void mode4_motion_detection(void);

// 模式处理函数数组
static void (*mode_process_functions[])(void) = {
    process_mode_init,
    process_mode_1,
    process_mode_2,
    process_mode_3,
    process_mode_4
};

// 模式名称数组（用于日志）
static const char *mode_names[] = {
    "INIT",
    "MODE_1_ALL_COLORS",
    "MODE_2_AUDIO_LOOP",
    "MODE_3_MIC_VAD",
    "MODE_4_GSENSOR_TEST"
};

// 模式切换函数
static void switch_to_mode(work_mode_e new_mode)
{
    if (new_mode >= WORK_MODE_MAX) {
        log_i("Invalid mode: %d, staying in current mode", new_mode);
        return;
    }
    
    log_i("=== MODE SWITCH ===");
    log_i("From: %s (%d) -> To: %s (%d)", 
          mode_names[g_current_mode], g_current_mode,
          mode_names[new_mode], new_mode);
    
    // 退出当前模式时的清理操作
    if (g_current_mode == WORK_MODE_3 && g_mode3_vad_enabled) {
        log_i("Cleaning up MODE 3 VAD functionality");
        mode3_vad_uninit();
    }
    if (g_current_mode == WORK_MODE_4 && g_mode4_gsensor_enabled) {
        log_i("Cleaning up MODE 4 G-Sensor functionality");
        mode4_gsensor_uninit();
    }
    
    g_current_mode = new_mode;
    g_mode_timer = timer_get_ms();
    g_led_color_index = 0;
    
    // 模式切换时的初始化操作
    switch (new_mode) {
    case WORK_MODE_INIT:
        set_led_red();
        log_i("INIT MODE: Red LED ON");
        // 播放进入模式0的音频
        devices_check_play_music(CONFIG_VOICE_PROMPT_FILE_PATH"start_test.mp3");
        log_i("Playing start_test.mp3 for INIT mode");
        break;
    case WORK_MODE_1:
        set_led_red();
        log_i("MODE 1: All colors cycling started (0.5s interval)");
        // 播放进入模式1的音频          LED
        devices_check_play_music(CONFIG_VOICE_PROMPT_FILE_PATH"led_test.mp3");
        log_i("Playing led_test.mp3 for MODE 1");
        break;
    case WORK_MODE_2:
        set_led_white();
        g_audio_playing = 1;  // 设置为1，表示正在播放loud_test.mp3
        g_mode2_playing_prompt = 1;  // 标记正在播放提示音
        log_i("MODE 2: Audio loop mode started");
        // 播放进入模式2的音频，使用回调    
        lwl_app_context_t *context = lwl_app_core_context_get_instance();
        app_music_play_local_music_with_callback(
            context->music_player,
            CONFIG_VOICE_PROMPT_FILE_PATH"loud_test.mp3",
            mode2_audio_callback, NULL);
        log_i("Playing loud_test.mp3 for MODE 2 with callback");
        break;
    case WORK_MODE_3:
        set_led_green();  // 初始绿灯表示无语音
        g_audio_playing = 1;  // 设置为1，表示正在播放micro_test.mp3
        g_mode3_vad_enabled = 0;  // VAD尚未启用
        log_i("MODE 3: micro VAD detection mode started");
        // 播放进入模式3的音频，使用回调
        lwl_app_context_t *context3 = lwl_app_core_context_get_instance();
        app_music_play_local_music_with_callback(
            context3->music_player,
            CONFIG_VOICE_PROMPT_FILE_PATH"micro_test.mp3",
            mode3_audio_callback, NULL);
        log_i("Playing micro_test.mp3 for MODE 3 with callback");
        break;
    case WORK_MODE_4:
        set_led_green();  // 初始绿灯表示G-Sensor模式（静止状态）
        g_audio_playing = 1;  // 设置为1，表示正在播放SDcardMusic
        g_mode4_gsensor_enabled = 0;  // G-Sensor尚未启用
        g_motion_detected = 0;  // 重置运动检测状态
        g_motion_count = 0;     // 重置运动计数
        g_small_motion_count = 0; // 重置小幅晃动计数
        g_large_motion_count = 0; // 重置大幅晃动计数
        log_i("MODE 4: G-Sensor accelerometer test mode started");
        // 播放进入模式4的音频，使用回调
        lwl_app_context_t *context4 = lwl_app_core_context_get_instance();
        app_music_play_local_music_with_callback(
            context4->music_player,
            CONFIG_VOICE_PROMPT_FILE_PATH"gyro_test.mp3",  // 使用gyro_test音频文件
            mode4_audio_callback, NULL);
        log_i("Playing gyro_test.mp3 for MODE 4 with callback");
        break;
    default:
        break;
    }
}

// 初始模式处理函数
static void process_mode_init(void)
{
    // 初始模式只需要保持红灯常亮，等待按键切换
    // 无需特殊处理
}

// 模式1处理函数：所有颜色循环闪烁
static void process_mode_1(void)
{
    u32 current_time = timer_get_ms();
    
    // 每0.5秒(500ms)切换一次LED颜色
    if (current_time - g_mode_timer >= 500) {
        // 使用系统预定义的颜色设置函数数组 - 准确颜色优先
        void (*all_color_functions[])(void) = {
            set_led_red,           // 红色 - 准确基准色
            set_led_green,         // 绿色 - 准确基准色  
            set_led_blue,          // 蓝色 - 准确基准色
            set_led_purple,        // 紫色 - 准确基准色
            set_led_cyan,          // 青色 = 绿+蓝
            set_led_yellow,        // 黄色 = 红+绿
            set_led_magenta,       // 洋红 = 红+蓝 (类似紫色但更偏红)
            set_led_white,         // 白色 = 红+绿+蓝
            set_led_orange,        // 橙色 = 红+少绿 (调整更偏红)
            set_led_pink,          // 粉红 = 红+适量绿蓝
            set_led_dark_yellow,   // 深黄 = 少红+少绿
            set_led_dark_blue,     // 深蓝 = 少量蓝
            set_led_maroon,        // 栗色 = 少量红
            set_led_brown,         // 褐色 = 较多红+少绿蓝
            set_led_gray,          // 灰色 = 中等亮度全色
            set_led_black          // 黑色 = 全关
        };
        
        // 对应的颜色名称
        const char *color_names[] = {
            "RED", "GREEN", "BLUE", "PURPLE", "CYAN", "YELLOW", 
            "MAGENTA", "WHITE", "ORANGE", "PINK", "DARK_YELLOW", 
            "DARK_BLUE", "MAROON", "BROWN", "GRAY", "BLACK"
        };
        
        int total_colors = sizeof(all_color_functions) / sizeof(all_color_functions[0]);
        
        all_color_functions[g_led_color_index]();
        log_i("MODE 1: All Colors LED -> %s (%d/%d)", 
              color_names[g_led_color_index], 
              g_led_color_index + 1, 
              total_colors);
        
        g_led_color_index = (g_led_color_index + 1) % total_colors;
        g_mode_timer = current_time;
    }
}

// 模式2处理函数：先播放提示音，然后循环播放测试音频
static void process_mode_2(void)
{
    lwl_app_context_t *context = lwl_app_core_context_get_instance();
    
    // 如果不在播放提示音阶段，且当前没有音频在播放，则开始播放循环音频
    if (!g_mode2_playing_prompt && !g_audio_playing) {
        log_i("MODE 2: Starting loop audio playback");
        
        // 播放循环测试音频，使用回调
        int err = app_music_play_local_music_with_callback(
            context->music_player, 
            CONFIG_VOICE_PROMPT_FILE_PATH"test1hz5s.mp3", 
            mode2_audio_callback, NULL);
            
        if (err == 0) {
            g_audio_playing = 1;
            log_i("MODE 2: Loop audio started successfully");
        } else {
            log_i("MODE 2: Loop audio start failed, error: %d", err);
        }
    }
}

// 模式2音频播放回调函数
static void mode2_audio_callback(void *priv)
{
    if (g_mode2_playing_prompt) {
        // 提示音播放完成，开始播放循环音频
        log_i("MODE 2: Prompt audio callback - finished, starting loop audio");
        g_mode2_playing_prompt = 0;
        g_audio_playing = 0;
    } else {
        // 循环音频播放完成，准备重新播放
        log_i("MODE 2: Loop audio callback - finished, will restart");
        g_audio_playing = 0;
    }
}

// 模式3音频播放回调函数
static void mode3_audio_callback(void *priv)
{
    log_i("MODE 3: Prompt audio callback - finished, starting VAD monitoring");
    g_audio_playing = 0;
    
    // 提示音播放完成，启动VAD功能
    if (mode3_vad_init() == 0) {
        g_mode3_vad_enabled = 1;
        log_i("MODE 3: VAD monitoring enabled successfully");
    } else {
        log_e("MODE 3: Failed to enable VAD monitoring");
    }
}

//=================================================================================================
// 模式3 VAD功能相关函数
//=================================================================================================
/**
 * @brief VAD回调函数 - 处理语音活动检测事件
 * @param priv 私有数据指针
 * @param type VAD事件类型 (1=语音开始, 0=语音结束)
 */
static void mode3_vad_callback(void* priv, int type)
{
    u32 current_time = timer_get_ms();
    
    if (type == 1) {
        // 语音开始
        g_vad_state = 1;
        g_vad_start_time = current_time;
        log_i("[MODE 3 VAD] Voice activity START at %d ms", current_time);
        
        // 开始说话时打印10行分隔符
        for (int i = 0; i < 10; i++) {
            printf("---------------------------\r\n");
        }
        
        // 检测到语音时设置红灯
        set_led_red();
        log_i("MODE 3: Voice detected - LED set to RED");
        
    } else {
        // 语音结束
        g_vad_state = 0;
        g_vad_duration = current_time - g_vad_start_time;
        log_i("[MODE 3 VAD] Voice activity END at %d ms, duration: %d ms", current_time, g_vad_duration);
        
        // 结束说话时打印10行分隔符
        for (int i = 0; i < 10; i++) {
            printf("|||||||||||||||||||||||||||||||\r\n");
        }
        
        // 语音结束时恢复绿灯
        set_led_green();
        log_i("MODE 3: Voice ended - LED set to GREEN");
        
        // 如果语音持续时间超过1秒，可以触发某些动作
        if (g_vad_duration > 1000) {
            log_i("[MODE 3 VAD] Long voice detected (%d ms), triggering action...", g_vad_duration);
        }
    }
}

/**
 * @brief 启动VAD功能
 * @return 0成功, -1失败
 */
static int mode3_vad_init(void)
{
    lwl_app_context_t *context = lwl_app_core_context_get_instance();
    
    if (!context || !context->recorder) {
        log_e("[MODE 3 VAD] Recorder not available");
        return -1;
    }
    
    // 设置VAD回调函数
    int ret = recorder_set_vad_cbk(context->recorder, mode3_vad_callback, NULL);
    if (ret != 0) {
        log_e("[MODE 3 VAD] Failed to set VAD callback: %d", ret);
        return -1;
    }
    
    // 启动录音器以启用VAD功能
    ret = recorder_start(context->recorder);
    if (ret != 0) {
        log_e("[MODE 3 VAD] Failed to start recorder: %d", ret);
        return -1;
    }
    
    log_i("[MODE 3 VAD] VAD functionality initialized and started");
    return 0;
}

/**
 * @brief 停止VAD功能
 */
static void mode3_vad_uninit(void)
{
    lwl_app_context_t *context = lwl_app_core_context_get_instance();
    
    if (context && context->recorder) {
        // 停止录音器
        recorder_stop(context->recorder);
        // 清除VAD回调
        recorder_set_vad_cbk(context->recorder, NULL, NULL);
        log_i("[MODE 3 VAD] VAD functionality stopped");
    }
    
    g_mode3_vad_enabled = 0;
    g_vad_state = 0;
    g_vad_state_prev = 0;
}

/**
 * @brief 更新VAD状态信息并打印
 */
static void mode3_vad_status_update(void)
{
    static u32 last_print_time = 0;
    static u32 voice_detect_count = 0;  // 语音检测次数统计
    static u32 total_voice_time = 0;    // 总语音时间统计
    u32 current_time = timer_get_ms();
    
    // 检测VAD状态变化
    if (g_vad_state != g_vad_state_prev) {
        if (g_vad_state == 1) {
            voice_detect_count++;
            log_i("[MODE 3 VAD] === VOICE DETECTED === (#%d)", voice_detect_count);
            printf("[VAD] Voice start detected at %d ms\r\n", current_time);
        } else {
            total_voice_time += g_vad_duration;
            log_i("[MODE 3 VAD] === VOICE STOPPED === (Duration: %d ms, Total: %d ms)", 
                  g_vad_duration, total_voice_time);
            printf("[VAD] Voice end detected at %d ms, duration: %d ms\r\n", 
                   current_time, g_vad_duration);
        }
        g_vad_state_prev = g_vad_state;
    }
    
    // 每3秒打印一次状态信息
    if (current_time - last_print_time >= 3000) {
        if (g_vad_state == 1) {
            u32 speaking_duration = current_time - g_vad_start_time;
            log_i("[MODE 3 VAD] Status: SPEAKING (Duration: %d ms)", speaking_duration);
            printf("[VAD] Currently speaking: %d ms\r\n", speaking_duration);
        } else {
            log_i("[MODE 3 VAD] Status: SILENT (Count: %d, Total voice time: %d ms)", 
                  voice_detect_count, total_voice_time);
            printf("[VAD] Silent mode - Voice events: %d, Total time: %d ms\r\n", 
                   voice_detect_count, total_voice_time);
        }
        last_print_time = current_time;
    }
}

// 模式3处理函数：麦克风VAD检测模式
static void process_mode_3(void)
{
    // 如果VAD功能已启用，则监控VAD状态
    if (g_mode3_vad_enabled) {
        mode3_vad_status_update();
    }
    
    // 如果提示音还在播放中，则等待播放完成
    if (g_audio_playing) {
        // 等待音频播放完成，无需特殊处理
        return;
    }
}

// 模式4处理函数：加速度传感器测试模式
static void process_mode_4(void)
{
    // 如果G-Sensor功能已启用，则监控传感器状态
    if (g_mode4_gsensor_enabled) {
        mode4_gsensor_status_update();
        mode4_motion_detection();
    }
    
    // 如果提示音还在播放中，则等待播放完成
    if (g_audio_playing) {
        // 等待音频播放完成，无需特殊处理
        return;
    }
}

// 模式4音频播放回调函数
static void mode4_audio_callback(void *priv)
{
    log_i("MODE 4: Prompt audio callback - finished, starting G-Sensor monitoring");
    g_audio_playing = 0;
    
    // 提示音播放完成，启动G-Sensor功能
    if (mode4_gsensor_init() == 0) {
        g_mode4_gsensor_enabled = 1;
        log_i("MODE 4: G-Sensor monitoring enabled successfully");
    } else {
        log_e("MODE 4: Failed to enable G-Sensor monitoring");
    }
}

//=================================================================================================
// 模式4 G-Sensor功能相关函数
//=================================================================================================
/**
 * @brief 启动G-Sensor功能
 * @return 0成功, -1失败
 */
static int mode4_gsensor_init(void)
{
    // 获取外部已初始化的SC7A20 IIC句柄
    if (!g_iic_hdl) {
        log_e("[MODE 4 GSENSOR] SC7A20 IIC handle not initialized");
        return -1;
    }
    
    g_sc7a20_iic_hdl = g_iic_hdl;
    
    // 初始化变量
    g_accel_x = g_accel_y = g_accel_z = 0;
    g_accel_x_prev = g_accel_y_prev = g_accel_z_prev = 0;
    g_motion_count = 0;
    g_motion_detected = 0;
    g_small_motion_count = 0;
    g_large_motion_count = 0;
    
    log_i("[MODE 4 GSENSOR] SC7A20 G-Sensor functionality initialized and started");
    return 0;
}

/**
 * @brief 停止G-Sensor功能
 */
static void mode4_gsensor_uninit(void)
{
    if (g_sc7a20_iic_hdl) {
        g_sc7a20_iic_hdl = NULL;
        log_i("[MODE 4 GSENSOR] SC7A20 G-Sensor functionality stopped");
    }
    
    g_mode4_gsensor_enabled = 0;
    g_motion_detected = 0;
    g_motion_count = 0;
    g_small_motion_count = 0;
    g_large_motion_count = 0;
}

/**
 * @brief 读取G-Sensor数据
 * @return 0成功, -1失败
 */
static int mode4_gsensor_read_data(void)
{
    if (!g_sc7a20_iic_hdl) {
        return -1;
    }
    
    // 保存前一次的值
    g_accel_x_prev = g_accel_x;
    g_accel_y_prev = g_accel_y;
    g_accel_z_prev = g_accel_z;
    
    // 使用SC7A20接口读取真实的加速度数据
    if (read_accel_data(g_sc7a20_iic_hdl, &g_accel_x, &g_accel_y, &g_accel_z) != 0) {
        // 如果读取失败，返回错误
        return -1;
    }
    
    return 0;
}

/**
 * @brief 检测运动状态
 */
static void mode4_motion_detection(void)
{
    // 定义运动检测阈值
    #define SMALL_MOTION_THRESHOLD 350   // 小幅晃动阈值（降低以便检测）
    #define LARGE_MOTION_THRESHOLD 6000   // 大幅晃动阈值（保持较高）
    
    // 计算三轴加速度变化
    s16 delta_x = g_accel_x - g_accel_x_prev;
    s16 delta_y = g_accel_y - g_accel_y_prev;
    s16 delta_z = g_accel_z - g_accel_z_prev;
    
    // 计算总变化量
    u32 total_delta = (u32)(abs(delta_x) + abs(delta_y) + abs(delta_z));
    
    u8 motion_now = 0;  // 0=静止, 1=小幅晃动, 2=大幅晃动
    
    if (total_delta >= LARGE_MOTION_THRESHOLD) {
        motion_now = 2;  // 大幅晃动
    } else if (total_delta >= SMALL_MOTION_THRESHOLD) {
        motion_now = 1;  // 小幅晃动
    } else {
        motion_now = 0;  // 静止
    }
    
    // 检测运动状态变化
    if (motion_now != g_motion_detected) {
        if (motion_now == 2) {
            // 检测到大幅晃动
            g_large_motion_count++;
            g_motion_count++;
            set_led_red();  // 大幅晃动时红灯
            log_i("[MODE 4 GSENSOR] === LARGE MOTION DETECTED === (#%d total, #%d large)", 
                  g_motion_count, g_large_motion_count);
            log_i("[MODE 4 GSENSOR] Delta: X=%d, Y=%d, Z=%d, Total=%d (LARGE)", 
                  delta_x, delta_y, delta_z, total_delta);
            
            // 打印大幅运动开始的分隔符
            for (int i = 0; i < 5; i++) {
                printf(">>> LARGE MOTION START <<<\r\n");
            }
        } else if (motion_now == 1) {
            // 检测到小幅晃动
            g_small_motion_count++;
            g_motion_count++;
            set_led_blue();  // 小幅晃动时蓝灯
            log_i("[MODE 4 GSENSOR] === SMALL MOTION DETECTED === (#%d total, #%d small)", 
                  g_motion_count, g_small_motion_count);
            log_i("[MODE 4 GSENSOR] Delta: X=%d, Y=%d, Z=%d, Total=%d (SMALL)", 
                  delta_x, delta_y, delta_z, total_delta);
            
            // 打印小幅运动开始的分隔符
            for (int i = 0; i < 3; i++) {
                printf(">>> SMALL MOTION START <<<\r\n");
            }
        } else {
            // 运动停止
            set_led_green();  // 静止时绿灯
            if (g_motion_detected == 2) {
                log_i("[MODE 4 GSENSOR] === LARGE MOTION STOPPED ===");
                // 打印大幅运动结束的分隔符
                for (int i = 0; i < 5; i++) {
                    printf(">>> LARGE MOTION STOP <<<\r\n");
                }
            } else if (g_motion_detected == 1) {
                log_i("[MODE 4 GSENSOR] === SMALL MOTION STOPPED ===");
                // 打印小幅运动结束的分隔符
                for (int i = 0; i < 3; i++) {
                    printf(">>> SMALL MOTION STOP <<<\r\n");
                }
            }
        }
        g_motion_detected = motion_now;
    }
}

/**
 * @brief 更新G-Sensor状态信息并打印
 */
static void mode4_gsensor_status_update(void)
{
    static u32 last_print_time = 0;
    static u32 last_data_read_time = 0;
    static u32 read_error_count = 0;
    u32 current_time = timer_get_ms();
    
    // 每50ms读取一次传感器数据
    if (current_time - last_data_read_time >= 50) {
        if (mode4_gsensor_read_data() != 0) {
            read_error_count++;
            // 如果连续读取失败次数过多，打印错误信息
            if (read_error_count % 20 == 1) {  // 每20次失败打印一次错误
                log_e("[MODE 4 GSENSOR] Failed to read sensor data (error count: %d)", read_error_count);
            }
        } else {
            // 读取成功，重置错误计数
            read_error_count = 0;
        }
        last_data_read_time = current_time;
    }
    
    // 每0.5秒(500ms)打印一次陀螺仪状态信息
    if (current_time - last_print_time >= 500) {
        const char *motion_status;
        if (g_motion_detected == 2) {
            motion_status = "LARGE_MOTION";
        } else if (g_motion_detected == 1) {
            motion_status = "SMALL_MOTION";
        } else {
            motion_status = "STATIC";
        }
        
        log_i("[MODE 4 GSENSOR] Gyroscope Status: X=%d, Y=%d, Z=%d", g_accel_x, g_accel_y, g_accel_z);
        log_i("[MODE 4 GSENSOR] Motion: %s, Total: %d, Small: %d, Large: %d, Errors: %d", 
              motion_status, g_motion_count, g_small_motion_count, g_large_motion_count, read_error_count);
        printf("[GYROSCOPE] X:%d Y:%d Z:%d Motion:%s Total:%d Small:%d Large:%d Errors:%d\r\n", 
               g_accel_x, g_accel_y, g_accel_z, motion_status, 
               g_motion_count, g_small_motion_count, g_large_motion_count, read_error_count);
        last_print_time = current_time;
    }
}

static void devices_check_play_music(const char *fname) 
{
    lwl_app_context_t *context = lwl_app_core_context_get_instance();
    
    // g_music_player.play_voice_prompt = 1;
    log_i("play: %s", fname);
    int err = app_music_play_local_music_with_callback(context->music_player, fname, NULL, NULL);
    if (err) {
        // key_event_enable();
        // g_music_player.play_voice_prompt = 0;
    }
}

//=================================================================================================
// pDev response callback
//=================================================================================================
static void devices_check_cb(bka_cmd_code_e code, void *priv)
{
    lwl_eend_dev_t *pdev = (lwl_eend_dev_t*)(priv);
    log_i("devices_check_cb\n");
    
    char sn_string[SN_SIZE * 2 + 1] = "(NULL)";
    if (pdev) {
        pdev->get_string(pdev, sn_string, sizeof(sn_string));
    }
    switch(code) {
    case BKA_CMD_CODE_COMPL:
        log_i("Switch PortE %s OK\n", sn_string);
        devices_check_play_music(CONFIG_VOICE_PROMPT_FILE_PATH"NetCfgSucc.mp3");
        break;
    case BKA_CMD_CODE_DEVLOST:
        log_i("Switch PortE Dev Lost\n");
        break;
    case BKA_CMD_CODE_BUFFER_FULL:
        log_i("Switch PortE BLE TX Buffer Full\n");
        break;
    case BKA_CMD_CODE_SEARCH_PROFILE_TIMEOUT:
        log_i("Switch PoertE search GATT timeout\n");
        break;
    case BKA_CMD_CODE_E_TURN_ON_TIMEOUT:
        log_i("Switch PortE Timeout\n");
        break;
    case BKA_CMD_CODE_E_TURN_ON_ALREADY_BOUND:
        log_i("Switch PortE Failed: E is already bound\n");
        break;
    case BKA_CMD_CODE_UNEXPECTED:
        log_i("Switch PortE Error\n");
        break;
    }
    if (code != BKA_CMD_CODE_COMPL) {
    }
}


int devices_check_e_turn_on(lwl_eend_dev_t *p_eend_dev, bka_cmd_cb_funp_t cb, void *priv, uint32_t *cmd_id_out) 
{
    ASSERT(p_eend_dev != NULL);
    lwl_app_context_t *ctx = lwl_app_core_context_get_instance();

    uart_send_set_sn(p_eend_dev->sn);

    u8 cmd_turn_on[] = {0xA0, 0x01, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    memcpy(cmd_turn_on + 2, ctx->mac, 6);

    return ble_keep_alive_device_push_cmd(p_eend_dev, cmd_turn_on, sizeof(cmd_turn_on), cb, priv, cmd_id_out);
}

lwl_eend_dev_t *pdev;

//=================================================================================================
// LWL event callback
//=================================================================================================
static void devices_check_lwl_event_handler_cb(lwl_event_t *e)
{
    static int en = 1;
    lwl_eend_dev_t *p_eend_dev = e->distance_change.pdev;
    switch (e->event_type) {
    case LWL_EVENT_TYPE_EEND_DEV_CLOSEST_CHANGE: {
        log_i("LWL_EVENT_TYPE_EEND_DEV_CLOSEST_CHANGE %d %d", pdev, p_eend_dev);
        if (p_eend_dev != NULL) {
            char sn_string[SN_SIZE] = "(NULL)";
            p_eend_dev->get_string(p_eend_dev, sn_string, sizeof(sn_string));
            log_i("LWL_EVENT_TYPE_EEND_DEV_CLOSEST_CHANGE %s", sn_string);
        }
        if (pdev != p_eend_dev) {
            pdev = p_eend_dev;
            uint32_t cmd_id;
            int ret = devices_check_e_turn_on(pdev, devices_check_cb, pdev, &cmd_id);
            log_i("turn on cmd_id: %d", ret);
        }
    }   break;
    case LWL_EVENT_TYPE_EEND_DEV_DISTANCE_CHANGE: {
        char sn_string[SN_SIZE * 2 + 1] = "(NULL)";
        p_eend_dev->get_string(p_eend_dev, sn_string, sizeof(sn_string));
        log_i("[MEASDIST] %s: %d %d\r\n", sn_string, p_eend_dev->distance_info.rssi, p_eend_dev->distance_info.distance_type);
    }   break;
    case LWL_EVENT_TYPE_EEND_DEV_NEW_IN:
        log_i("ORMET_NEW_DEVICE_IN");
        break;
    case LWL_EVENT_TYPE_UART_IO_KEY_SHORT_CLICK: {
        log_i("=== UART IO KEY SHORT CLICK DETECTED ===");
        log_i("Button pressed via UART interface");
        log_i("Event timestamp: %d ms", timer_get_ms());
        log_i("Current mode: %s (%d)", mode_names[g_current_mode], g_current_mode);
        
        // 按键切换模式逻辑
        work_mode_e next_mode;
        switch (g_current_mode) {
        case WORK_MODE_INIT:
            next_mode = WORK_MODE_1;
            break;
        case WORK_MODE_1:
            next_mode = WORK_MODE_2;
            break;
        case WORK_MODE_2:
            next_mode = WORK_MODE_3;
            break;
        case WORK_MODE_3:
            next_mode = WORK_MODE_4;
            break;
        case WORK_MODE_4:
        default:
            next_mode = WORK_MODE_INIT; // 模式4后回到初始模式，形成循环
            break;
        }
        
        switch_to_mode(next_mode);

    }   break;
    case LWL_EVENT_TYPE_UART_SYNC:
        log_i("SYNC");
        log_i("battery: %d", e->uart_cmd_sync.battery);
        // if(e->uart_cmd_sync.battery < 30 )
        //     uart_send_set_led_color(6000, 10000, 10000); //red~ low power
        uart_response_synch_status(1, 12);/// 音量 E only
        break;
    case LWL_EVENT_TYPE_UART_TOUCH_KEY_DOWN:
        log_i("=== UART TOUCH KEY DOWN DETECTED ===");
        log_i("Touch key pressed down");
        log_i("Event timestamp: %d ms", timer_get_ms());
        log_i("Current mode: %s (%d)", mode_names[g_current_mode], g_current_mode);
        
        // 触摸按键检测时设置黄灯（所有模式都支持）
        set_led_yellow();
        log_i("Touch key detected, LED set to YELLOW temporarily");
        break;
    case LWL_EVENT_TYPE_UART_SHUTDOWN:
        log_i("SHUTDOWN");
        break;
    case LWL_EVENT_TYPE_UART_MOVE_MORE_THAN_ONE_METER:
        log_i("MOVE_MORE_THAN_ONE_METER");
        break;
    }
}

static int devices_check_init(void)
{
    lwl_app_context_t *g_app_ctx = lwl_app_core_context_get_instance();
    
    msleep(5000);
    log_i("=== DEFAULT MODE INITIALIZED ===");
    log_i("Multi-mode system ready");
    log_i("Key detection enabled");
    log_i("UART event monitoring active");
    
    ble_simple_server_module_enable(0);
    ble_simple_client_module_enable(1);
    /* UART指令开启C端le audio */
    uart_send_open_le_audio(g_app_ctx->mac);

    /* 注册lwl事件处理回调 */
    lwl_event_handler_add(devices_check_lwl_event_handler_cb);
    
    // 初始化模式系统，进入初始模式（红灯常亮）
    switch_to_mode(WORK_MODE_INIT);
    
    log_i("=== MODE SYSTEM INITIALIZED ===");
    log_i("Press key to switch modes:");
    log_i("INIT -> MODE_1 (All Colors Cycling) -> MODE_2 (Audio Loop) -> MODE_3 (Microphone VAD) -> MODE_4 (G-Sensor Test) -> INIT (Loop)");
    log_i("Available colors (16 total): Red*, Green*, Blue*, Purple*, Cyan, Yellow, Magenta, White, Orange, Pink, Dark_Yellow, Dark_Blue, Maroon, Brown, Gray, Black");
    log_i("Color basis: Red/Green/Blue/Purple are accurate (* = accurate), others adjusted");
    log_i("MODE 3: Red LED = Voice detected, Green LED = Silent");
    log_i("MODE 4: Green LED = Static, Blue LED = Small motion, Red LED = Large motion, Gyroscope data every 0.5s");
    log_i("Touch Key: Yellow LED = Touch detected (all modes)");
    
    return 0;
}

static int devices_check_unint(void)
{
    // 如果当前在模式3且VAD已启用，则清理VAD功能
    if (g_current_mode == WORK_MODE_3 && g_mode3_vad_enabled) {
        mode3_vad_uninit();
    }
    // 如果当前在模式4且G-Sensor已启用，则清理G-Sensor功能
    if (g_current_mode == WORK_MODE_4 && g_mode4_gsensor_enabled) {
        mode4_gsensor_uninit();
    }
    
    lwl_event_handler_remove(devices_check_lwl_event_handler_cb);
    return 0;
}

#define HEAT_TIMEOUT 1000
void app_devices_check_mode(void)
{
    devices_check_init();
    static u32 g_heat_timer;
    g_heat_timer = timer_get_ms();

    log_i("=== ENTERING MAIN LOOP ===");
    log_i("Multi-mode system active");

    while (1) {
        // 主循环延时
        mdelay(10);
        
        // 执行当前模式的处理函数
        if (g_current_mode < WORK_MODE_MAX && mode_process_functions[g_current_mode]) {
            mode_process_functions[g_current_mode]();
        }
        
        // 检测按键状态（保留原有的GPIO按键检测功能）
        check_key_status();
        if (false && WIFI_CONNECT_SUCC == get_wifi_connect_status(NULL)) {
            lwl_app_mode_switch_to(APP_NET_CONNECTED_MODE);
        }
        
        u32 g_curr_time = timer_get_ms();

        // 定期输出设备信息（减少频率以避免日志过多）
        if (g_curr_time - g_heat_timer >= HEAT_TIMEOUT * 5) { // 每5秒输出一次
            log_i("=== SYSTEM STATUS ===");
            log_i("Current mode: %s (%d)", mode_names[g_current_mode], g_current_mode);
            log_i("System uptime: %d ms", g_curr_time);
            dump_all_eend_info();
            g_heat_timer = g_curr_time;
        }

        if (lwl_app_mode_exitting()) {
            log_i("=== EXITING MULTI-MODE SYSTEM ===");
            devices_check_unint();
            return;
        }
    }
}

// 添加按键状态检测函数
static void check_key_status(void)
{
    static u32 last_key_check_time = 0;
    static u8 last_key_state = 0;
    
    u32 current_time = timer_get_ms();
    
    // 每100ms检查一次按键状态
    if (current_time - last_key_check_time >= 100) {
        // 这里可以添加具体的按键GPIO检测
        // 示例：检查某个GPIO引脚状态
        // u8 current_key_state = gpio_read(KEY_GPIO_PIN);
        
        last_key_check_time = current_time;
        
        // 如果需要检测GPIO按键，可以取消注释下面的代码
        /*
        if (current_key_state != last_key_state) {
            if (current_key_state == 0) { // 按键按下（假设低电平有效）
                log_i("=== GPIO KEY PRESSED ===");
                log_i("Key press detected at: %d ms", current_time);
                // GPIO按键按下时设置黄灯（所有模式）
                set_led_yellow();
            } else { // 按键释放
                log_i("=== GPIO KEY RELEASED ===");
                log_i("Key release detected at: %d ms", current_time);
            }
            last_key_state = current_key_state;
        }
        */
    }
}