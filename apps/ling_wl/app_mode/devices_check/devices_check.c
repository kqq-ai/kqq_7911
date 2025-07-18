/**
 * @author luotianyu
 * @brief 
 *   +----> IDLE
 *   |
 * POWER_ON -------> INIT -------> MODE_1 -------> MODE_2 -------> MODE_3 -------> MODE_4 -------> MODE_5 -------> MODE_6(串行状态) -------> MODE_7
 *   ^                                                                                                                        |
 *   |------------------------------------------------------------------------------------------------------------------------+
 * 
 * 模式说明：
 * INIT         初始模式（绿灯常亮）
 * MODE_1       触摸按键检测
 * MODE_2       所有颜色循环闪烁
 * MODE_3       循环播放音频（C端播放）
 * MODE_4       麦克风VAD检测
 * MODE_5       加速度传感器测试
 * MODE_6       串行状态机（E端设备音频循环播放，含多子状态）
 * MODE_7       预留，进入时播放test_bly.mp3
 * 
 * 按键短按依次切换模式，MODE_7后回到INIT，循环。
 * 
 * ── MODE_6 串行状态子状态机 ──
 * 
 *   +-------------------+
 *   |                   v
 * [IDLE] -> [PLAY_PROMPT] -> [CONNECT_1] -> [PLAY_1] -> [CONNECT_2] -> [PLAY_2]
 *    ^                                                      |
 *    +------------------------------------------------------+
 * 
 * 说明：
 *   - IDLE         ：空闲
 *   - PLAY_PROMPT  ：播放提示音
 *   - CONNECT_1    ：连接第1个E端设备
 *   - PLAY_1       ：第1设备连接成功后播放音频
 *   - CONNECT_2    ：连接第2个E端设备
 *   - PLAY_2       ：第2设备连接成功后播放音频
 *   - PLAY_2后回到CONNECT_1，循环
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
#include "cJSON_common/cJSON.h"
#include "bt_ble/app_wifi_manage.h"
#include "http/http_cli.h"

static void mode6_audio_callback(void *priv); // 声明提前到文件最前面

// 为模式3麦克风VAD功能添加必要的头文件
// 这些头文件可能需要根据实际的SDK结构进行调整
// #include "audio/recorder.h"     // 录音器相关头文件
// #include "audio/vad.h"          // VAD相关头文件

// 为模式4加速度传感器功能添加必要的头文件
#include "../../../common/gsensor/gSensor_manage.h"  // G-Sensor管理头文件
#include "../../SC7A20/sc7a20.h"           // SC7A20陀螺仪头文件
#include "../../SC7A20/app_sc7a20_task.h"  // SC7A20任务头文件

// 使用系统预定义的颜色常量
static void set_led_red(void)        { uart_send_set_led_color(COLOR_RED); }
static void set_led_green(void)      { uart_send_set_led_color(COLOR_GREEN); }
static void set_led_blue(void)       { uart_send_set_led_color(COLOR_BLUE); }
static void set_led_cyan(void)       { uart_send_set_led_color(COLOR_CYAN); }
static void set_led_white(void)      { uart_send_set_led_color(COLOR_WHITE); }
static void set_led_black(void)      { uart_send_set_led_color(COLOR_BLACK); }
static void set_led_orange(void)     { uart_send_set_led_color(COLOR_ORANGE); }
static void set_led_pink(void)       { uart_send_set_led_color(COLOR_PINK); }
static void set_led_purple(void)     { uart_send_set_led_color(COLOR_PURPLE); }
static void set_led_dark_yellow(void){ uart_send_set_led_color(COLOR_DARK_YELLOW); }
static void set_led_dark_blue(void)  { uart_send_set_led_color(COLOR_DARK_BLUE); }
static void set_led_maroon(void)     { uart_send_set_led_color(COLOR_MAROON); }
static void set_led_brown(void)      { uart_send_set_led_color(COLOR_BROWN); }
static void set_led_gray(void)       { uart_send_set_led_color(COLOR_GRAY); }

// 定义工作模式枚举
typedef enum {
    WORK_MODE_INIT = 0,     // 初始模式（绿灯常亮）
    WORK_MODE_1,            // 模式1（触摸按键检测）
    WORK_MODE_2,            // 模式2（所有颜色循环闪烁）
    WORK_MODE_3,            // 模式3（循环播放音频 - C端播放）
    WORK_MODE_4,            // 模式4（麦克风VAD检测）
    WORK_MODE_5,            // 模式5（加速度传感器测试）
    WORK_MODE_6,            // 模式6（循环播放音频 - E端播放）
    WORK_MODE_7,            // 模式7（预留，进入时播放test_bly.mp3）
    WORK_MODE_MAX           // 模式总数
} work_mode_e;

// 全局变量
 work_mode_e g_current_mode = WORK_MODE_INIT;
static u32 g_mode_timer = 0;
static u8 g_led_color_index = 0;
static u8 g_audio_playing = 0;
static u8 g_mode3_playing_prompt = 1; // 模式3状态：1=播放提示音，0=循环播放测试音
static u8 g_mode6_playing_prompt = 1; // 模式6状态：1=播放提示音，0=循环播放测试音
static u8 g_mode_switching = 0;      // 模式切换锁定标志，防止重复切换导致问题

// 模式1 触摸按键检测相关变量
static u8 g_mode1_touch_enabled = 0;    // 模式1 触摸检测是否已启用
static u32 g_mode1_touch_start_time = 0; // 触摸开始时间
static u8 g_mode1_touch_active = 0;     // 触摸是否正在激活 (0=绿灯, 1=红灯)

// 模式4 VAD相关变量
static int g_vad_state = 0;          // 当前VAD状态 (0=无语音, 1=有语音)
static int g_vad_state_prev = 0;     // 前一次VAD状态
static u32 g_vad_start_time = 0;     // 语音开始时间
static u32 g_vad_duration = 0;       // 语音持续时间
static u8 g_mode4_vad_enabled = 0;   // 模式4 VAD是否已启用

// 模式5 加速度传感器相关变量
extern void *g_iic_hdl;                            // 外部IIC句柄声明（在app_sc7a20_task.c中定义）
static void *g_sc7a20_iic_hdl = NULL;              // SC7A20 IIC设备句柄
static u8 g_mode5_gsensor_enabled = 0;             // 模式5 G-Sensor是否已启用
static s16 g_accel_x = 0, g_accel_y = 0, g_accel_z = 0;  // 三轴加速度值
static s16 g_accel_x_prev = 0, g_accel_y_prev = 0, g_accel_z_prev = 0;  // 前一次三轴加速度值
static u32 g_motion_count = 0;                 // 运动检测计数
static u8 g_motion_detected = 0;               // 运动检测状态 (0=静止, 1=小幅晃动, 2=大幅晃动)
static u32 g_small_motion_count = 0;           // 小幅晃动计数
static u32 g_large_motion_count = 0;           // 大幅晃动计数

// 模式6 E端设备相关变量
static int g_current_e_device_index = -1;      // 当前E端设备索引

// ========== 模式6 串行状态机重构 ===========
typedef enum {
    MODE6_IDLE = 0,        // 空闲状态，未进行任何操作
    MODE6_PLAY_PROMPT,     // 播放提示音（如开机提示、操作提示）
    MODE6_CONNECT_1,       // 尝试连接第一个目标设备
    MODE6_PLAY_1,          // 第一个设备连接成功后，进入播放状态
    MODE6_CONNECT_2,       // 尝试连接第二个目标设备
    MODE6_PLAY_2,          // 第二个设备连接成功后，进入播放状态
    // 移除DISCONNECT状态 // 该状态已废弃，不再使用
} mode6_state_e;

static mode6_state_e g_mode6_state = MODE6_IDLE;
static int g_mode6_e_index = 0; // 0:第1设备, 1:第2设备
static char *g_mode6_e_sn_list[2] = {"KPCNC5F10CEEEG5", "KPCNC5F10SEEEH1"};
// 标志“模式6”相关动作是否正在进行
// 1：有动作在执行（如连接、播放等），防止重复或并发操作
// 0：无动作，允许推进新流程
static u8 g_mode6_action_in_progress = 0;

// ========== 模式6回调函数声明 ===========
static void mode6_e_connect_cb(bka_cmd_code_e code, void *priv);
static void mode6_e_disconnect_cb(bka_cmd_code_e code, void *priv);
static void mode6_audio_callback(void *priv);

// 模式6推进函数
static void mode6_try_next_step(void)
{
    if (g_mode6_action_in_progress) return;
    g_mode6_action_in_progress = 1;
    lwl_app_context_t *context = lwl_app_core_context_get_instance();
    char *target_sn = g_mode6_e_sn_list[g_mode6_e_index];
    lwl_eend_dev_t *pdev = NULL;
    MapIterator it = map_begin(context->dev_map);
    MapIterator end = map_end(context->dev_map);
    for (; !map_iterator_equal(it, end); map_iterator_next(&it)) {
        lwl_eend_dev_t *tmp = (lwl_eend_dev_t *)map_iterator_value(it);
        if (strncmp(tmp->sn, target_sn, 15) == 0) {
            pdev = tmp;
            break;
        }
    }
    map_iterator_unlock(it);

    switch (g_mode6_state) {
    case MODE6_CONNECT_1:
    case MODE6_CONNECT_2:
        if (pdev) {
            pdev->handshake_info.need_handshake = true;
            uint cmd_id;
            //发送开机命令到E端设备
            bka_e_turn_on(pdev, mode6_e_connect_cb, pdev, &cmd_id);
        } else {
            // 设备找不到，重试
            g_mode6_action_in_progress = 0;
        }
        break;
    case MODE6_PLAY_1:
        gpio_direction_output(IO_PORTA_00, 0); // 关闭C端扬声器
        app_music_play_local_music_with_callback(
            context->music_player,
            CONFIG_VOICE_PROMPT_FILE_PATH"test_dev1.mp3",
            mode6_audio_callback, NULL);
        break;
    case MODE6_PLAY_2:
        gpio_direction_output(IO_PORTA_00, 0); // 关闭C端扬声器
        app_music_play_local_music_with_callback(
            context->music_player,
            CONFIG_VOICE_PROMPT_FILE_PATH"test_dev2.mp3",
            mode6_audio_callback, NULL);
        break;
    default:
        g_mode6_action_in_progress = 0;
        break;
    }
}

// 连接回调
static void mode6_e_connect_cb(bka_cmd_code_e code, void *priv)
{
    if (g_current_mode != WORK_MODE_6) return;
    int dev_idx = g_mode6_e_index;
    if ((g_mode6_state == MODE6_CONNECT_1 && dev_idx == 0) ||
        (g_mode6_state == MODE6_CONNECT_2 && dev_idx == 1)) {
        if (code == BKA_CMD_CODE_COMPL) {
            g_mode6_action_in_progress = 0;
            g_mode6_state = (dev_idx == 0) ? MODE6_PLAY_1 : MODE6_PLAY_2;
            mode6_try_next_step();
        } else {
            // 连接失败，重试
            g_mode6_action_in_progress = 0;
            mode6_try_next_step();
        }
    }
}

// 音频播放回调
static void mode6_audio_callback(void *priv)
{
    if (g_current_mode != WORK_MODE_6) return;
    if (g_mode6_state == MODE6_PLAY_PROMPT) {
        g_mode6_action_in_progress = 0;
        g_mode6_state = MODE6_CONNECT_1;
        mode6_try_next_step();
    } else if (g_mode6_state == MODE6_PLAY_1) {
        g_mode6_action_in_progress = 0;
        // 播放完第1设备，直接连接第2设备
        g_mode6_e_index = 1;
        g_mode6_state = MODE6_CONNECT_2;
        mode6_try_next_step();
    } else if (g_mode6_state == MODE6_PLAY_2) {
        g_mode6_action_in_progress = 0;
        // 播放完第2设备，直接连接第1设备
        g_mode6_e_index = 0;
        g_mode6_state = MODE6_CONNECT_1;
        mode6_try_next_step();
    }
}

// 模式切换函数声明
static void switch_to_mode(work_mode_e new_mode);
static void process_mode_init(void);
static void process_mode_1(void);
static void process_mode_2(void);
static void process_mode_3(void);
static void process_mode_4(void);
static void process_mode_5(void);
static void process_mode_6(void);
static void process_mode_7(void); // 新增模式7处理函数声明
static void devices_check_play_music(const char *fname);
static void mode1_audio_callback(void *priv);
static void mode3_audio_callback(void *priv);
static void mode6_audio_callback(void *priv); // <-- 提前声明
static void mode4_audio_callback(void *priv);
static void mode5_audio_callback(void *priv);
static void check_key_status(void);

// 模式3 VAD功能声明
static void mode4_vad_callback(void* priv, int type);
static int mode4_vad_init(void);
static void mode4_vad_uninit(void);
static void mode4_vad_status_update(void);

// 模式5 加速度传感器功能声明
static int mode5_gsensor_init(void);
static void mode5_gsensor_uninit(void);
static int mode5_gsensor_read_data(void);
static void mode5_gsensor_status_update(void);
static void mode5_motion_detection(void);

// 模式处理函数数组
static void (*mode_process_functions[])(void) = {
    process_mode_init,
    process_mode_1,
    process_mode_2,
    process_mode_3,
    process_mode_4,
    process_mode_5,
    process_mode_6,
    process_mode_7 // 新增模式7
};

// 模式名称数组（用于日志）
static const char *mode_names[] = {
    "INIT",
    "MODE_1_TOUCH_KEY",
    "MODE_2_ALL_COLORS",
    "MODE_3_AUDIO_LOOP_C",
    "MODE_4_MIC_VAD",
    "MODE_5_GSENSOR_TEST",
    "MODE_6_AUDIO_LOOP_E",
    "MODE_7_BLY" // 新增模式7名称
};

// 模式切换函数
static void switch_to_mode(work_mode_e new_mode)
{
    // 防止模式切换时的重复调用
    if (g_mode_switching) {
        log_i("Mode switching in progress, ignoring new request");
        return;
    }
    
    if (new_mode >= WORK_MODE_MAX) {
        log_i("Invalid mode: %d, staying in current mode", new_mode);
        return;
    }
    
    // 设置切换锁定
    g_mode_switching = 1;
    
    log_i("=== MODE SWITCH ===");
    log_i("From: %s (%d) -> To: %s (%d)", 
          mode_names[g_current_mode], g_current_mode,
          mode_names[new_mode], new_mode);
    
    // 退出当前模式时的清理操作
    if (g_current_mode == WORK_MODE_1 && g_mode1_touch_enabled) {
        log_i("Cleaning up MODE 1 touch key functionality");
        g_mode1_touch_enabled = 0;
        g_mode1_touch_active = 0;
    }
    if (g_current_mode == WORK_MODE_3) {
        log_i("Cleaning up MODE 3 audio functionality");
        g_audio_playing = 0;
        g_mode3_playing_prompt = 1;
        // 停止当前播放的音频
        lwl_app_context_t *context = lwl_app_core_context_get_instance();
        if (context && context->music_player) {
            // 这里可能需要调用停止音频的函数，但先尝试重置状态
            // app_music_stop(context->music_player);
        }
    }
    if (g_current_mode == WORK_MODE_4) {
        log_i("Cleaning up MODE 4 VAD functionality");
        // 无论VAD是否已启用，都要清理VAD功能
        mode4_vad_uninit();
    }
    if (g_current_mode == WORK_MODE_5 && g_mode5_gsensor_enabled) {
        log_i("Cleaning up MODE 5 G-Sensor functionality");
        mode5_gsensor_uninit();
    }
    if (g_current_mode == WORK_MODE_6) {
        log_i("Cleaning up MODE 6 audio functionality");
        g_audio_playing = 0;
        g_mode6_playing_prompt = 1;
        // 停止当前播放的音频
        lwl_app_context_t *context = lwl_app_core_context_get_instance();
        if (context && context->music_player) {
            // 这里可能需要调用停止音频的函数，但先尝试重置状态
            // app_music_stop(context->music_player);
        }
    }
    
    g_current_mode = new_mode;
    g_mode_timer = timer_get_ms();
    g_led_color_index = 0;
    
    // 模式切换时的初始化操作
    switch (new_mode) {
    case WORK_MODE_INIT:
        set_led_green();
        log_i("INIT MODE: Green LED ON");
        // 播放进入模式0的音频
        devices_check_play_music(CONFIG_VOICE_PROMPT_FILE_PATH"test_start.mp3");
        log_i("Playing test_start.mp3 for INIT mode");
        break;
    case WORK_MODE_1:
        set_led_green();
        g_audio_playing = 1;  // 设置为1，表示正在播放test_touchk.mp3
        g_mode1_touch_enabled = 0;  // 触摸检测尚未启用
        g_mode1_touch_active = 0;   // 触摸状态为非激活
        log_i("MODE 1: Touch key detection mode started");
        // 播放进入模式1的音频，使用回调
        lwl_app_context_t *context1 = lwl_app_core_context_get_instance();
        app_music_play_local_music_with_callback(
            context1->music_player,
            CONFIG_VOICE_PROMPT_FILE_PATH"test_touchk.mp3",
            mode1_audio_callback, NULL);
        log_i("Playing test_touchk.mp3 for MODE 1 with callback");
        break;
    case WORK_MODE_2:
        set_led_red();
        log_i("MODE 2: All colors cycling started (0.5s interval)");
        // 播放进入模式2的音频          LED
        devices_check_play_music(CONFIG_VOICE_PROMPT_FILE_PATH"test_led.mp3");
        log_i("Playing test_led.mp3 for MODE 2");
        break;
    case WORK_MODE_3:
        set_led_green();
        g_audio_playing = 1;  // 设置为1，表示正在播放loud_test.mp3
        g_mode3_playing_prompt = 1;  // 标记正在播放提示音
        log_i("MODE 3: Audio loop mode started");
        // 播放进入模式3的音频，使用回调    
        lwl_app_context_t *context3 = lwl_app_core_context_get_instance();
        app_music_play_local_music_with_callback(
            context3->music_player,
            CONFIG_VOICE_PROMPT_FILE_PATH"test_loud.mp3",
            mode3_audio_callback, NULL);
        log_i("Playing test_loud.mp3 for MODE 3 with callback");
        
        break;
    case WORK_MODE_4:

        gpio_direction_output(IO_PORTA_00, 1);  //Enable    C speaker
        set_led_green();  // 初始绿灯表示无语音
        g_audio_playing = 1;  // 设置为1，表示正在播放micro_test.mp3
        g_mode4_vad_enabled = 0;  // VAD尚未启用
        log_i("MODE 4: micro VAD detection mode started");
        // 播放进入模式4的音频，使用回调
        lwl_app_context_t *context4 = lwl_app_core_context_get_instance();
        app_music_play_local_music_with_callback(
            context4->music_player,
            CONFIG_VOICE_PROMPT_FILE_PATH"test_micro.mp3",
            mode4_audio_callback, NULL);
        log_i("Playing test_micro.mp3 for MODE 4 with callback");
        break;
    case WORK_MODE_5:
        gpio_direction_output(IO_PORTA_00, 1);  //Enable    C speaker
        set_led_green();  // 初始绿灯表示G-Sensor模式（静止状态）
        g_audio_playing = 1;  // 设置为1，表示正在播放SDcardMusic
        g_mode5_gsensor_enabled = 0;  // G-Sensor尚未启用
        g_motion_detected = 0;  // 重置运动检测状态
        g_motion_count = 0;     // 重置运动计数
        g_small_motion_count = 0; // 重置小幅晃动计数
        g_large_motion_count = 0; // 重置大幅晃动计数
        log_i("MODE 5: G-Sensor accelerometer test mode started");
        // 播放进入模式5的音频，使用回调
        lwl_app_context_t *context5 = lwl_app_core_context_get_instance();
        app_music_play_local_music_with_callback(
            context5->music_player,
            CONFIG_VOICE_PROMPT_FILE_PATH"test_gyro.mp3",  // 使用gyro_test音频文件
            mode5_audio_callback, NULL);
        log_i("Playing test_gyro.mp3 for MODE 5 with callback");
        break;
    case WORK_MODE_6:
        set_led_purple();
        gpio_direction_output(IO_PORTA_00, 1);  // 启用C端扬声器
        g_mode6_state = MODE6_PLAY_PROMPT;
        g_mode6_e_index = 0;
        g_mode6_action_in_progress = 0;
        {
            lwl_app_context_t *context6 = lwl_app_core_context_get_instance();
            app_music_play_local_music_with_callback(
                context6->music_player,
                CONFIG_VOICE_PROMPT_FILE_PATH"test_edev.mp3",
                mode6_audio_callback, NULL);
            g_mode6_action_in_progress = 1;
        }
        log_i("MODE 6: 串行状态机启动");
        break;
    case WORK_MODE_7:
        set_led_blue(); // 进入模式7时点亮蓝灯
        log_i("MODE 7: 进入模式7，播放test_bly.mp3");
        {
            lwl_app_context_t *context7 = lwl_app_core_context_get_instance();
            app_music_play_local_music_with_callback(
                context7->music_player,
                CONFIG_VOICE_PROMPT_FILE_PATH"test_bly.mp3",
                NULL, NULL);
        }
        // 具体功能待定
        break;
    default:
        break;
    }
    
    // 解除切换锁定
    g_mode_switching = 0;
    log_i("=== MODE SWITCH COMPLETED ===");
}

// 初始模式处理函数
static void process_mode_init(void)
{
    // 初始模式只需要保持红灯常亮，等待按键切换
    // 无需特殊处理
}

// 模式1处理函数：触摸按键检测
static void process_mode_1(void)
{
    u32 current_time = timer_get_ms();
    
    // 如果触摸功能已启用，检查红灯持续时间
    if (g_mode1_touch_enabled && g_mode1_touch_active) {
        // 检查红灯是否已持续0.8秒(800ms)
        if (current_time - g_mode1_touch_start_time >= 800) {
            // 恢复绿灯
            set_led_green();
            g_mode1_touch_active = 0;
            log_i("MODE 1: Touch timeout, LED restored to GREEN");
        }
    }
    
    // 如果提示音还在播放中，则等待播放完成
    if (g_audio_playing) {
        // 等待音频播放完成，无需特殊处理
        return;
    }
}

// 模式1音频播放回调函数
static void mode1_audio_callback(void *priv)
{
    log_i("MODE 1: Prompt audio callback - finished, starting touch key monitoring");
    g_audio_playing = 0;
    
    // 提示音播放完成，启用触摸按键检测功能
    g_mode1_touch_enabled = 1;
    g_mode1_touch_active = 0;
    set_led_green();  // 确保LED为绿色
    log_i("MODE 1: Touch key monitoring enabled successfully");
}

// 模式2处理函数：所有颜色循环闪烁
static void process_mode_2(void)
{
    u32 current_time = timer_get_ms();
    
    // 每0.5秒(500ms)切换一次LED颜色
    if (current_time - g_mode_timer >= 500) {
        // 使用系统预定义的颜色设置函数数组 - 准确颜色优先
        void (*all_color_functions[])(void) = {
            set_led_red,           // 红色 
            set_led_green,         // 绿色 
            set_led_blue,          // 蓝色 
            set_led_purple,        // 紫色 
            set_led_white,         // 白色 = 红+绿+蓝
            set_led_orange,        // 橙色 = 红+少绿 (调整更偏红)
            
        };
        
        // 对应的颜色名称
        const char *color_names[] = {
            "RED", "GREEN", "BLUE", "PURPLE", "WHITE", "ORANGE"
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

// 模式3处理函数：先播放提示音，然后循环播放测试音频
static void process_mode_3(void)
{
    lwl_app_context_t *context = lwl_app_core_context_get_instance();
    static u32 last_play_time = 0;
    u32 current_time = timer_get_ms();
    
    // 检查context有效性
    if (!context || !context->music_player) {
        log_e("MODE 3: Context or music_player is NULL");
        return;
    }
    
    // 如果不在播放提示音阶段，且当前没有音频在播放，则开始播放循环音频
    if (!g_mode3_playing_prompt && !g_audio_playing) {
        // 防止频繁重新播放，至少间隔100ms
        if (current_time - last_play_time >= 5000) {
            log_i("MODE 3: Starting loop audio playback");
            
            // 播放循环测试音频，使用回调
            int err = app_music_play_local_music_with_callback(
                context->music_player, 
                CONFIG_VOICE_PROMPT_FILE_PATH"test_1hz5s.mp3", 
                mode3_audio_callback, NULL);
                
            if (err == 0) {
                g_audio_playing = 1;
                last_play_time = current_time;
                log_i("MODE 3: Loop audio started successfully");
            } else {
                log_e("MODE 3: Loop audio start failed, error: %d", err);
            }
        }
    }
}

// 模式3音频播放回调函数 - C端播放
static void mode3_audio_callback(void *priv)
{
    log_i("MODE 3: C端播放音频");
    
    // 启用C端扬声器，确保音频在本地播放
    gpio_direction_output(IO_PORTA_00, 1);  // Enable C speaker
    
    if (g_mode3_playing_prompt) {
        // 提示音播放完成，开始播放循环音频
        log_i("MODE 3: Prompt audio callback - finished, starting loop audio");
        g_mode3_playing_prompt = 0;
        g_audio_playing = 0;
    } else {
        // 循环音频播放完成，准备重新播放
        log_i("MODE 3: Loop audio callback - finished, will restart");
        g_audio_playing = 0;
    }
}



// 模式4音频播放回调函数
static void mode4_audio_callback(void *priv)
{
    log_i("MODE 4: Prompt audio callback - finished, starting VAD monitoring");
    g_audio_playing = 0;
    
    // 提示音播放完成，启动VAD功能
    if (mode4_vad_init() == 0) {
        g_mode4_vad_enabled = 1;
        log_i("MODE 4: VAD monitoring enabled successfully");
    } else {
        log_e("MODE 4: Failed to enable VAD monitoring");
    }
}

//=================================================================================================
// 模式4 VAD功能相关函数
//=================================================================================================
/**
 * @brief VAD回调函数 - 处理语音活动检测事件
 * @param priv 私有数据指针
 * @param type VAD事件类型 (1=语音开始, 0=语音结束)
 */
