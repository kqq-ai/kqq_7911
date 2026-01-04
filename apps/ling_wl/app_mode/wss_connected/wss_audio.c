#include "wss_audio.h"
#include "wss_app_util.h"
#include "protocol/odm/odmp.h"

#include "bt_ble/client/ble_distance.h"
#include "app_mode/lwl_app_core.h"
#include "gpio.h"
#include "util/threadpool_tasks.h"

#include "LED/LED.h"

#include "os/os_api.h"

static lwl_app_context_t *g_app_ctx = NULL;
static OS_MUTEX g_audio_mutex;

static void set_led_tts_on(void)    { set_rgb_ON(COLOR_BLUE,3); }
static void set_led_tts_off(void)   { set_rgb_OFF(COLOR_BLUE,3); }

u32 audio_req_time = 0;
uint32_t total_written = 0;

/*=================================================================================================
 * Audio dec app context
 *=================================================================================================*/
static int g_audio_off = true; // 音频播放结束标志（外部请求状态）
static int g_audio_play_ongoing = false; // 音频启动中标志（播放器状态）
static int g_audio_playing = false; // 音频播放中标志（播放器状态）
static int g_audio_player_start = false; // 音频播放中标志（播放器状态）

#define PREBUFFER_SIZE        (100 * 1024) /* 100KB */
#define PREBUFFER_TIMEOUT_MS  1000         /* 最长等待时间，超时仍开始播放 */

static uint8_t g_prebuffer[PREBUFFER_SIZE];
static uint32_t g_prebuffer_pos = 0;
static bool g_stream_end = false; // 音频流结束标志

static uint32_t odmp_audio_data_cbk(uint8_t *data, uint32_t len);

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
    g_audio_player_start = false;
    set_led_tts_off();
}

static void wss_audio_playback_start(void);
// 允许网络音频开始播放
void wss_audio_allow_playback()
{
    log_i("[AUDIO]starting music player %d %d", g_audio_play_ongoing, g_audio_player_start);
    os_mutex_pend(&g_audio_mutex, 0);
    if (g_audio_play_ongoing) {
        log_w("[AUDIO]playback already started, ignoring");
        os_mutex_post(&g_audio_mutex);
        return;
    }

    /* 停止其他音频 */
    lwl_app_mode_stop_playing();
    set_speaker();

    /* 标记为播放中 */
    audio_req_time = timer_get_ms();
    g_audio_play_ongoing = true;
    set_led_tts_on();

    
    wss_audio_playback_start();
    os_mutex_post(&g_audio_mutex);
}

/**
 * @brief 启动音频播放并将预缓冲数据刷入播放器
 * @details
 *   由外部（如tts_bind回调或应用层）显式调用，无内部计时逻辑。
 *   启动播放器并将已缓冲的音频数据分片写入。
 * @return 0 成功，-1 失败
 */
static void wss_audio_playback_start(void)
{
    const uint32_t MAX_CHUNK_SIZE = 16000;
    if (g_audio_player_start) return;
    g_audio_player_start = true;

    log_i("[AUDIO]playback_start: flushing %u bytes from prebuffer", g_prebuffer_pos);

    os_mutex_pend(&g_audio_mutex, 0);
    /* 启动播放器 */
    app_music_play_stream_music_with_callback(g_app_ctx->music_player, app_music_play_end_cb, NULL);
    

    if (g_prebuffer_pos == 0) {
        log_w("[AUDIO]no prebuffered data to flush");
        os_mutex_post(&g_audio_mutex);
        return;
    }

    /* 把预缓冲的数据分片写入播放器 */
    uint32_t written_all = 0;
    while (written_all < g_prebuffer_pos) {
        uint32_t chunk = (g_prebuffer_pos - written_all > MAX_CHUNK_SIZE) ? MAX_CHUNK_SIZE : (g_prebuffer_pos - written_all);
        uint32_t written = app_music_play_stream_write(g_app_ctx->music_player, g_prebuffer + written_all, chunk);
        if (written == 0) {
            log_e("[AUDIO]failed to write prebuffer chunk at offset %u", written_all);
            break;
        }
        written_all += written;
        log_d("[AUDIO]flushed prebuffer chunk %u/%u", written_all, g_prebuffer_pos);
        msleep(10);
    }

    /* reset prebuffer */
    g_prebuffer_pos = 0;
    os_mutex_post(&g_audio_mutex);

    log_i("[AUDIO]playback_start completed, flushed %u bytes", written_all);
    return;
}

