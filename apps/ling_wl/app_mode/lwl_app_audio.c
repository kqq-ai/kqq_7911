#include "lwl_app_audio.h"

#include "cpu.h"
#include "log.h"
#include "app_mode/lwl_app_core.h"
#include "app_core.h"
#include "net_music_play_my.h"
#include "gpio.h"
#include "os/os_api.h"
#include "util/threadpool_tasks.h"

/* 本地文件播放控制 */
static u8 g_play_voice_prompt = 0;          // 本地文件播放中标志
typedef int (*music_cbk_t)(void);
static music_cbk_t g_prompt_cb_priv = NULL; // 播放完成回调

/* 流式播放控制 */
static int g_stream_audio_on = false;       // 流式播放启动标志
static int g_stream_audio_playing = false;  // 流式播放实际状态
static int g_stream_audio_thread_running = false; // 流式播放线程运行标志

/* 全局互斥锁和上下文 */
static OS_MUTEX g_audio_mutex;
static lwl_app_context_t *g_app_ctx = NULL;

/* 音量控制 */
#define G_MAX_VOLUME 100
static int g_volume = G_MAX_VOLUME;

/* 前向声明 */
extern uint32_t wss_stream_get_data(uint8_t *buffer, uint32_t max_len);
extern bool wss_stream_on(void);

/* LED控制 */
extern void set_rgb_ON(int color, int num);
extern void set_rgb_OFF(int color, int num);
extern void bdo_enable(void);
#define COLOR_BLUE 3

//=================================================================================================
// 本地文件播放回调
//=================================================================================================
static void lwl_app_audio_play_cb(void *priv)
{
    log_i("[AUDIO_LOCAL]lwl_app_audio_play_cb");
    os_mutex_pend(&g_audio_mutex, 0);
    
    g_play_voice_prompt = 0;
    
    if (g_prompt_cb_priv) {
        g_prompt_cb_priv();
    }
    
    gpio_direction_output(IO_PORTA_00, 0);  // Disable C speaker
    os_mutex_post(&g_audio_mutex);
}

/**
 * @brief 设置本地播放器配置
 * @details
 *   启用C扬声器，设置默认音量，启用前级设备(7016)
 */
static void set_local_speaker(void)
{
    log_i("[AUDIO_LOCAL]set_local_speaker");
    gpio_direction_output(IO_PORTA_00, 1);  // Enable C speaker
    gpio_direction_output(IO_PORTB_02, 0);  // Disable E speaker
    set_dec_volume(g_app_ctx->music_player, INIT_VOLUME_VALUE);
}

/**
 * @brief 设置流式播放器配置
 * @details
 *   启用E扬声器，设置当前音量，启用前级设备(7016)
 */
static void set_stream_speaker(void)
{
    log_i("[AUDIO_STREAM]set_stream_speaker volume: %d", g_volume);
    
    // 先关闭本地播放
    lwl_app_audio_stop_playing();
    
    gpio_direction_output(IO_PORTA_00, 0);  // Disable C speaker
    gpio_direction_output(IO_PORTB_02, 1);  // Enable E speaker
    set_dec_volume(g_app_ctx->music_player, g_volume);
    bdo_enable();
}

/**
 * @brief 流式播放完成回调
 */
static void lwl_app_audio_stream_end_cb(void *priv)
{
    log_i("[AUDIO_STREAM]stream playback end callback");
    g_stream_audio_playing = false;
    g_stream_audio_on = false;
    set_rgb_OFF(COLOR_BLUE, 3);
}

//=================================================================================================
// 本地文件播放控制
//=================================================================================================

/**
 * @brief 播放本地提示音文件（同步等待前一个播放完成，然后启动新播放）
 * @param fname 音频文件名
 * @param priv  播放完成时的回调函数
 */
void lwl_app_audio_play(const char *fname, void *priv)
{
    // 等待上一次播放结束并回调完成
    while (g_play_voice_prompt) {
        msleep(10);
    }
    
    os_mutex_pend(&g_audio_mutex, 0);
    log_i("[AUDIO_LOCAL]lwl_app_audio_play: play %s", fname);

    // 先停止所有其他播放
    lwl_app_audio_stop_playing();
    lwl_app_audio_stream_stop();
    
    // 启动本地文件播放
    set_local_speaker();
    int err = app_music_play_local_music_with_callback(
        g_app_ctx->music_player, 
        fname, 
        lwl_app_audio_play_cb, 
        NULL
    );
    
    if (!err) {
        g_play_voice_prompt = 1;
        g_prompt_cb_priv = (music_cbk_t)priv;
    }
    os_mutex_post(&g_audio_mutex);
}