static void mode4_vad_callback(void* priv, int type)
{
    // 检查当前模式，如果不是模式4，则忽略VAD事件
    if (g_current_mode != WORK_MODE_4) {
        log_d("[MODE 4 VAD] Ignoring VAD event - not in mode 4 (current mode: %d)", g_current_mode);
        return;
    }
    
    // 检查VAD是否已启用
    if (!g_mode4_vad_enabled) {
        log_d("[MODE 4 VAD] Ignoring VAD event - VAD not enabled");
        return;
    }
    
    u32 current_time = timer_get_ms();
    
    if (type == 1) {
        // 语音开始
        g_vad_state = 1;
        g_vad_start_time = current_time;
        log_i("[MODE 4 VAD] Voice activity START at %d ms", current_time);
        
        // 开始说话时打印10行分隔符
        for (int i = 0; i < 10; i++) {
            printf("---------------------------\r\n");
        }
        
        // 检测到语音时设置红灯
        set_led_red();
        log_i("MODE 4: Voice detected - LED set to RED");
        
    } else {
        // 语音结束
        g_vad_state = 0;
        g_vad_duration = current_time - g_vad_start_time;
        log_i("[MODE 4 VAD] Voice activity END at %d ms, duration: %d ms", current_time, g_vad_duration);
        
        // 结束说话时打印10行分隔符
        for (int i = 0; i < 10; i++) {
            printf("|||||||||||||||||||||||||||||||\r\n");
        }
        
        // 语音结束时恢复绿灯
        set_led_green();
        log_i("MODE 4: Voice ended - LED set to GREEN");
        
        // 如果语音持续时间超过1秒，可以触发某些动作
        if (g_vad_duration > 1000) {
            log_i("[MODE 4 VAD] Long voice detected (%d ms), triggering action...", g_vad_duration);
        }
    }
}

