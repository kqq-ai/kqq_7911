#include "mode_list.h"

#include "gpio.h"
// 为模式3麦克风VAD功能添加必要的头文件
// 这些头文件可能需要根据实际的SDK结构进行调整
// #include "audio/recorder.h"     // 录音器相关头文件
// #include "audio/vad.h"          // VAD相关头文件

static u8 g_mode3_playing_prompt = 1; // 模式3状态：1=播放提示音，0=循环播放测试音

// 模式3音频播放回调函数 - C端播放
void mode3_audio_callback(void *priv)
{
    // 检查当前模式，如果不是模式3，则忽略回调
    if (get_current_mode() != WORK_MODE_3) {
        log_i("MODE 3: Audio callback ignored - not in mode 3 (current mode: %d)", get_current_mode());
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

// 模式3处理函数：先播放提示音，然后循环播放测试音频
void process_mode_3(void)
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
                CONFIG_CP_VOICE_FILE_PATH"test_1hz5s.mp3", 
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

void mode3_init()
{
    g_audio_playing = 1;  // 设置为1，表示正在播放loud_test.mp3
    g_mode3_playing_prompt = 1;  // 标记正在播放提示音
    log_i("MODE 3: Audio loop mode started, C speaker enabled");
    // 播放进入模式3的音频，使用回调    
    lwl_app_context_t *context3 = lwl_app_core_context_get_instance();
    app_music_play_local_music_with_callback(
        context3->music_player,
        CONFIG_CP_VOICE_FILE_PATH"test_loud.mp3",
        mode3_audio_callback, NULL);
    log_i("Playing test_loud.mp3 for MODE 3 with callback");
}

void mode3_uninit()
{
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