/**
 * @brief 检查本地文件播放是否正在进行
 */
bool lwl_app_audio_playing(void)
{
    bool rst;
    os_mutex_pend(&g_audio_mutex, 0);
    rst = g_play_voice_prompt;
    os_mutex_post(&g_audio_mutex);
    return rst;
}

/**
 * @brief 停止本地文件播放
 */
void lwl_app_audio_stop_playing(void)
{
    os_mutex_pend(&g_audio_mutex, 0);
    if (g_play_voice_prompt) {
        log_i("[AUDIO_LOCAL]stopping local audio playback");
        app_music_play_stop(g_app_ctx->music_player);
        g_play_voice_prompt = 0;
    }
    os_mutex_post(&g_audio_mutex);
}

//=================================================================================================
// 流式音频播放控制
//=================================================================================================

const uint32_t MAX_CHUNK_SIZE = 16000;
static uint8_t g_stream_buffer[MAX_CHUNK_SIZE];
#define STREAM_TIMEOUT_MS 5000

/**
 * @brief 流式音频播放线程函数
 * @details
 *   循环从stream缓冲队列中获取数据并写入播放器，直到stream结束且队列为空。
 *   如果5秒内没有收到音频数据则自动退出。
 */
static void lwl_app_audio_stream_playback_thread(void *priv)
{
    uint32_t last_data_time = timer_get_ms();
    
    log_i("[AUDIO_STREAM]playback thread started");

    // 等待stream启动
    while (wss_stream_on() == false && g_stream_audio_thread_running) {
        msleep(10);
    }
    
    log_i("[AUDIO_STREAM]stream is on, starting playback loop");
    
    while (g_stream_audio_thread_running) {
        /* 从stream获取数据 */
        uint32_t read_len = wss_stream_get_data(g_stream_buffer, MAX_CHUNK_SIZE);
        
        if (read_len > 0) {
            /* 有数据，写入播放器 */
            last_data_time = timer_get_ms();
            os_mutex_pend(&g_audio_mutex, 0);
            
            if (!g_stream_audio_thread_running) {
                os_mutex_post(&g_audio_mutex);
                break;
            }
            
            if (!g_stream_audio_on) {
                /* 第一次播放，启动播放器 */
                set_stream_speaker();
                app_music_play_stream_music_with_callback(
                    g_app_ctx->music_player, 
                    lwl_app_audio_stream_end_cb, 
                    NULL
                );
                g_stream_audio_on = true;
                set_rgb_ON(COLOR_BLUE, 3);
                log_i("[AUDIO_STREAM]playback started");
            }
            
            uint32_t written = app_music_play_stream_write(g_app_ctx->music_player, g_stream_buffer, read_len);
            if (written > 0) {
                log_d("[AUDIO_STREAM]stream_write %u bytes", written);
            } else {
                log_e("[AUDIO_STREAM]failed to write stream");
            }
            
            os_mutex_post(&g_audio_mutex);
        } else if (!g_stream_audio_on) {
            /* 无数据，检查是否stream已结束或超时 */
            if (!wss_stream_on()) {
                log_i("[AUDIO_STREAM]stream finish");
                break;
            } else {
                uint32_t elapsed = timer_get_ms() - last_data_time;
                if (elapsed > STREAM_TIMEOUT_MS) {
                    log_w("[AUDIO_STREAM]stream timeout: no data for %u ms, exiting", elapsed);
                    break;
                }
            }
            msleep(20);
        } else if (!g_stream_audio_playing) {
            /* 检查超时 */
            if (app_music_play_get_status(g_app_ctx->music_player) == AUDIO_DEC_START) {
                g_stream_audio_playing = true;
            } else {
                uint32_t elapsed = timer_get_ms() - last_data_time;
                if (elapsed > STREAM_TIMEOUT_MS) {
                    log_w("[AUDIO_STREAM]decoder timeout: no data for %u ms, exiting", elapsed);
                    break;
                }
            }
            msleep(20);
        }
    }

    os_mutex_pend(&g_audio_mutex, 0);
    if (g_stream_audio_on) {
        g_stream_audio_on = false;
        app_music_play_stop(g_app_ctx->music_player);
        g_stream_audio_playing = false;
        set_rgb_OFF(COLOR_BLUE, 3);
        log_i("[AUDIO_STREAM]audio playback stopped");
    }
    g_stream_audio_thread_running = false;
    os_mutex_post(&g_audio_mutex);
    
    log_i("[AUDIO_STREAM]playback thread exited");
}