/**
 * @brief 启动VAD功能
 * @return 0成功, -1失败
 */
static int mode4_vad_init(void)
{
    lwl_app_context_t *context = lwl_app_core_context_get_instance();
    
    if (!context || !context->recorder) {
        log_e("[MODE 4 VAD] Recorder not available");
        return -1;
    }
    
    // 设置VAD回调函数
    int ret = recorder_set_vad_cbk(context->recorder, mode4_vad_callback, NULL);
    if (ret != 0) {
        log_e("[MODE 4 VAD] Failed to set VAD callback: %d", ret);
        return -1;
    }
    
    // 启动录音器以启用VAD功能
    ret = recorder_start(context->recorder);
    if (ret != 0) {
        log_e("[MODE 4 VAD] Failed to start recorder: %d", ret);
        return -1;
    }
    
    log_i("[MODE 4 VAD] VAD functionality initialized and started");
    return 0;
}

/**
 * @brief 停止VAD功能
 */
static void mode4_vad_uninit(void)
{
    // 如果VAD功能未启用，直接返回
    if (!g_mode4_vad_enabled) {
        log_i("[MODE 4 VAD] VAD functionality already stopped");
        return;
    }
    
    lwl_app_context_t *context = lwl_app_core_context_get_instance();
    
    if (context && context->recorder) {
        // 停止录音器
        recorder_stop(context->recorder);
        // 清除VAD回调
        recorder_set_vad_cbk(context->recorder, NULL, NULL);
        log_i("[MODE 4 VAD] VAD functionality stopped - recorder stopped and callback cleared");
    } else {
        log_w("[MODE 4 VAD] Context or recorder is NULL during cleanup");
    }
    
    // 重置所有VAD相关的状态变量
    g_mode4_vad_enabled = 0;
    g_vad_state = 0;
    g_vad_state_prev = 0;
    g_vad_start_time = 0;
    g_vad_duration = 0;
    
    log_i("[MODE 4 VAD] All VAD state variables reset");
}

