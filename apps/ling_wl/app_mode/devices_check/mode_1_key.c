#include "mode_list.h"
#include "../lwl_app_framework.h"
#include "log.h"
#include "../../LED/LED.h"

// 模式1 触摸按键检测相关变量
static u8 g_mode1_touch_enabled = 0;    // 模式1 触摸检测是否已启用
static u32 g_mode1_touch_start_time = 0; // 触摸开始时间（保留兼容性）
static u8 g_mode1_touch_active = 0;     // 触摸是否正在激活 (0=绿灯, 1=红灯)
static u8 t_key_playing = 0;            // 模式1：t_key.mp3 播放标志


static u32 g_mode_timer = 0;
static u8 g_led_color_index = 0;
 
// 模式1：t_key.mp3 播放结束回调
void mode1_tkey_audio_end_cb(void *priv)
{
    // 仅在仍处于模式1时复位播放标志
    if (get_current_mode() == WORK_MODE_1) {
        t_key_playing = 0;
    }
    g_audio_playing = 0;
    log_i("MODE 1: t_key.mp3 playback finished");
}

void process_mode_1_touch_key(u8 touch_key_event_type)
{
    if (g_mode1_touch_enabled) {
        if (touch_key_event_type == 0) {
            g_mode1_touch_active = 1;
            log_i("MODE 1: Touch key pressed - LED set to BLUE");
            log_i("++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++");
            // 播放 t_key.mp3（仅在未播放时触发）
            if (!t_key_playing && !g_audio_playing) {
                t_key_playing = 1;
                g_audio_playing = 1;
                lwl_app_audio_play(CONFIG_CP_VOICE_FILE_PATH"t_key.mp3", mode1_tkey_audio_end_cb);
                log_i("MODE 1: t_key.mp3 started successfully");
            }
        } else if (touch_key_event_type == 1 && g_mode1_touch_active) {
            g_mode1_touch_active = 0;
            log_i("MODE 1: Touch key released - LED restored to GREEN");
            log_i("------------------------------------------------------------------------");
            // 允许下次按下时再次触发播放（若音频已结束回调会复位）
            t_key_playing = 0;
        }
    }
}

// 模式1处理函数：触摸按键检测
void process_mode_1(void)
{
    // 如果提示音还在播放中，则等待播放完成
    if (g_audio_playing) {
        // 等待音频播放完成，无需特殊处理
        return;
    }
    
    // 模式1不再需要超时处理，LED状态完全由按键按下/释放事件控制
}

// 模式1音频播放回调函数
void mode1_audio_callback(void *priv)
{
    // 检查当前模式，如果不是模式1，则忽略回调
    if (get_current_mode() != WORK_MODE_1) {
        log_i("MODE 1: Audio callback ignored - not in mode 1 (current mode: %d)", get_current_mode());
        return;
    }
    
    log_i("MODE 1: Prompt audio callback - finished, starting touch key monitoring");
    g_audio_playing = 0;
    
    // 提示音播放完成，启用触摸按键检测功能
    g_mode1_touch_enabled = 1;
    g_mode1_touch_active = 0;
    log_i("MODE 1: Touch key monitoring enabled successfully");
}

void mode1_init()
{
    g_audio_playing = 1;  // 设置为1，表示正在播放test_touchk.mp3
    g_mode1_touch_enabled = 0;  // 触摸检测尚未启用
    g_mode1_touch_active = 0;   // 触摸状态为非激活
    log_i("MODE 1: Touch key detection mode started, C speaker enabled");

    // 播放进入模式1的音频，使用回调
    lwl_app_audio_play(CONFIG_CP_VOICE_FILE_PATH"test_touchk.mp3", mode1_audio_callback);
    log_i("Playing test_touchk.mp3 for MODE 1 with callback");
}

static void mode1_uninit()
{
    // 如果当前在模式1且触摸检测已启用，则清理触摸检测功能
    if (g_mode1_touch_enabled) {
        log_i("Cleaning up MODE 1 touch key functionality");
        g_mode1_touch_enabled = 0;
        g_mode1_touch_active = 0;
    }
}