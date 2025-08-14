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
 * MODE_7       获取SN号，进入时播放test_snkey.mp3
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

// 为模式7 SN/Key写入功能添加必要的头文件
#include "../../../common/example/third_party/littlefs/lfs.h"  // LittleFS文件系统

// 外部声明全局LittleFS实例
extern lfs_t g_lfs;

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
    WORK_MODE_7,            // 模式7（获取SN号，进入时播放test_snkey.mp3）
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
static u32 g_mode1_touch_start_time = 0; // 触摸开始时间（保留兼容性）
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
lwl_eend_dev_t *target_pdev = NULL; // 当前目标E端设备指针
lwl_eend_dev_t *current_pdev = NULL; // 当前连接的E端设备指针
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
// 标志“模式6”相关动作是否正在进行
// 1：有动作在执行（如连接、播放等），防止重复或并发操作
// 0：无动作，允许推进新流程
static u8 g_mode6_action_in_progress = 0;

// ========== 模式6回调函数声明 ===========
static void mode6_audio_callback(void *priv);
int devices_check_e_turn_on(lwl_eend_dev_t *p_eend_dev, bka_cmd_cb_funp_t cb, void *priv, uint32_t *cmd_id_out);
static void devices_check_cb(bka_cmd_code_e code, void *priv);

#define MAX_PDEV_LIST 16
static lwl_eend_dev_t* pdev_list[MAX_PDEV_LIST] = {0};
static int pdev_list_count = 0;

// 判断pdev是否已在列表中
static int is_pdev_in_list(lwl_eend_dev_t* pdev) {
    for (int i = 0; i < pdev_list_count; ++i) {
        if (pdev_list[i] == pdev) return 1;
    }
    return 0;
}

// 添加pdev到列表
static void add_pdev_to_list(lwl_eend_dev_t* pdev) {
    if (!pdev || is_pdev_in_list(pdev) || pdev_list_count >= MAX_PDEV_LIST) return;
    pdev_list[pdev_list_count++] = pdev;
    char sn_string[SN_SIZE] = "(NULL)";
    pdev->get_string(pdev, sn_string, sizeof(sn_string));
    log_i("[PDEV_LIST] Added: %s, total: %d", sn_string, pdev_list_count);
    
    uint32_t cmd_id;
    int ret = devices_check_e_turn_on(pdev, devices_check_cb, pdev, &cmd_id);
    log_i("turn on cmd_id: %d", ret);
}

// 从列表中移除pdev
static void remove_pdev_from_list(lwl_eend_dev_t* pdev) {
    for (int i = 0; i < pdev_list_count; ++i) {
        if (pdev_list[i] == pdev) {
            for (int j = i; j < pdev_list_count - 1; ++j) {
                pdev_list[j] = pdev_list[j + 1];
            }
            pdev_list[--pdev_list_count] = NULL;
            char sn_string[SN_SIZE] = "(NULL)";
            pdev->get_string(pdev, sn_string, sizeof(sn_string));
            log_i("[PDEV_LIST] Removed: %s, total: %d", sn_string, pdev_list_count);
            break;
        }
    }
}

static void mode6_e_connect_cb(bka_cmd_code_e code, void *priv)
{
    lwl_eend_dev_t *pdev = (lwl_eend_dev_t*)(priv);
    char sn_string[SN_SIZE] = "(NULL)";
    if (pdev) {
        pdev->get_string(pdev, sn_string, sizeof(sn_string));
    }
    log_i("bka_e_turn_on code %d %s\n", code, sn_string);

    if (BKA_CMD_CODE_COMPL == code) {
        current_pdev = pdev; // 设置当前连接的E端设备指针
    } else {
        if (pdev == target_pdev) target_pdev = NULL; // 连接失败，清空当前设备指针
        uint cmd_id;
    }
}
static void switch_to_e_target(lwl_eend_dev_t* pdev)
{
    if (pdev == NULL) return;
    char sn_string[SN_SIZE] = "(NULL)";
    pdev->get_string(pdev, sn_string, sizeof(sn_string));
    log_i("Switching to target: %s", sn_string);
    
    // 设置新的目标设备
    target_pdev = pdev;
    uint cmd_id;
    bka_e_turn_on(pdev, mode6_e_connect_cb, pdev, &cmd_id); // E ON~  start_handshake
    log_i("+-- -+");
    // 参考default_e_turn_on函数中的命令发送方式
}

//TODO test
static void turn_off_e_target(char *target)
{
    if (target == NULL) return;
    log_i("turn off EEEEEEEEEEEE target: %s", target);
    lwl_eend_dev_t *pdev = find_eend_dev_by_sn(target);  // 查找目标设备
    if (pdev == NULL) return;
    
    uint cmd_id;
    bka_e_turn_off(pdev, NULL, pdev, &cmd_id); // E OFF
    log_i("+-- -+");
    // 参考default_e_turn_on函数中的命令发送方式
}

// 模式6推进函数
static void mode6_try_next_step(void)
{
    lwl_app_context_t *context = lwl_app_core_context_get_instance();
    
    // 如果不在播放提示音阶段，且当前没有音频在播放，则开始播放循环音频
    if (!g_mode6_playing_prompt && !g_audio_playing && (target_pdev == current_pdev || target_pdev == NULL)) {
        log_i("MODE 6: Starting loop audio playback");
        
        // 播放循环测试音频，使用回调
        int err = 0;
        if((g_mode6_e_index&1) == 0 ){
            err = app_music_play_local_music_with_callback(
            context->music_player, 
            CONFIG_VOICE_PROMPT_FILE_PATH"test_dev1.mp3", 
            mode6_audio_callback, NULL);
        }else{
            err = app_music_play_local_music_with_callback(
            context->music_player, 
            CONFIG_VOICE_PROMPT_FILE_PATH"test_dev2.mp3", 
            mode6_audio_callback, NULL);
        }
        
            
        if (err == 0) {
            g_audio_playing = 1;
            log_i("MODE 6: Loop audio started successfully");
        } else {
            log_i("MODE 6: Loop audio start failed, error: %d", err);
        }
    }
}