/**
 * @brief 音频数据流写入（不包含播放启动逻辑）
 * @details
 *   接收音频数据并写入预缓冲或直接流入播放器（如果已启动）。
 *   无内部计时逻辑，仅负责数据缓冲/转发。
 * @param data 音频数据指针
 * @param len  音频数据长度
 * @return 0 成功，-1 失败
 */
static int wss_audio_stream_write(uint8_t *data, uint32_t len)
{
    const uint32_t MAX_CHUNK_SIZE = 16000;
    uint32_t idx = 0;

    if (!data || len == 0) {
        return 0;
    }

    if (g_audio_off) {
        return 0;
    }

    log_d("[AUDIO]stream_write len=%u, play_ongoing=%d, prebuffer_pos=%u", len, g_audio_play_ongoing, g_prebuffer_pos);

    os_mutex_pend(&g_audio_mutex, 0);

    /* 如果已启动播放，直接写入流 */
    if (g_audio_play_ongoing) {
        goto stream_write;
    }

    /* 未启动播放：写入预缓冲 */
    if (g_prebuffer && g_prebuffer_pos +len < PREBUFFER_SIZE) {
        memcpy(g_prebuffer + g_prebuffer_pos, data, len);
        g_prebuffer_pos += len;
        total_written += len;
        log_d("[AUDIO]prebuffered %u/%u bytes", g_prebuffer_pos, PREBUFFER_SIZE);
        os_mutex_post(&g_audio_mutex);
        return 0;
    } else {
        /* 预缓冲已满或未分配，但尚未启动播放 => 丢弃或缓存溢出 */
        log_w("[AUDIO]prebuffer full/unavailable but playback not started, dropping %u bytes", len);
        wss_audio_allow_playback();
        goto stream_write;
    }

stream_write:
    if (g_audio_player_start == false) {
        log_i("[AUDIO]stream_write: starting music player");
        wss_audio_playback_start(); // 尝试启动播放
        g_audio_player_start = true;
    }
    /* 直接写入播放器流（data/len 为需要写入的部分） */
    while (len > 0 && !g_audio_off) {
        uint32_t chunk_size = (len > MAX_CHUNK_SIZE) ? MAX_CHUNK_SIZE : len;
        uint32_t written = app_music_play_stream_write(g_app_ctx->music_player, data + idx, chunk_size);
        if (written == 0) {
            log_e("[AUDIO]failed to write stream chunk");
            break;
        }
        idx += written;
        total_written += written;
        log_d("[AUDIO]stream_write chunk: %u bytes (total: %u / %u)", written, idx, total_written);
        if (idx == len || g_audio_off) break;
        msleep(10);
    }

    /* update playing flag if started */
    if (g_audio_playing == false && app_music_play_get_status(g_app_ctx->music_player) == AUDIO_DEC_START) {
        g_audio_playing = true;
        log_i("[AUDIO]stream playing...");
        send_device_status(DEVICE_STATUS_AUDIO_PLAYING); // 正在播放
    }
    os_mutex_post(&g_audio_mutex);

    log_i("[AUDIO]stream_write completed: %u / %u bytes", idx, total_written);
    return 0;
}

/**
 * @brief 音频数据流回调（供ODM协议层注册）
 * @details
 *   此函数作为回调被协议层调用，负责将音频数据转发给 wss_audio_stream_write。
 *   保持向后兼容，但将实际逻辑委托给拆分后的函数。
 * @param data 音频数据指针
 * @param len  音频数据长度
 * @return 0 成功
 */