/**
 * @brief 更新VAD状态信息并打印
 */
static void mode4_vad_status_update(void)
{
    static u32 last_print_time = 0;
    static u32 voice_detect_count = 0;  // 语音检测次数统计
    static u32 total_voice_time = 0;    // 总语音时间统计
    u32 current_time = timer_get_ms();
    
    // 检测VAD状态变化
    if (g_vad_state != g_vad_state_prev) {
        if (g_vad_state == 1) {
            voice_detect_count++;
            log_i("[MODE 4 VAD] === VOICE DETECTED === (#%d)", voice_detect_count);
            printf("[VAD] Voice start detected at %d ms\r\n", current_time);
        } else {
            total_voice_time += g_vad_duration;
            log_i("[MODE 4 VAD] === VOICE STOPPED === (Duration: %d ms, Total: %d ms)", 
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
            log_i("[MODE 4 VAD] Status: SPEAKING (Duration: %d ms)", speaking_duration);
            printf("[VAD] Currently speaking: %d ms\r\n", speaking_duration);
        } else {
            log_i("[MODE 4 VAD] Status: SILENT (Count: %d, Total voice time: %d ms)", 
                  voice_detect_count, total_voice_time);
            printf("[VAD] Silent mode - Voice events: %d, Total time: %d ms\r\n", 
                   voice_detect_count, total_voice_time);
        }
        last_print_time = current_time;
    }
}

// 模式4处理函数：麦克风VAD检测模式
static void process_mode_4(void)
{
    // 如果VAD功能已启用，则监控VAD状态
    if (g_mode4_vad_enabled) {
        mode4_vad_status_update();
    }
    
    // 如果提示音还在播放中，则等待播放完成
    if (g_audio_playing) {
        // 等待音频播放完成，无需特殊处理
        return;
    }
}

// 模式5处理函数：加速度传感器测试模式
static void process_mode_5(void)
{
    // 如果G-Sensor功能已启用，则监控传感器状态
    if (g_mode5_gsensor_enabled) {
        mode5_gsensor_status_update();
        mode5_motion_detection();
    }
    
    // 如果提示音还在播放中，则等待播放完成
    if (g_audio_playing) {
        // 等待音频播放完成，无需特殊处理
        return;
    }
}

// 模式6处理函数
static void process_mode_6(void)
{
    // 只需定期推进状态机
    mode6_try_next_step();
}

// 新增模式7处理函数
static void process_mode_7(void)
{
    static u8 has_run = 0;
    if (has_run) return; // Only run once
    has_run = 1;

    set_led_green();

    log_i("================================1Entering mode 7================================");
    log_i("================================2Entering mode 7================================");

    // Check WIFI connection status
    WIFI_connect_STATUES wifi_status = get_wifi_connect_status(NULL);
    log_i("==============================[MODE7] wifi_status: %d================================", wifi_status);
    if (wifi_status != WIFI_CONNECT_SUCC && wifi_status != WIFI_CONNECT_DHCP_SUCC) {
        log_i("===============:WIFI disconnected===============");
        log_i("[MODE7] WIFI not connected, cannot request SN, process terminated");
        return;
    }
    log_i("========================================== [MODE7] WIFI connected================================");

    // Build POST request to https://test.coolqq.com/api/sn
    const char *url = "https://test.coolqq.com/api/sn";
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "C");
    char *post_data = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    log_i("==========================================[MODE7]POST:%s================================", post_data);

    http_body_obj http_body_buf;
    httpcli_ctx ctx;
    memset(&ctx, 0, sizeof(ctx));
    memset(&http_body_buf, 0, sizeof(http_body_buf));

    http_body_buf.recv_len = 0;
    http_body_buf.buf_len = 4096;
    http_body_buf.buf_count = 1;
    http_body_buf.p = (char *)malloc(http_body_buf.buf_len * http_body_buf.buf_count);
    if (!http_body_buf.p) {
        log_i("===========================================");
        log_i("[MODE7] Memory allocation failed, process terminated");
        free(post_data);
        return;
    }

    ctx.url = url;
    ctx.timeout_millsec = 5000;
    ctx.priv = &http_body_buf;
    ctx.connection = "close";
    ctx.data_format = "application/json"; // Set Content-Type
    ctx.post_data = post_data;
    ctx.data_len = strlen(post_data);

    log_i("========================Sending httpcli_post...=============");
    int ret = httpcli_post(&ctx);
    log_i("=======================[MODE7] httpcli_post return: %d, length: %d===========", ret, http_body_buf.recv_len);
    free(post_data);

    if (ret != HERROR_OK || http_body_buf.recv_len <= 0) {
        log_i("==========================================MODE 7: HTTP request failed================================");
        log_i("==========================================[MODE7] HTTP request failed, process terminated================================");
        if (http_body_buf.p) free(http_body_buf.p);
        return;
    }

    log_i("==========================================[MODE7] HTTP response: %s================================", http_body_buf.p);
    // Parse JSON response
    const cJSON *resp = cJSON_Parse(http_body_buf.p);
    if (!resp) {
        log_i("==========================================MODE 7: Response is not valid JSON================================");
        log_i("==========================================[MODE7] Response is not valid JSON, process terminated================================");
        free(http_body_buf.p);
        return;
    }
    const cJSON *data = cJSON_GetObjectItem(resp, "data");
    const cJSON *sn = data ? cJSON_GetObjectItem(data, "sn") : NULL;
    const cJSON *key = data ? cJSON_GetObjectItem(data, "key") : NULL;
    if (sn && key && cJSON_IsString(sn) && cJSON_IsString(key)) {
        log_i("===============================[MODE7] SN: %s=======================", sn->valuestring);
        log_i("===============================[MODE7] Key: %s=======================", key->valuestring);
        set_led_red();
        log_i("========================[MODE7] SN/Key obtained successfully, red LED is ON=================");
    } else {
        log_i("===============================[MODE7] Failed to get SN/Key, process terminated==========");
        log_i("=============================MODE 7: SN/Key fetch failed================================");
    }
    cJSON_Delete(resp);
    free(http_body_buf.p);
}

