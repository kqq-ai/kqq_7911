#include "mode_list.h"

// 为模式4加速度传感器功能添加必要的头文件
#include "../../../common/gsensor/gSensor_manage.h"  // G-Sensor管理头文件
#include "../../SC7A20/sc7a20.h"           // SC7A20陀螺仪头文件
#include "../../SC7A20/app_sc7a20_task.h"  // SC7A20任务头文件
#include "../../LED/LED.h"

static void set_led_mic_on(void)    { set_rgb_ON(COLOR_YELLOW,1); }
static void set_led_mic_off(void)   { set_rgb_OFF(COLOR_YELLOW,1); }
static void set_led_vad_on(void)    { set_rgb_ON(COLOR_GREEN,2); }
static void set_led_vad_off(void)   { set_rgb_OFF(COLOR_GREEN,2); }

// 模式4 VAD相关变量
static int g_vad_state = 0;          // 当前VAD状态 (0=无语音, 1=有语音)
static int g_vad_state_prev = 0;     // 前一次VAD状态
static u32 g_vad_start_time = 0;     // 语音开始时间
static u32 g_vad_duration = 0;       // 语音持续时间
static u8 g_mode4_vad_enabled = 0;   // 模式4 VAD是否已启用
// 模式4 VAD功能声明
static void mode4_vad_callback(void* priv, int type);
static void mode4_vad_status_update(void);
// 模式4音频播放回调函数
void mode4_audio_callback(void *priv)
{
    // 检查当前模式，如果不是模式4，则忽略回调
    if (get_current_mode() != WORK_MODE_4) {
        log_i("MODE 4: Audio callback ignored - not in mode 4 (current mode: %d)", get_current_mode());
        return;
    }
    
    log_i("MODE 4: Prompt audio callback - finished, starting VAD monitoring");
    g_audio_playing = 0;
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
    if (get_current_mode() != WORK_MODE_4) {
        log_d("[MODE 4 VAD] Ignoring VAD event - not in mode 4 (current mode: %d)", get_current_mode());
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
        set_led_vad_on();
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
        set_led_vad_off();
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
    set_led_mic_on();
    if (ret != 0) {
        log_e("[MODE 4 VAD] Failed to start recorder: %d", ret);
        return -1;
    }
    g_mode4_vad_enabled = 1;
    
    log_i("[MODE 4 VAD] VAD functionality initialized and started");
    return 0;
}

/**
 * @brief 停止VAD功能
 */
static void mode4_vad_uninit(void)
{
    log_i("Cleaning up MODE 4 VAD functionality");
    // 如果VAD功能未启用，直接返回
    if (!g_mode4_vad_enabled) {
        log_i("[MODE 4 VAD] VAD functionality already stopped");
        return;
    }
    
    lwl_app_context_t *context = lwl_app_core_context_get_instance();
    
    if (context && context->recorder) {
        // 停止录音器
        recorder_stop(context->recorder);
        set_led_mic_off();
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
void process_mode_4(void)
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


void mode4_init()
{
    g_audio_playing = 1;  // 设置为1，表示正在播放micro_test.mp3
    g_mode4_vad_enabled = 0;  // VAD尚未启用
    log_i("MODE 4: micro VAD detection mode started");
    // 播放进入模式4的音频，使用回调
    lwl_app_context_t *context4 = lwl_app_core_context_get_instance();
    app_music_play_local_music_with_callback(
        context4->music_player,
        CONFIG_CP_VOICE_FILE_PATH"test_micro.mp3",
        mode4_audio_callback, NULL);
    mode4_vad_init();
    log_i("Playing test_micro.mp3 for MODE 4 with callback");
}

void mode4_uninit()
{
    mode4_vad_uninit();
}