static uint32_t odmp_audio_data_cbk(uint8_t *data, uint32_t len)
{
    log_i("[AUDIO]odmp_audio_data_cbk len=%u", len);
    wss_audio_stream_write(data, len);
    return 0;
}

// 获取当前音频播放状态
bool is_audio_playing(){
    bool playing = g_audio_playing || (g_audio_play_ongoing && 
        (total_written >> 2) + 1000 > timer_get_ms()-audio_req_time); // 数据长度小于当前流逝时间，可能无法启动播放
    if (!g_audio_playing && playing) {
        log_d("[AUDIO]is_audio_playing total_written=%u time last %u", total_written, timer_get_ms()-audio_req_time);
    }
    return playing;
}

// 停止蓝牙音频播放，关闭音频数据流，释放播放器资源
void wss_ble_audio_stop() {
    log_i("[AUDIO]wss_ble_audio_stop %d %d", g_audio_play_ongoing, g_audio_player_start);
    log_mqtt("💔💔💔 停止蓝牙音频播放");
    odmp_register_audio_data_cbk(NULL); 
    os_mutex_pend(&g_audio_mutex, 0);
    g_audio_off = true;
    if (g_audio_play_ongoing) {
        set_led_tts_off();
        g_audio_play_ongoing = false; // 停止音频播放
        g_audio_playing = false;
        if (g_audio_player_start) {
            app_music_play_stop(g_app_ctx->music_player);
            g_audio_player_start = false;
        }
    }
    os_mutex_post(&g_audio_mutex);
    send_device_status(DEVICE_STATUS_BLE_AUDIO_STOP);
}

// 开始蓝牙音频，设置音频数据回调，有数据时会启动播放器
void wss_ble_audio_start() {
    log_i("[AUDIO]wss_ble_audio_start %d %d", g_audio_play_ongoing, g_audio_player_start);
    log_mqtt("❤️‍🔥❤️‍🔥❤️‍🔥 开始蓝牙音频播放");
    os_mutex_pend(&g_audio_mutex, 0);
    g_audio_off = false;
    g_audio_player_start = false;
    /* reset prebuffer */
    total_written = 0;
    g_prebuffer_pos = 0;
    g_stream_end = false;
    os_mutex_post(&g_audio_mutex);
}

void wss_audio_set_stream_start() {
    log_i("[AUDIO]wss_audio_set_stream_start");
    g_stream_end = false;
    odmp_register_audio_data_cbk(odmp_audio_data_cbk); 
}

void wss_audio_set_stream_end() {
    log_i("[AUDIO]wss_audio_set_stream_end");
    odmp_register_audio_data_cbk(NULL); 
    if (total_written > 0) {
        log_i("[AUDIO]total_written=%u marking stream end", total_written);
        g_stream_end = true;
    }
}

bool wss_audio_is_stream_end() {
    bool ended;
    os_mutex_pend(&g_audio_mutex, 0);
    ended = g_stream_end;
    os_mutex_post(&g_audio_mutex);
    return ended;
}

void wss_audio_init()
{
    log_i("[AUDIO]Init WSS audio\n");
    if (!os_mutex_valid(&g_audio_mutex)) {
        os_mutex_create(&g_audio_mutex);
    }

    g_app_ctx = lwl_app_core_context_get_instance();
    lwl_peri_dev_7016_on(true);  // 开启 7016

    /* UART指令开启C端le audio，并指定监听的 mac 地址*/
    uart_send_open_le_audio(g_app_ctx->mac);
}

void wss_audio_uninit()
{
    log_i("[AUDIO]Uninit WSS audio\n");
    os_mutex_pend(&g_audio_mutex, 0);
    
    lwl_peri_dev_7016_on(false);  // 关闭 7016
    wss_ble_audio_stop(); // 停止音频播放

    os_mutex_post(&g_audio_mutex);
}