// 模式5音频播放回调函数
static void mode5_audio_callback(void *priv)
{
    log_i("MODE 5: Prompt audio callback - finished, starting G-Sensor monitoring");
    g_audio_playing = 0;
    
    // 提示音播放完成，启动G-Sensor功能
    if (mode5_gsensor_init() == 0) {
        g_mode5_gsensor_enabled = 1;
        log_i("MODE 5: G-Sensor monitoring enabled successfully");
    } else {
        log_e("MODE 5: Failed to enable G-Sensor monitoring");
    }
}

//=================================================================================================
// 模式5 G-Sensor功能相关函数
//=================================================================================================
/**
 * @brief 启动G-Sensor功能
 * @return 0成功, -1失败
 */
static int mode5_gsensor_init(void)
{
    // 获取外部已初始化的SC7A20 IIC句柄
    if (!g_iic_hdl) {
        log_e("[MODE 5 GSENSOR] SC7A20 IIC handle not initialized");
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
    
    log_i("[MODE 5 GSENSOR] SC7A20 G-Sensor functionality initialized and started");
    return 0;
}

/**
 * @brief 停止G-Sensor功能
 */
static void mode5_gsensor_uninit(void)
{
    if (g_sc7a20_iic_hdl) {
        g_sc7a20_iic_hdl = NULL;
        log_i("[MODE 5 GSENSOR] SC7A20 G-Sensor functionality stopped");
    }
    
    g_mode5_gsensor_enabled = 0;
    g_motion_detected = 0;
    g_motion_count = 0;
    g_small_motion_count = 0;
    g_large_motion_count = 0;
}

/**
 * @brief 读取G-Sensor数据
 * @return 0成功, -1失败
 */
static int mode5_gsensor_read_data(void)
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
static void mode5_motion_detection(void)
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
            log_i("[MODE 5 GSENSOR] === LARGE MOTION DETECTED === (#%d total, #%d large)", 
                  g_motion_count, g_large_motion_count);
            log_i("[MODE 5 GSENSOR] Delta: X=%d, Y=%d, Z=%d, Total=%d (LARGE)", 
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
            log_i("[MODE 5 GSENSOR] === SMALL MOTION DETECTED === (#%d total, #%d small)", 
                  g_motion_count, g_small_motion_count);
            log_i("[MODE 5 GSENSOR] Delta: X=%d, Y=%d, Z=%d, Total=%d (SMALL)", 
                  delta_x, delta_y, delta_z, total_delta);
            
            // 打印小幅运动开始的分隔符
            for (int i = 0; i < 3; i++) {
                printf(">>> SMALL MOTION START <<<\r\n");
            }
        } else {
            // 运动停止
            set_led_green();  // 静止时绿灯
            if (g_motion_detected == 2) {
                log_i("[MODE 5 GSENSOR] === LARGE MOTION STOPPED ===");
                // 打印大幅运动结束的分隔符
                for (int i = 0; i < 5; i++) {
                    printf(">>> LARGE MOTION STOP <<<\r\n");
                }
            } else if (g_motion_detected == 1) {
                log_i("[MODE 5 GSENSOR] === SMALL MOTION STOPPED ===");
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
static void mode5_gsensor_status_update(void)
{
    static u32 last_print_time = 0;
    static u32 last_data_read_time = 0;
    static u32 read_error_count = 0;
    u32 current_time = timer_get_ms();
    
    // 每50ms读取一次传感器数据
    if (current_time - last_data_read_time >= 50) {
        if (mode5_gsensor_read_data() != 0) {
            read_error_count++;
            // 如果连续读取失败次数过多，打印错误信息
            if (read_error_count % 20 == 1) {  // 每20次失败打印一次错误
                log_e("[MODE 5 GSENSOR] Failed to read sensor data (error count: %d)", read_error_count);
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
        
        log_i("[MODE 5 GSENSOR] Gyroscope Status: X=%d, Y=%d, Z=%d", g_accel_x, g_accel_y, g_accel_z);
        log_i("[MODE 5 GSENSOR] Motion: %s, Total: %d, Small: %d, Large: %d, Errors: %d", 
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
    
    // 检查context有效性
    if (!context || !context->music_player) {
        log_e("devices_check_play_music: Context or music_player is NULL");
        return;
    }
    
    // 检查文件名有效性
    if (!fname) {
        log_e("devices_check_play_music: Invalid filename");
        return;
    }
    
    // g_music_player.play_voice_prompt = 1;
    log_i("play: %s", fname);
    int err = app_music_play_local_music_with_callback(context->music_player, fname, NULL, NULL);
    if (err) {
        log_e("devices_check_play_music: Failed to play %s, error: %d", fname, err);
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
        ///////////////////////////////////////////
        //devices_check_play_music(CONFIG_VOICE_PROMPT_FILE_PATH"NetCfgSucc.mp3");
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
            next_mode = WORK_MODE_5;
            break;
        case WORK_MODE_5:
            next_mode = WORK_MODE_6;
            break;
        case WORK_MODE_6:
            next_mode = WORK_MODE_7;
            break;
        case WORK_MODE_7:
        default:
            next_mode = WORK_MODE_INIT; // 模式7后回到初始模式，形成循环
            break;
        }
        
        switch_to_mode(next_mode);

    }   break;
    case LWL_EVENT_TYPE_UART_SYNC:
        log_i("SYNC");
        log_i("battery: %d", e->uart_cmd_sync.battery);
        // if(e->uart_cmd_sync.battery < 30 )
        //     set_led_red(); //red~ low power
        uart_response_synch_status(1, 12);/// 音量  e的声音 only
        break;
    case LWL_EVENT_TYPE_UART_TOUCH_KEY_DOWN:
        log_i("=== UART TOUCH KEY DOWN DETECTED ===");
        log_i("Touch key pressed down");
        log_i("Event timestamp: %d ms", timer_get_ms());
        log_i("Current mode: %s (%d)", mode_names[g_current_mode], g_current_mode);
        
        // 特殊处理模式1的触摸按键检测
        if (g_current_mode == WORK_MODE_1 && g_mode1_touch_enabled) {
            // 模式1触摸按键检测：设置红灯持续0.8秒
            set_led_red();
            g_mode1_touch_active = 1;
            g_mode1_touch_start_time = timer_get_ms();
            log_i("MODE 1: Touch key detected - LED set to RED for 0.8s");
        } else {
            // 其他模式的触摸按键检测：设置黄灯（原有逻辑）
            log_i("Touch key detected, LED set to YELLOW temporarily");
        }
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
    log_i("INIT -> MODE_1 (Touch Key Detection) -> MODE_2 (All Colors Cycling) -> MODE_3 (Audio Loop) -> MODE_4 (Microphone VAD) -> MODE_5 (G-Sensor Test) -> INIT (Loop)");
    log_i("Available colors (16 total): Red*, Green*, Blue*, Purple*, Cyan, Yellow, Magenta, White, Orange, Pink, Dark_Yellow, Dark_Blue, Maroon, Brown, Gray, Black");
    log_i("Color basis: Red/Green/Blue/Purple are accurate (* = accurate), others adjusted");
    log_i("MODE 1: Green LED = Ready, Red LED = Touch detected (0.8s duration)");
    log_i("MODE 4: Red LED = Voice detected, Green LED = Silent");
    log_i("MODE 5: Green LED = Static, Blue LED = Small motion, Red LED = Large motion, Gyroscope data every 0.5s");
    log_i("Touch Key: MODE 1 = Red LED (0.8s), Other modes = Yellow LED (temporary)");
     
    return 0;
}

static int devices_check_unint(void)
{
    // 如果当前在模式1且触摸检测已启用，则清理触摸检测功能
    if (g_current_mode == WORK_MODE_1 && g_mode1_touch_enabled) {
        g_mode1_touch_enabled = 0;
        g_mode1_touch_active = 0;
    }
    // 如果当前在模式4且VAD已启用，则清理VAD功能
    if (g_current_mode == WORK_MODE_4 && g_mode4_vad_enabled) {
        mode4_vad_uninit();
    }
    // 如果当前在模式5且G-Sensor已启用，则清理G-Sensor功能
    if (g_current_mode == WORK_MODE_5 && g_mode5_gsensor_enabled) {
        mode5_gsensor_uninit();
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
        last_key_check_time = current_time;
    }
}