/**
 * @brief 启动流式音频播放
 */
void lwl_app_audio_stream_start(void)
{
    if (g_stream_audio_thread_running) {
        log_w("[AUDIO_STREAM]playback thread already running");
        return;
    }
    
    log_i("[AUDIO_STREAM]starting playback thread");
    g_stream_audio_thread_running = true;
    
    // 启动播放线程
    threadpool_add_task(g_app_ctx->threadpool, lwl_app_audio_stream_playback_thread, NULL);
}

/**
 * @brief 停止流式音频播放
 */
void lwl_app_audio_stream_stop(void)
{
    log_i("[AUDIO_STREAM]lwl_app_audio_stream_stop");
    
    os_mutex_pend(&g_audio_mutex, 0);
    g_stream_audio_thread_running = false;
    if (g_stream_audio_on) {
        g_stream_audio_on = false;
        app_music_play_stop(g_app_ctx->music_player);
        g_stream_audio_playing = false;
        set_rgb_OFF(COLOR_BLUE, 3);
        log_i("[AUDIO_STREAM]audio playback stopped");
    }
    os_mutex_post(&g_audio_mutex);
}

/**
 * @brief 检查流式播放是否正在进行
 */
bool lwl_app_audio_stream_playing(void)
{
    bool rst;
    os_mutex_pend(&g_audio_mutex, 0);
    rst = g_stream_audio_thread_running || g_stream_audio_on;
    os_mutex_post(&g_audio_mutex);
    return rst;
}

//=================================================================================================
// 音量控制
//=================================================================================================

/**
 * @brief 设置播放器音量
 */
void lwl_app_audio_set_volume(int volume)
{
    if (volume < 1) volume = 1;
    if (volume > 100) volume = 100;
    
    os_mutex_pend(&g_audio_mutex, 0);
    g_volume = volume * G_MAX_VOLUME / 100;
    log_i("[AUDIO]set volume to %d (mapped to %d)", volume, g_volume);
    
    // 如果正在播放流式音频，立即更新音量
    if (g_stream_audio_on) {
        set_dec_volume(g_app_ctx->music_player, g_volume);
    }
    os_mutex_post(&g_audio_mutex);
}

//=================================================================================================
// 通用接口
//=================================================================================================

/**
 * @brief 获取当前音频播放状态
 */
bool lwl_app_audio_is_playing(void)
{
    os_mutex_pend(&g_audio_mutex, 0);
    bool playing = g_play_voice_prompt || g_stream_audio_thread_running || g_stream_audio_on;
    os_mutex_post(&g_audio_mutex);
    return playing;
}

//=================================================================================================
// 初始化和反初始化
//=================================================================================================

/**
 * @brief 初始化统一音频管理模块
 */
void lwl_app_audio_init(void)
{
    log_i("[AUDIO]Init unified audio management module");
    
    if (!os_mutex_valid(&g_audio_mutex)) {
        os_mutex_create(&g_audio_mutex);
    }

    g_app_ctx = lwl_app_core_context_get_instance();
    
    g_play_voice_prompt = 0;
    g_prompt_cb_priv = NULL;
    
    g_stream_audio_on = false;
    g_stream_audio_playing = false;
    g_stream_audio_thread_running = false;
    
    g_volume = G_MAX_VOLUME;
}

/**
 * @brief 反初始化统一音频管理模块
 */
void lwl_app_audio_uninit(void)
{
    log_i("[AUDIO]Uninit unified audio management module");
    
    // 停止所有音频播放
    lwl_app_audio_stop_playing();
    lwl_app_audio_stream_stop();
    
    os_mutex_pend(&g_audio_mutex, 0);
    os_mutex_post(&g_audio_mutex);
    
    os_mutex_del(&g_audio_mutex, 1);
}
