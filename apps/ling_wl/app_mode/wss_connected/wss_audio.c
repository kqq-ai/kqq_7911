#include "wss_audio.h"
#include "wss_app_util.h"
#include "protocol/odm/odmp.h"

#include "bt_ble/client/ble_distance.h"
#include "app_mode/lwl_app_core.h"
#include "gpio.h"
#include "util/threadpool_tasks.h"

#include "LED/LED.h"
#include "os/os_api.h"

#define TIMEOUT_MS 5000 // 5秒超时

static lwl_app_context_t *g_app_ctx = NULL;
static OS_MUTEX g_audio_mutex;

static void set_led_tts_on(void)    { set_rgb_ON(COLOR_BLUE,3); }
static void set_led_tts_off(void)   { set_rgb_OFF(COLOR_BLUE,3); }

/*=================================================================================================
 * Audio dec app context
 *=================================================================================================*/
static int g_audio_on = false; // 音频播放结束标志（外部请求状态）
static int g_audio_playing = false; // 音频播放中标志（播放线程状态）
static int g_audio_thread_running = false; // 播放线程运行标志

#define G_MAX_VOLUME 100
int g_volume = G_MAX_VOLUME;
// 设置播放器音量(1~70)
void set_tts_volume(int volume) {
    if (volume < 1) volume = 1;
    if (volume > 100) volume = 100;
    g_volume = volume * G_MAX_VOLUME / 100; // 保护E的电池，需要设置最高默认音量为 G_MAX_VOLUME
}

void set_speaker() {
    log_i("[AUDIO]set_speaker Pi: %d", g_volume);
    os_mutex_pend(&g_audio_mutex, 0);
    lwl_app_mode_stop_playing();
    gpio_direction_output(IO_PORTA_00, 0);  //Disable C speaker
    gpio_direction_output(IO_PORTB_02, 1);  //Enable E speaker
    set_dec_volume(g_app_ctx->music_player, g_volume);
    bdo_enable();
    os_mutex_post(&g_audio_mutex);
}

//=================================================================================================
// MUSIC response callback
//=================================================================================================
void app_music_play_end_cb(void *priv)
{
    log_i("[AUDIO]app_music_play_end_cb");
    g_audio_playing = false;
    g_audio_on = false;
    set_led_tts_off();
}

/**
 * @brief 音频播放线程函数
 * @details
 *   循环从stream缓冲队列中获取数据并写入播放器，直到stream结束且队列为空。
 *   如果5秒内没有收到音频数据则自动退出。
 */
const uint32_t MAX_CHUNK_SIZE = 16000;
uint8_t buffer[MAX_CHUNK_SIZE];
static void wss_audio_playback_thread(void *priv)
{
    uint32_t last_data_time = timer_get_ms();
    
    log_i("[AUDIO]playback thread started");
    
    while (g_audio_thread_running) {
        
        /* 从stream获取数据 */
        uint32_t read_len = wss_stream_get_data(buffer, MAX_CHUNK_SIZE);
        
        if (read_len > 0) {
            /* 有数据，写入播放器 */
            last_data_time = timer_get_ms(); // 更新最后数据时间
            os_mutex_pend(&g_audio_mutex, 0);
            
            if (!g_audio_on) {
                /* 第一次播放，启动播放器 */
                set_speaker();
                app_music_play_stream_music_with_callback(g_app_ctx->music_player, app_music_play_end_cb, NULL);
                g_audio_on = true;
                set_led_tts_on();
                log_i("[AUDIO]playback started");
            }
            
            uint32_t written = app_music_play_stream_write(g_app_ctx->music_player, buffer, read_len);
            if (written > 0) {
                log_d("[AUDIO]stream_write %u bytes", written);
            } else {
                log_e("[AUDIO]failed to write stream");
            }
            
            os_mutex_post(&g_audio_mutex);
        } else if (!g_audio_on) {
            /* 无数据，检查是否stream已结束或超时 */
            if (wss_stream_is_empty()) {
                log_i("[AUDIO]stream finish");
                break;
            }
            msleep(20);
        } else if (!g_audio_playing) {
            /* 检查超时 */
            if (app_music_play_get_status(g_app_ctx->music_player) == AUDIO_DEC_START) {
                g_audio_playing = true;
            } else {
                uint32_t elapsed = timer_get_ms() - last_data_time;
                if (elapsed > TIMEOUT_MS) {
                    log_w("[AUDIO]timeout: no data for %u ms, exiting", elapsed);
                    break;
                }
            }
            msleep(50);
        }
    }

    if (g_audio_on) {
        app_music_play_stop(g_app_ctx->music_player);
        g_audio_playing = false;
        g_audio_on = false;
        set_led_tts_off();
        log_i("[AUDIO]audio playback stopped");
    }
    g_audio_thread_running = false;
    
    log_i("[AUDIO]playback thread exited");
}

// 获取当前音频播放状态
bool is_audio_playing()
{
    return g_audio_thread_running || g_audio_on;
}

/**
 * @brief 启动音频播放线程
 * @details
 *   初始化播放线程，该线程会循环调用wss_stream_get_data获取音频数据并写入播放器。
 */
static void wss_audio_playback_start(void)
{
    if (g_audio_thread_running) {
        log_w("[AUDIO]playback thread already running");
        return;
    }
    
    log_i("[AUDIO]starting playback thread");
    g_audio_thread_running = true;
    
    /* 启动播放线程 */
    threadpool_add_task(g_app_ctx->threadpool, wss_audio_playback_thread, NULL);
}

// 停止蓝牙音频播放，关闭音频数据流，释放播放器资源
void wss_ble_audio_stop() {
    log_i("[AUDIO]wss_ble_audio_stop");
    log_mqtt("💔💔💔 停止蓝牙音频播放");
    
    wss_stream_reset();
    os_mutex_pend(&g_audio_mutex, 0);
    g_audio_on = false;
    g_audio_thread_running = false;
    os_mutex_post(&g_audio_mutex);
    
    /* 等待播放线程退出 */
    msleep(100);
    
    os_mutex_pend(&g_audio_mutex, 0);
    if (g_audio_on) {
        set_led_tts_off();
        g_audio_on = false;
        app_music_play_stop(g_app_ctx->music_player);
        log_i("[AUDIO]audio playback stopped");
    }
    os_mutex_post(&g_audio_mutex);
    
    send_device_status(DEVICE_STATUS_BLE_AUDIO_STOP);
}

// 开始蓝牙音频，启动播放线程，循环获取stream数据进行播放
void wss_ble_audio_start() {
    log_i("[AUDIO]wss_ble_audio_start");
    log_mqtt("❤️‍🔥❤️‍🔥❤️‍🔥 开始蓝牙音频播放");
    
    os_mutex_pend(&g_audio_mutex, 0);
    g_audio_on = false;
    g_audio_playing = false;
    os_mutex_post(&g_audio_mutex);
    
    /* 启动播放线程 */
    wss_audio_playback_start();
}

void wss_audio_init()
{
    log_i("[AUDIO]Init WSS audio\n");
    if (!os_mutex_valid(&g_audio_mutex)) {
        os_mutex_create(&g_audio_mutex);
    }

    g_app_ctx = lwl_app_core_context_get_instance();
    lwl_peri_dev_7016_on(true);  // 开启 7016
}

void wss_audio_uninit()
{
    log_i("[AUDIO]Uninit WSS audio\n");
    
    wss_ble_audio_stop(); // 停止音频播放
    
    os_mutex_pend(&g_audio_mutex, 0);
    lwl_peri_dev_7016_on(false);  // 关闭 7016
    os_mutex_post(&g_audio_mutex);
}