// 音频播放回调
static void mode6_audio_callback(void *priv)
{
    // 检查当前模式，如果不是模式6，则忽略回调
    if (g_current_mode != WORK_MODE_6) {
        log_i("MODE 6: Audio callback ignored - not in mode 6 (current mode: %d)", g_current_mode);
        return;
    }
    
    gpio_direction_output(IO_PORTA_00, 0);  //disable    C speaker

    // 播放结束后，先关闭当前已连接的E端设备（如果有）
    if (current_pdev) {
        char sn_string[SN_SIZE] = "(NULL)";
        current_pdev->get_string(current_pdev, sn_string, sizeof(sn_string));
        log_i("MODE 6: Turn OFF current E device: %s", sn_string);
        uint cmd_id;
        bka_e_turn_off(current_pdev, NULL, current_pdev, &cmd_id);
        current_pdev = NULL; // 清空当前连接设备指针
    }

    g_mode6_e_index++;
    if (g_mode6_e_index >= pdev_list_count) g_mode6_e_index = 0;
    if (pdev_list_count > 0) {
    switch_to_e_target(pdev_list[g_mode6_e_index]);
    } else {
        log_i("MODE 6: pdev_list empty, skip switching target");
    }
    if (g_mode6_playing_prompt) {
        // 提示音播放完成，开始播放循环音频
        log_i("MODE 6: Prompt audio callback - finished, starting loop audio");
        g_mode6_playing_prompt = 0;
        g_audio_playing = 0;
    } else {
        // 循环音频播放完成，准备重新播放
        log_i("MODE 6: Loop audio callback - finished, will restart");
        g_audio_playing = 0;
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
static void mode7_audio_callback(void *priv); // 新增模式7音频回调声明
static void mode7_success_audio_callback(void *priv); // 新增模式7成功音频回调声明
static void mode7_wrsn_audio_callback(void *priv); // 新增模式7 WRSN音频回调声明
static void mode7_execute_sn_request(void); // 新增模式7执行SN请求函数声明
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

// 模式7功能声明
static void mode7_reset_network_check(void);
static int mode7_write_sn_key_to_storage(const char* sn, const char* key); // 新增模式7写入SN/Key函数声明
static int mode7_verify_written_sn_key(const char* expected_sn, const char* expected_key); // 新增模式7验证SN/Key函数声明
static int mode7_read_sn_key_from_storage(char* sn_out, int sn_size, char* key_out, int key_size); // 新增模式7读取SN/Key函数声明
static int mode7_write_and_verify_sn_key(const char* sn, const char* key); // 新增模式7写入并验证SN/Key综合函数声明

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
    "MODE_7_SNKEY" // 模式7名称：SN号获取
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
        // 确保退出模式1时LED状态正确
        if (new_mode != WORK_MODE_1) {
            // 如果不是保持在模式1，则可能需要重置LED（由新模式初始化处理）
        }
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
        g_mode6_action_in_progress = 0;  // 重置模式6动作进行标志
        g_mode6_state = MODE6_IDLE;      // 重置模式6状态机
        target_pdev = NULL;              // 清空目标设备指针
        current_pdev = NULL;             // 清空当前设备指针
        // 停止当前播放的音频
        lwl_app_context_t *context = lwl_app_core_context_get_instance();
        if (context && context->music_player) {
            // 这里可能需要调用停止音频的函数，但先尝试重置状态
            // app_music_stop(context->music_player);
        }
        log_i("MODE 6: All state variables reset");
    }
    
    // 重置模式7状态（无论从哪个模式切换，都重置模式7的SN号获取状态）
    mode7_reset_network_check();
    
    g_current_mode = new_mode;
    g_mode_timer = timer_get_ms();
    g_led_color_index = 0;
    
    // 模式切换时的初始化操作
    switch (new_mode) {
    case WORK_MODE_INIT:
        set_led_green();
        gpio_direction_output(IO_PORTA_00, 1);  // 启用C端扬声器
        log_i("INIT MODE: Green LED ON, C speaker enabled");
        // 播放进入模式0的音频
        devices_check_play_music(CONFIG_VOICE_PROMPT_FILE_PATH"test_start.mp3");
        log_i("Playing test_start.mp3 for INIT mode");
        break;
    case WORK_MODE_1:
        set_led_green();
        gpio_direction_output(IO_PORTA_00, 1);  // 启用C端扬声器
        g_audio_playing = 1;  // 设置为1，表示正在播放test_touchk.mp3
        g_mode1_touch_enabled = 0;  // 触摸检测尚未启用
        g_mode1_touch_active = 0;   // 触摸状态为非激活
        log_i("MODE 1: Touch key detection mode started, C speaker enabled");
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
        gpio_direction_output(IO_PORTA_00, 1);  // 启用C端扬声器
        log_i("MODE 2: All colors cycling started (0.5s interval), C speaker enabled");
        // 播放进入模式2的音频          LED
        devices_check_play_music(CONFIG_VOICE_PROMPT_FILE_PATH"test_led.mp3");
        log_i("Playing test_led.mp3 for MODE 2");
        break;
    case WORK_MODE_3:
        set_led_green();
        gpio_direction_output(IO_PORTA_00, 1);  // 启用C端扬声器
        g_audio_playing = 1;  // 设置为1，表示正在播放loud_test.mp3
        g_mode3_playing_prompt = 1;  // 标记正在播放提示音
        log_i("MODE 3: Audio loop mode started, C speaker enabled");
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
        gpio_direction_output(IO_PORTA_00, 1);  // 启用C端扬声器
        g_audio_playing = 1;  // 设置为1，表示正在播放test_snkey.mp3
        log_i("MODE 7: enter mode7 test_snkey.mp3");
        {
            log_i("MODE 7: Starting SN key retrieval mode");
            lwl_app_context_t *context7 = lwl_app_core_context_get_instance();
            int err = app_music_play_local_music_with_callback(
                context7->music_player,
                CONFIG_VOICE_PROMPT_FILE_PATH"test_snkey.mp3",
                mode7_audio_callback, NULL);
            if (err != 0) {
                log_e("MODE 7: Failed to play test_snkey.mp3, error: %d", err);
                g_audio_playing = 0;  // 播放失败时重置状态
            } else {
                log_i("MODE 7: test_snkey.mp3 started successfully");
            }
            log_i("MODE 7: SN key retrieval mode initialized");
        }
        // 获取SN号功能
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
    // 如果提示音还在播放中，则等待播放完成
    if (g_audio_playing) {
        // 等待音频播放完成，无需特殊处理
        return;
    }
    
    // 模式1不再需要超时处理，LED状态完全由按键按下/释放事件控制
}

// 模式1音频播放回调函数
static void mode1_audio_callback(void *priv)
{
    // 检查当前模式，如果不是模式1，则忽略回调
    if (g_current_mode != WORK_MODE_1) {
        log_i("MODE 1: Audio callback ignored - not in mode 1 (current mode: %d)", g_current_mode);
        return;
    }
    
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
    // 检查当前模式，如果不是模式3，则忽略回调
    if (g_current_mode != WORK_MODE_3) {
        log_i("MODE 3: Audio callback ignored - not in mode 3 (current mode: %d)", g_current_mode);
        return;
    }
    
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
    // 检查当前模式，如果不是模式4，则忽略回调
    if (g_current_mode != WORK_MODE_4) {
        log_i("MODE 4: Audio callback ignored - not in mode 4 (current mode: %d)", g_current_mode);
        return;
    }
    
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

// 模式7网络检查状态变量
static u8 g_mode7_network_check_done = 0;
static u8 g_mode7_sn_key_obtained = 0;     // SN和Key是否已获取成功
static u8 g_mode7_first_touch_handled = 0; // 第一次触摸按键是否已处理
// 模式7写入请求延迟到任务上下文执行的标志与运行中标志
static u8 g_mode7_write_pending = 0;
static u8 g_mode7_op_running = 0;

// 全局LittleFS访问读写锁（保护所有lfs_*并发访问）
#include "../../util/rwlock.h"
static rwlock_t g_lfs_rwlock;

// 模式7存储从服务器获取的SN和Key
#define MODE7_SN_SIZE 32
#define MODE7_KEY_SIZE 64
static char g_mode7_obtained_sn[MODE7_SN_SIZE] = {0};   // 从服务器获取的SN
static char g_mode7_obtained_key[MODE7_KEY_SIZE] = {0}; // 从服务器获取的Key

// 模式7重置函数
static void mode7_reset_network_check(void)
{
    g_mode7_network_check_done = 0;
    g_mode7_sn_key_obtained = 0;
    g_mode7_first_touch_handled = 0;
    g_mode7_write_pending = 0;
    g_mode7_op_running = 0;
    // 清空存储的SN和Key
    memset(g_mode7_obtained_sn, 0, MODE7_SN_SIZE);
    memset(g_mode7_obtained_key, 0, MODE7_KEY_SIZE);
    log_i("MODE 7: Network check state reset");
}

// 新增模式7处理函数
static void process_mode_7(void)
{
    // 如果提示音还在播放中，则等待播放完成
    if (g_audio_playing) {
        // 等待音频播放完成，无需特殊处理
        return;
    }

    // 如果有挂起的写入请求，且当前未在执行写入，则在任务上下文执行
    if (g_mode7_write_pending && !g_mode7_op_running && g_mode7_sn_key_obtained) {
        g_mode7_op_running = 1;
        log_i("MODE 7: Pending SN/Key write detected, executing in task context");
        int result = mode7_write_and_verify_sn_key(g_mode7_obtained_sn, g_mode7_obtained_key);
        
        log_i("MODE 7: Write+Verify completed with result=%d", result);
        log_i("MODE 7: Write+Verify completed with result=%d", result);
        log_i("MODE 7: Write+Verify completed with result=%d", result);
        log_i("MODE 7: Write+Verify completed with result=%d", result);
        log_i("MODE 7: Write+Verify completed with result=%d", result);
        log_i("MODE 7: Write+Verify completed with result=%d", result);
        log_i("MODE 7: Write+Verify completed with result=%d", result);
        log_i("MODE 7: Write+Verify completed with result=%d", result);
        g_mode7_write_pending = 0;
        g_mode7_op_running = 0;
    }

    // 提示音播放完成后，SN请求将在audio callback中执行
    // 其余无需额外处理
}

// 模式7执行SN请求函数
static void mode7_execute_sn_request(void)
{
    // 只执行一次网络检查
    if (g_mode7_network_check_done) {
        return; // 网络检查已完成，无需重复执行
    }
    
    g_mode7_network_check_done = 1;

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
        
        // 保存获取到的SN和Key到全局变量
        strncpy(g_mode7_obtained_sn, sn->valuestring, MODE7_SN_SIZE - 1);
        g_mode7_obtained_sn[MODE7_SN_SIZE - 1] = '\0';  // 确保字符串终止
        strncpy(g_mode7_obtained_key, key->valuestring, MODE7_KEY_SIZE - 1);
        g_mode7_obtained_key[MODE7_KEY_SIZE - 1] = '\0';  // 确保字符串终止
        
        log_i("===============================[MODE7] SN and Key saved to global variables=======================");
        
        set_led_red();
        log_i("========================[MODE7] SN/Key obtained successfully, red LED is ON=================");
        
        // 设置SN和Key获取成功标志
        g_mode7_sn_key_obtained = 1;
        
        // 播放获取SN成功的音频
        lwl_app_context_t *context7_success = lwl_app_core_context_get_instance();
        g_audio_playing = 1;  // 设置音频播放标志
        int err = app_music_play_local_music_with_callback(
            context7_success->music_player,
            CONFIG_VOICE_PROMPT_FILE_PATH"test_getsn.mp3",
            mode7_success_audio_callback, NULL);
        if (err != 0) {
            log_e("MODE 7: Failed to play test_getsn.mp3, error: %d", err);
            g_audio_playing = 0;  // 播放失败时重置状态
        } else {
            log_i("MODE 7: test_getsn.mp3 started successfully");
        }
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
    // 检查当前模式，如果不是模式5，则忽略回调
    if (g_current_mode != WORK_MODE_5) {
        log_i("MODE 5: Audio callback ignored - not in mode 5 (current mode: %d)", g_current_mode);
        return;
    }
    
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

// 模式7音频播放回调函数
static void mode7_audio_callback(void *priv)
{
    // 检查当前模式，如果不是模式7，则忽略回调
    if (g_current_mode != WORK_MODE_7) {
        log_i("MODE 7: Audio callback ignored - not in mode 7 (current mode: %d)", g_current_mode);
        return;
    }
    
    log_i("MODE 7: test_snkey.mp3 playback completed");
    g_audio_playing = 0;
    
    // 模式7音频播放完成后，开始SN号获取流程
    log_i("MODE 7: Audio playback completed, starting SN key retrieval process");
    mode7_execute_sn_request();
}

// 模式7成功音频播放回调函数
static void mode7_success_audio_callback(void *priv)
{
    // 检查当前模式，如果不是模式7，则忽略回调
    if (g_current_mode != WORK_MODE_7) {
        log_i("MODE 7: Success audio callback ignored - not in mode 7 (current mode: %d)", g_current_mode);
        return;
    }
    
    log_i("MODE 7: test_getsn.mp3 playback completed");
    g_audio_playing = 0;
    
    // SN获取成功音频播放完成后的处理
    log_i("MODE 7: SN retrieval success audio completed");
}

// 模式7 test_wrsn.mp3 音频播放回调函数
static void mode7_wrsn_audio_callback(void *priv)
{
    // 检查当前模式，如果不是模式7，则忽略回调
    if (g_current_mode != WORK_MODE_7) {
        log_i("MODE 7: WRSN audio callback ignored - not in mode 7 (current mode: %d)", g_current_mode);
        return;
    }
    
    log_i("MODE 7: test_wrsn.mp3 playback completed");
    g_audio_playing = 0;
    
    log_i("MODE 7: WRSN audio playback finished");
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
    log_i("devices_check_cb  %s\n", pdev->sn);
    switch(code) {
    case BKA_CMD_CODE_COMPL:
        log_i("Switch PortE OK\n");
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

//=================================================================================================
// LWL event callback
//=================================================================================================
static void devices_check_lwl_event_handler_cb(lwl_event_t *e)
{
    static int en = 1;
    lwl_eend_dev_t *p_eend_dev = e->distance_change.pdev;
    switch (e->event_type) {
    case LWL_EVENT_TYPE_EEND_DEV_CLOSEST_CHANGE: {
        log_i("LWL_EVENT_TYPE_EEND_DEV_CLOSEST_CHANGE %d %d", p_eend_dev, p_eend_dev);
        if (p_eend_dev != NULL) {
            char sn_string[SN_SIZE] = "(NULL)";
            p_eend_dev->get_string(p_eend_dev, sn_string, sizeof(sn_string));
            log_i("LWL_EVENT_TYPE_EEND_DEV_CLOSEST_CHANGE %s", sn_string);
        }
    }   break;
    case LWL_EVENT_TYPE_EEND_DEV_DISTANCE_CHANGE: {
        char sn_string[SN_SIZE * 2 + 1] = "(NULL)";
        p_eend_dev->get_string(p_eend_dev, sn_string, sizeof(sn_string));
        log_i("[MEASDIST] %s: %d %d\r\n", sn_string, p_eend_dev->distance_info.rssi, p_eend_dev->distance_info.distance_type);
        if (p_eend_dev->distance_info.distance_type == DISTANCE_0) {
            add_pdev_to_list(p_eend_dev); // 新增：维护pdev列表
        } else if (p_eend_dev->distance_info.distance_type == DISTANCE_2 || p_eend_dev->distance_info.distance_type == DISTANCE_3) {
            remove_pdev_from_list(p_eend_dev); // 新增：从列表中移除pdev
        }
    }   break;
    case LWL_EVENT_TYPE_EEND_DEV_NEW_IN:
        log_i("ORMET_NEW_DEVICE_IN");
        break;
    // case LWL_EVENT_TYPE_EEND_DEVLOST:
    //     if (p_eend_dev != NULL) {
    //         remove_pdev_from_list(p_eend_dev);
    //     }
        // break;
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
    case LWL_EVENT_TYPE_UART_CLEAR_WIFI_SSID:
        // return to WORK_MODE_1
        switch_to_mode(WORK_MODE_1);
        break;
    case LWL_EVENT_TYPE_UART_SYNC:
        log_i("battery: %d", e->uart_cmd_sync.battery);
        log_i("SYNC response by devices_check_lwl_event_handler_cb (1, 12)");
        if(e->uart_cmd_sync.battery < 30 ){
            // set_led_red();
        }
        uart_response_synch_status(1, 12);/// 0是idle 1是busy，必须回复
        break;
    case LWL_EVENT_TYPE_UART_TOUCH_KEY_DOWN:
        log_i("=== UART TOUCH KEY DOWN DETECTED ===");
        log_i("Touch key pressed down");
        log_i("Event timestamp: %d ms", timer_get_ms());
        log_i("Current mode: %s (%d)", mode_names[g_current_mode], g_current_mode);
        
        log_i("Touch key event type: %d",e->uart_cmd_touch.touch_key_event_type);
        
        // 特殊处理模式7的触摸按键检测
        if (g_current_mode == WORK_MODE_7 && g_mode7_sn_key_obtained && !g_mode7_first_touch_handled) {
            if (e->uart_cmd_touch.touch_key_event_type == 0) {
                // 第一次触摸按键按下事件，且SN/Key已获取成功
                log_i("MODE 7: First touch key detected after SN/Key obtained");
                
                //=============================================================================================================================================================================================
                // 添加刷写SN和Key的代码
                log_i("MODE 7: ========== STARTING SN/KEY WRITE PROCESS ==========");
                log_i("MODE 7: SN to write: %s", g_mode7_obtained_sn);
                log_i("MODE 7: Key to write: %s", g_mode7_obtained_key);
                log_i("MODE 7: ==================================================================");
                // 触发写入动作：改为设置挂起标志，避免在事件回调/模式切换上下文直接进行LFS操作
                g_mode7_write_pending = 1;
                int result = 0; // 实际结果将在任务上下文执行时输出

                //=============================================================================================================================================================================================

                // 打印十行分隔符
                for (int i = 0; i < 10; i++) {
                    log_i("。。。。。。。。。。。。。。。。。。。。。。。。。。。。。。。。。");
                }
                
                // 播放test_wrsn.mp3
                lwl_app_context_t *context7_wrsn = lwl_app_core_context_get_instance();
                if (context7_wrsn && context7_wrsn->music_player) {
                    g_audio_playing = 1;
                    int err = app_music_play_local_music_with_callback(
                        context7_wrsn->music_player,
                        CONFIG_VOICE_PROMPT_FILE_PATH"test_wrsn.mp3",
                        mode7_wrsn_audio_callback, NULL);
                    if (err != 0) {
                        log_e("MODE 7: Failed to play test_wrsn.mp3, error: %d", err);
                        g_audio_playing = 0;
                    } else {
                        log_i("MODE 7: test_wrsn.mp3 started successfully");
                    }
                }
                
                // 标记第一次触摸已处理
                g_mode7_first_touch_handled = 1;
                log_i("MODE 7: First touch handling completed");
            }
        }
        // 特殊处理模式1的触摸按键检测
        else if (g_current_mode == WORK_MODE_1 && g_mode1_touch_enabled) {
            if (e->uart_cmd_touch.touch_key_event_type == 0) {
                // 按键按下事件：设置蓝灯
                set_led_blue();
                g_mode1_touch_active = 1;
                log_i("MODE 1: Touch key pressed - LED set to BLUE");
                log_i("++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++");
            } else if (e->uart_cmd_touch.touch_key_event_type == 1) {
                // 按键释放事件：恢复绿灯
                set_led_green();
                g_mode1_touch_active = 0;
                log_i("MODE 1: Touch key released - LED restored to GREEN");
                log_i("------------------------------------------------------------------------");
            }
        } else {
            // 特殊处理初始模式的触摸按键检测
            if (g_current_mode == WORK_MODE_INIT) {
                if (e->uart_cmd_touch.touch_key_event_type == 0) {
                    //set_led_black(); // 控制 LED 关闭
                    //msleep(1000);
                    //uart_response_synch_status(0, 12);/// 音量  e的声音 only
                    log_i("*******************************************************************************************************");
                    //log_i("uart_response_synch_status(0, 12);");
                    //log_i("INIT MODE: Touch key pressed in initial mode");
                    //log_i("the UART_CMD_POWER_OFF !\r\n");
                    //msleep(1000);
                    //lwl_event_t e;
                    //e.event_type = LWL_EVENT_TYPE_UART_SHUTDOWN;
                    //lwl_event_notify(&e);                    
                }
            } else {
                // 其他模式的触摸按键检测：设置黄灯（原有逻辑）
                log_i("Touch key detected, LED set to YELLOW temporarily");
            }
        }
        break;
    case LWL_EVENT_TYPE_UART_TOUCH_KEY_UP:
        log_i("=== UART TOUCH KEY UP DETECTED ===");
        log_i("Touch key released");
        log_i("Event timestamp: %d ms", timer_get_ms());
        log_i("Current mode: %s (%d)", mode_names[g_current_mode], g_current_mode);
        
        log_i("Touch key event type: %d", e->uart_cmd_touch.touch_key_event_type);
        
        // 可以在这里添加触摸按键抬起的特殊处理逻辑
        if (g_current_mode == WORK_MODE_1 && g_mode1_touch_enabled && g_mode1_touch_active) {
            // 模式1中的触摸按键抬起：恢复绿灯
            set_led_green();
            g_mode1_touch_active = 0;
            log_i("MODE 1: Touch key released - LED restored to GREEN");
            log_i("------------------------------------------------------------------------");
        } else {
            log_i("Touch key released in mode %s", mode_names[g_current_mode]);
        }
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

    pdev_list_count = 0; // 清空设备列表
    /* 注册lwl事件处理回调 */
    lwl_event_handler_add(devices_check_lwl_event_handler_cb);
    
    // 初始化模式系统，进入初始模式（红灯常亮）
    switch_to_mode(WORK_MODE_INIT);

    // 初始化全局LFS读写锁
    rwlock_init(&g_lfs_rwlock);
    
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

    // 销毁全局LFS读写锁
    rwlock_destroy(&g_lfs_rwlock);
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

//=================================================================================================
// 模式7 SN/Key写入功能实现
//=================================================================================================

/**
 * @brief 将SN和Key写入存储
 * @param sn 序列号字符串
 * @param key 密钥字符串 
 * @return 0成功, -1失败
 */
static int mode7_write_sn_key_to_storage(const char* sn, const char* key)
{
    if (!sn || !key) {
        log_e("MODE 7: Invalid SN or Key pointer");
        return -1;
    }
    
    if (strlen(sn) == 0 || strlen(key) == 0) {
        log_e("MODE 7: Empty SN or Key string");
        return -2;
    }
    
    log_i("MODE 7: Writing SN/Key to storage...");
    log_i("MODE 7: SN: %s (length: %d)", sn, strlen(sn));
    log_i("MODE 7: Key: %s (length: %d)", key, strlen(key));
    
    // 真正的LittleFS写入实现
    struct lfs_info info;
    lfs_file_t file;
    int ret;

    // 写操作加写锁
    rwlock_acquire_write(&g_lfs_rwlock);
    
    // 先删除旧文件（如果存在）
    ret = lfs_stat(&g_lfs, "MODE7_SN_KEY.bin", &info);
    if (ret == LFS_ERR_OK) {
        lfs_remove(&g_lfs, "MODE7_SN_KEY.bin");
        log_i("MODE 7: Removed existing SN/Key file");
    }
    
    // 创建新文件
    ret = lfs_file_open(&g_lfs, &file, "MODE7_SN_KEY.bin", 
                        LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC);
    if (ret < 0) {
        log_e("MODE 7: Failed to create SN/Key file, error: %d", ret);
        rwlock_release_write(&g_lfs_rwlock);
        return -3;
    }
    
    // 写入数据格式：
    // [4 bytes] SN长度
    // [SN data] SN字符串
    // [4 bytes] Key长度 
    // [Key data] Key字符串
    
    uint32_t sn_len = strlen(sn);
    uint32_t key_len = strlen(key);
    
    // 写入SN长度
    lfs_ssize_t bytes_written = lfs_file_write(&g_lfs, &file, &sn_len, sizeof(sn_len));
    if (bytes_written != sizeof(sn_len)) {
        log_e("MODE 7: Failed to write SN length, written: %d", bytes_written);
        lfs_file_close(&g_lfs, &file);
        rwlock_release_write(&g_lfs_rwlock);
        return -4;
    }
    
    // 写入SN数据
    bytes_written = lfs_file_write(&g_lfs, &file, sn, sn_len);
    if (bytes_written != sn_len) {
        log_e("MODE 7: Failed to write SN data, written: %d, expected: %d", bytes_written, sn_len);
        lfs_file_close(&g_lfs, &file);
        rwlock_release_write(&g_lfs_rwlock);
        return -5;
    }
    
    // 写入Key长度
    bytes_written = lfs_file_write(&g_lfs, &file, &key_len, sizeof(key_len));
    if (bytes_written != sizeof(key_len)) {
        log_e("MODE 7: Failed to write Key length, written: %d", bytes_written);
        lfs_file_close(&g_lfs, &file);
        rwlock_release_write(&g_lfs_rwlock);
        return -6;
    }
    
    // 写入Key数据
    bytes_written = lfs_file_write(&g_lfs, &file, key, key_len);
    if (bytes_written != key_len) {
        log_e("MODE 7: Failed to write Key data, written: %d, expected: %d", bytes_written, key_len);
        lfs_file_close(&g_lfs, &file);
        rwlock_release_write(&g_lfs_rwlock);
        return -7;
    }
    
    // 关闭文件
    lfs_file_close(&g_lfs, &file);
    // 释放写锁（验证函数内部再加读锁）
    rwlock_release_write(&g_lfs_rwlock);
    
    log_i("MODE 7: ========== SN/KEY WRITE SUCCESS ==========");
    log_i("MODE 7: Written SN:  %s (%d bytes)", sn, sn_len);
    log_i("MODE 7: Written Key: %s (%d bytes)", key, key_len);
    log_i("MODE 7: File: MODE7_SN_KEY.bin");
    log_i("MODE 7: ==========================================");
    
    // 验证写入的数据
    ret = mode7_verify_written_sn_key(sn, key);
    if (ret != 0) {
        log_e("MODE 7: Verification failed, error: %d", ret);
        return -8;
    }
    
    log_i("MODE 7: SN/Key verification passed");
    return 0;  // 成功
}

/**
 * @brief 验证写入的SN和Key是否正确
 * @param expected_sn 期望的SN
 * @param expected_key 期望的Key
 * @return 0成功, -1失败
 */
static int mode7_verify_written_sn_key(const char* expected_sn, const char* expected_key)
{
    if (!expected_sn || !expected_key) {
        log_e("MODE 7: Invalid expected SN or Key pointer");
        return -1;
    }
    
    log_i("MODE 7: Verifying written SN/Key...");
    
    struct lfs_info info;
    lfs_file_t file;
    int ret;

    // 读操作加读锁
    rwlock_acquire_read(&g_lfs_rwlock);
    
    // 检查文件是否存在
    ret = lfs_stat(&g_lfs, "MODE7_SN_KEY.bin", &info);
    if (ret != LFS_ERR_OK) {
        log_e("MODE 7: SN/Key file does not exist, error: %d", ret);
        rwlock_release_read(&g_lfs_rwlock);
        return -2;
    }
    
    // 打开文件进行读取
    ret = lfs_file_open(&g_lfs, &file, "MODE7_SN_KEY.bin", LFS_O_RDONLY);
    if (ret < 0) {
        log_e("MODE 7: Failed to open SN/Key file for verification, error: %d", ret);
        rwlock_release_read(&g_lfs_rwlock);
        return -3;
    }
    
    // 读取并验证SN
    uint32_t read_sn_len;
    lfs_ssize_t bytes_read = lfs_file_read(&g_lfs, &file, &read_sn_len, sizeof(read_sn_len));
    if (bytes_read != sizeof(read_sn_len)) {
        log_e("MODE 7: Failed to read SN length, read: %d", bytes_read);
        lfs_file_close(&g_lfs, &file);
        rwlock_release_read(&g_lfs_rwlock);
        return -4;
    }
    
    if (read_sn_len != strlen(expected_sn)) {
        log_e("MODE 7: SN length mismatch, expected: %d, read: %d", strlen(expected_sn), read_sn_len);
        lfs_file_close(&g_lfs, &file);
        rwlock_release_read(&g_lfs_rwlock);
        return -5;
    }
    
    char read_sn[MODE7_SN_SIZE] = {0};
    bytes_read = lfs_file_read(&g_lfs, &file, read_sn, read_sn_len);
    if (bytes_read != read_sn_len) {
        log_e("MODE 7: Failed to read SN data, read: %d, expected: %d", bytes_read, read_sn_len);
        lfs_file_close(&g_lfs, &file);
        rwlock_release_read(&g_lfs_rwlock);
        return -6;
    }
    
    if (strncmp(read_sn, expected_sn, read_sn_len) != 0) {
        log_e("MODE 7: SN data mismatch");
        log_e("MODE 7: Expected: %s", expected_sn);
        log_e("MODE 7: Read:     %.*s", read_sn_len, read_sn);
        lfs_file_close(&g_lfs, &file);
        rwlock_release_read(&g_lfs_rwlock);
        return -7;
    }
    
    // 读取并验证Key
    uint32_t read_key_len;
    bytes_read = lfs_file_read(&g_lfs, &file, &read_key_len, sizeof(read_key_len));
    if (bytes_read != sizeof(read_key_len)) {
        log_e("MODE 7: Failed to read Key length, read: %d", bytes_read);
        lfs_file_close(&g_lfs, &file);
        rwlock_release_read(&g_lfs_rwlock);
        return -8;
    }
    
    if (read_key_len != strlen(expected_key)) {
        log_e("MODE 7: Key length mismatch, expected: %d, read: %d", strlen(expected_key), read_key_len);
        lfs_file_close(&g_lfs, &file);
        rwlock_release_read(&g_lfs_rwlock);
        return -9;
    }
    
    char read_key[MODE7_KEY_SIZE] = {0};
    bytes_read = lfs_file_read(&g_lfs, &file, read_key, read_key_len);
    if (bytes_read != read_key_len) {
        log_e("MODE 7: Failed to read Key data, read: %d, expected: %d", bytes_read, read_key_len);
        lfs_file_close(&g_lfs, &file);
        rwlock_release_read(&g_lfs_rwlock);
        return -10;
    }
    
    if (strncmp(read_key, expected_key, read_key_len) != 0) {
        log_e("MODE 7: Key data mismatch");
        log_e("MODE 7: Expected: %s", expected_key);
        log_e("MODE 7: Read:     %.*s", read_key_len, read_key);
        lfs_file_close(&g_lfs, &file);
        rwlock_release_read(&g_lfs_rwlock);
        return -11;
    }
    
    // 关闭文件
    lfs_file_close(&g_lfs, &file);
    // 释放读锁
    rwlock_release_read(&g_lfs_rwlock);
    
    log_i("MODE 7: ========== VERIFICATION SUCCESS ==========");
    log_i("MODE 7: SN verification:  PASSED (%s)", expected_sn);
    log_i("MODE 7: Key verification: PASSED (%s)", expected_key);
    log_i("MODE 7: File size: %d bytes", info.size);
    log_i("MODE 7: ========================================");
    
    return 0;  // 验证成功
}

/**
 * @brief 从存储中读取SN和Key
 * @param sn_out 输出SN的缓冲区
 * @param sn_size SN缓冲区大小
 * @param key_out 输出Key的缓冲区  
 * @param key_size Key缓冲区大小
 * @return 0成功, -1失败
 */
static int mode7_read_sn_key_from_storage(char* sn_out, int sn_size, char* key_out, int key_size)
{
    if (!sn_out || !key_out || sn_size <= 0 || key_size <= 0) {
        log_e("MODE 7: Invalid output buffers or sizes");
        return -1;
    }
    
    log_i("MODE 7: Reading SN/Key from storage...");
    
    struct lfs_info info;
    lfs_file_t file;
    int ret;

    // 读操作加读锁
    rwlock_acquire_read(&g_lfs_rwlock);
    
    // 检查文件是否存在
    ret = lfs_stat(&g_lfs, "MODE7_SN_KEY.bin", &info);
    if (ret != LFS_ERR_OK) {
        log_e("MODE 7: SN/Key file does not exist, error: %d", ret);
        rwlock_release_read(&g_lfs_rwlock);
        return -2;
    }
    
    // 打开文件进行读取
    ret = lfs_file_open(&g_lfs, &file, "MODE7_SN_KEY.bin", LFS_O_RDONLY);
    if (ret < 0) {
        log_e("MODE 7: Failed to open SN/Key file for reading, error: %d", ret);
        rwlock_release_read(&g_lfs_rwlock);
        return -3;
    }
    
    // 读取SN长度
    uint32_t sn_len;
    lfs_ssize_t bytes_read = lfs_file_read(&g_lfs, &file, &sn_len, sizeof(sn_len));
    if (bytes_read != sizeof(sn_len)) {
        log_e("MODE 7: Failed to read SN length, read: %d", bytes_read);
        lfs_file_close(&g_lfs, &file);
        rwlock_release_read(&g_lfs_rwlock);
        return -4;
    }
    
    if (sn_len >= sn_size) {
        log_e("MODE 7: SN buffer too small, need: %d, have: %d", sn_len + 1, sn_size);
        lfs_file_close(&g_lfs, &file);
        rwlock_release_read(&g_lfs_rwlock);
        return -5;
    }
    
    // 读取SN数据
    bytes_read = lfs_file_read(&g_lfs, &file, sn_out, sn_len);
    if (bytes_read != sn_len) {
        log_e("MODE 7: Failed to read SN data, read: %d, expected: %d", bytes_read, sn_len);
        lfs_file_close(&g_lfs, &file);
        rwlock_release_read(&g_lfs_rwlock);
        return -6;
    }
    sn_out[sn_len] = '\0';  // 确保字符串终止
    
    // 读取Key长度
    uint32_t key_len;
    bytes_read = lfs_file_read(&g_lfs, &file, &key_len, sizeof(key_len));
    if (bytes_read != sizeof(key_len)) {
        log_e("MODE 7: Failed to read Key length, read: %d", bytes_read);
        lfs_file_close(&g_lfs, &file);
        rwlock_release_read(&g_lfs_rwlock);
        return -7;
    }
    
    if (key_len >= key_size) {
        log_e("MODE 7: Key buffer too small, need: %d, have: %d", key_len + 1, key_size);
        lfs_file_close(&g_lfs, &file);
        rwlock_release_read(&g_lfs_rwlock);
        return -8;
    }
    
    // 读取Key数据
    bytes_read = lfs_file_read(&g_lfs, &file, key_out, key_len);
    if (bytes_read != key_len) {
        log_e("MODE 7: Failed to read Key data, read: %d, expected: %d", bytes_read, key_len);
        lfs_file_close(&g_lfs, &file);
        rwlock_release_read(&g_lfs_rwlock);
        return -9;
    }
    key_out[key_len] = '\0';  // 确保字符串终止
    
    // 关闭文件
    lfs_file_close(&g_lfs, &file);
    // 释放读锁
    rwlock_release_read(&g_lfs_rwlock);
    
    log_i("MODE 7: ========== READ SUCCESS ==========");
    log_i("MODE 7: Read SN:  %s (%d bytes)", sn_out, sn_len);
    log_i("MODE 7: Read Key: %s (%d bytes)", key_out, key_len);
    log_i("MODE 7: File size: %d bytes", info.size);
    log_i("MODE 7: ====================================");
    
    return 0;  // 读取成功
}

/**
 * @brief 写入并验证SN和Key的综合函数
 * @param sn 序列号字符串
 * @param key 密钥字符串
 * @return 0成功, -1失败
 */
static int mode7_write_and_verify_sn_key(const char* sn, const char* key)
{
    if (!sn || !key) {
        log_e("MODE 7: Invalid SN or Key pointer");
        set_led_red();  // 参数错误，显示红灯
        return -1;
    }
    
    if (strlen(sn) == 0 || strlen(key) == 0) {
        log_e("MODE 7: Empty SN or Key string");
        set_led_red();  // 参数错误，显示红灯
        return -2;
    }
    
    log_i("MODE 7: Starting combined write and verify operation...");
    
    // 第一步：执行写入操作
    int write_result = mode7_write_sn_key_to_storage(sn, key);
    if (write_result != 0) {
        log_e("MODE 7: ========== SN/KEY WRITE FAILED ==========");
        log_e("MODE 7: Failed to write SN/Key to storage, error code: %d", write_result);
        set_led_red();    // 写入失败，显示红灯
        return write_result;
    }
    
    log_i("MODE 7: ========== SN/KEY WRITE SUCCESS ==========");
    log_i("MODE 7: SN/Key written to storage successfully!");
    
    // 第二步：执行读取验证
    char verify_sn[MODE7_SN_SIZE] = {0};
    char verify_key[MODE7_KEY_SIZE] = {0};
    int read_result = mode7_read_sn_key_from_storage(verify_sn, sizeof(verify_sn), verify_key, sizeof(verify_key));
    
    if (read_result != 0) {
        log_w("MODE 7: Read back verification failed, but write was successful");
        log_w("MODE 7: Read error code: %d", read_result);
        set_led_green();  // 写入成功但验证失败，仍显示绿灯
        return 0;  // 写入成功就算成功
    }
    
    // 第三步：验证数据一致性
    if (strcmp(verify_sn, sn) != 0 || strcmp(verify_key, key) != 0) {
        log_e("MODE 7: Data verification failed!");
        log_e("MODE 7: Original SN:  %s, Read SN:  %s", sn, verify_sn);
        log_e("MODE 7: Original Key: %s, Read Key: %s", key, verify_key);
        set_led_red();    // 验证失败，显示红灯
        return -100;
    }
    
    // 第四步：完全成功
    log_i("MODE 7: ========== COMPLETE SUCCESS ==========");
    log_i("MODE 7: Read back verification successful");
    log_i("MODE 7: Verified SN:  %s", verify_sn);
    log_i("MODE 7: Verified Key: %s", verify_key);
    log_i("MODE 7: All operations completed successfully!");
    set_led_green();  // 完全成功，显示绿灯
    
    return 0;  // 完全成功
}