#include "wss_audio.h"
#include "wss_app_util.h"
#include "cpu.h"
#include "app_cookie.h"

#include "util/cqueue.h"
#include "util/ringbuffer.h"
#include "bt_ble/client/ble_distance.h"
#include "app_mode/lwl_app_core.h"
#include "gpio.h"
#include "device.h"
#include "util/threadpool_tasks.h"

#include "ota/http_ota.h"
#include "uart.h"

#include "protocol/odm/odmp.h"

#include "protocol/odm/odm_secure.h"
#include "protocol/cp/c_uart_p.h"

#include "os/os_api.h"

#define TOUCH_KEY_HEAT_TIMEOUT_MS 500
#define VAD_START_HEAT_TIMEOUT_MS 100

static lwl_app_context_t *g_app_ctx = NULL;
static OS_MUTEX g_audio_mutex;

static void set_led_mic_on(void)        { uart_send_set_led_color(COLOR_ORANGE); }  //麦克风开启时灯光变橙色
static void set_led_asr_upload(void)    { uart_send_set_led_color(COLOR_GREEN); }   //音频上传时灯光变绿色
static void set_led_tts_play(void)      { uart_send_set_led_color(COLOR_BLUE); }    //音频下载时灯光变蓝色
static void set_led_idle(void)          { uart_send_set_led_color(COLOR_WHITE); }   //闲置时灯光变白色

/*=================================================================================================
 * Vad timer
 *=================================================================================================*/
u32 vad_start_time = 0;
u32 vad_stop_time = 0;  // 0表示未设置，超时停止VAD
u32 audio_req_time = 0;
u32 audio_start_time = 0;
uint32_t total_written = 0;

u32 get_time_stamp() {
    return timer_get_ms();
}

/*=================================================================================================
 * Audio dec app context
 *=================================================================================================*/
static int g_audio_off = true; // 音频播放结束标志（外部请求状态）
static int g_audio_play_ongoing = false; // 音频启动中标志（播放器状态）
static int g_audio_playing = false; // 音频播放中标志（播放器状态）
/*=================================================================================================
 * Record app context
 *=================================================================================================*/
static int g_recording = false;             // ASR强制上传标志忽略VAD，优先级更高
static int g_recording_vad = false;         // VAD状态标志（根据麦克风数据判断），发送ASR结束（1，2）
static int g_recording_upload = false;      // VAD开关录音上传状态标志（WSS 状态），发送ASR异常（1，3）
static int g_recording_uploading = false;   // 录音上传状态标志（WSS 状态）

/*=================================================================================================
 * Audio enc app context
 *=================================================================================================*/
#define ASR_DATA_CHUNK_SIZE 144
typedef struct {
    u8 data[ASR_DATA_CHUNK_SIZE];
} AsrData;
static AsrData asr_data_frame;

#define MY_ASR_DATA_QUEUE_CAPACITY 80
static MyBlockingQueue g_asr_data_queue; // MyBlockingQueue<AsrData>
static u8 g_asr_data_queue_buffer[MY_ASR_DATA_QUEUE_CAPACITY * sizeof(AsrData)];

#define MY_ASR_RING_BUFFER_CAPACITY 20
static MyRingBuffer g_asr_ringbuffer;    // MyRingBuffer<AsrData>
static u8 g_asr_ringbuffer_buffer[MY_ASR_RING_BUFFER_CAPACITY * sizeof(AsrData)];

#define PREBUFFER_SIZE        (100 * 1024) /* 100KB */
#define PREBUFFER_TIMEOUT_MS  1000         /* 最长等待时间，超时仍开始播放 */

static uint8_t g_prebuffer[PREBUFFER_SIZE];
static uint32_t g_prebuffer_pos = 0;
static bool g_prebuffer_active = false;

static uint32_t odmp_audio_data_cbk(uint8_t *data, uint32_t len);
void wait_vad_task(void *priv);
void wait_asr_task(void *priv);

/**
 * @name wss_app_recorder_en
 * @brief 开启/停止 录音编码器
 * @details 编码器默认打开VAD，可通过recorder_reg_vad_callback注册VAD回调
 *   停止编码器后，VAD将不再触发，直到重新开启
 */
#define ENABLE 1
#define DISABLE 0
void wss_app_recorder_en(u8 en)
{
    lwl_app_context_t *context = lwl_app_core_context_get_instance();
    if (context && context->recorder) {
        if (en && recorder_get_play_pause_state(context->recorder) == 0) {
            recorder_start(context->recorder); /* 开启录音编码 */
        } else if(!en && recorder_get_play_pause_state(context->recorder) == 1) {
            recorder_stop(context->recorder); /* 停止录音编码 */
        }
    }
}

/**
 * @name wss_app_recorder_data_upstream
 * @brief 设置 录音数据回调
 * @details 设置后所有录音数据将会存入g_asr_data_queue
 *   设置后，应用程序需要手动监听缓冲数据并取出处理
 */
static void wss_app_recorder_data_upstream(recorder_data_cb_t cb)
{
    lwl_app_context_t *context = lwl_app_core_context_get_instance();
    if (context && context->recorder) {
        recorder_set_data_cb(context->recorder, cb, NULL); /* 设置录音上传回调 */
    }
}

void music_play_stop()
{
    if (app_music_play_get_status(g_app_ctx->music_player) == AUDIO_DEC_START) {
        log_i("[AUDIO]music_play_stop");
        app_music_play_stop(g_app_ctx->music_player);
    }
}

// 用一个新线程停止音频播放，避免阻塞主线程
void music_play_stop_in_thread()
{
    if (app_music_play_get_status(g_app_ctx->music_player) == AUDIO_DEC_START) {
        log_i("[AUDIO]music_play_stop_in_thread");
        threadpool_add_task(g_app_ctx->threadpool, music_play_stop, NULL);
    }
}

// 获取当前音频上传状态
bool is_recording_uploading() {
    os_mutex_pend(&g_audio_mutex, 0);
    bool uploading = (g_recording_uploading == true);
    os_mutex_post(&g_audio_mutex);
    return uploading;
}

// 获取当前麦克风状态
static bool is_mic_on() {
    lwl_app_context_t *context = lwl_app_core_context_get_instance();
    bool recording = (recorder_get_play_pause_state(context->recorder) == 1);
    return recording;
}

// 停止麦克风录音，可选择是否上传剩余音频数据
void wss_mic_stop() {
    log_i("[AUDIO]wss_mic_stop");
    os_mutex_pend(&g_audio_mutex, 0);
    wss_app_recorder_data_upstream(NULL);
    wss_app_recorder_en(DISABLE);
    os_mutex_post(&g_audio_mutex);
}

// 开始麦克风录音，设置VAD超时时间
void wss_mic_start() {
    log_i("[AUDIO]wss_mic_start");
    os_mutex_pend(&g_audio_mutex, 0);
    wss_app_recorder_en(ENABLE);
    vad_start_time = get_time_stamp();
    os_mutex_post(&g_audio_mutex);
}

// 开始麦克风录音，设置VAD超时时间
void wss_vad_start(uint timestamp) {
    log_i("[AUDIO]wss_vad_start set timeout %d", timestamp);
    os_mutex_pend(&g_audio_mutex, 0);
    if (vad_stop_time == 0 && !g_recording) {
        g_recording_upload = true;
        vad_stop_time = get_time_stamp() + timestamp; // 设置VAD超时时间
        threadpool_add_task(g_app_ctx->threadpool, wait_vad_task, NULL);
    } else if (vad_stop_time != 0) {
        vad_stop_time = get_time_stamp() + timestamp; // 设置VAD超时时间
    }
    os_mutex_post(&g_audio_mutex);
}

// 开始麦克风录音，准备上传ASR
void wss_asr_start() {
    log_i("[AUDIO]wss_asr_start");
    os_mutex_pend(&g_audio_mutex, 0);
    if (!g_recording) {
        g_recording_upload = false; // 停止VAD上传
        g_recording = true;
        threadpool_add_task(g_app_ctx->threadpool, wait_asr_task, NULL);
    }
    os_mutex_post(&g_audio_mutex);
}

// 取消上传ASR
void wss_asr_stop() {
    os_mutex_pend(&g_audio_mutex, 0);
    g_recording = false;
    os_mutex_post(&g_audio_mutex);
}

#define G_MAX_VOLUME 100
int g_volume = G_MAX_VOLUME;
// 设置播放器音量(1~70)
void set_tts_volume(int volume) {
    if (volume < 1) volume = 1;
    if (volume > 100) volume = 100;
    g_volume = volume * G_MAX_VOLUME / 100; // 保护E的电池，需要设置最高默认音量为 G_MAX_VOLUME
}

bool c_speaker_on = false;
// 设置C音箱开关
void set_C_speaker_on(bool on) {
    log_i("[AUDIO]set_C_speaker_on: %d", on);
    os_mutex_pend(&g_audio_mutex, 0);
    c_speaker_on = on;
    os_mutex_post(&g_audio_mutex);
}

void set_speaker() {
    log_i("[AUDIO]set_speaker Pi: %d Cookie: %d", g_volume, c_speaker_on);
    os_mutex_pend(&g_audio_mutex, 0);
    gpio_direction_output(IO_PORTA_00, c_speaker_on);  //Disable C speaker
    gpio_direction_output(IO_PORTB_02, 1);  //Enable E speaker
    set_dec_volume(g_app_ctx->music_player, g_volume);
    os_mutex_post(&g_audio_mutex);
}

void wss_audio_set_player(u32 delta) {
    audio_start_time = get_time_stamp()+delta;
}

// 获取当前音频播放状态
bool is_audio_playing(){
    bool playing = g_audio_playing || (g_audio_play_ongoing && 
        (total_written >> 2) + 1000 > get_time_stamp()-audio_req_time); // 数据长度小于当前流逝时间，可能无法启动播放
    return playing;
}

// 停止蓝牙音频播放，关闭音频数据流，释放播放器资源
void wss_ble_audio_stop() {
    log_i("[AUDIO]wss_ble_audio_stop");
    os_mutex_pend(&g_audio_mutex, 0);
    g_audio_off = true;
    odmp_register_audio_data_cbk(NULL); 
    if (g_audio_play_ongoing) {
        g_audio_play_ongoing = false; // 停止音频播放
        g_audio_playing = false;
        music_play_stop();
    }
    os_mutex_post(&g_audio_mutex);
}

// 开始蓝牙音频，设置音频数据回调，有数据时会启动播放器
void wss_ble_audio_start() {
    log_i("[AUDIO]wss_ble_audio_start");
    os_mutex_pend(&g_audio_mutex, 0);
    g_audio_off = false;
    /* reset prebuffer */
    total_written = 0;
    g_prebuffer_pos = 0;
    g_prebuffer_active = false;
    odmp_register_audio_data_cbk(odmp_audio_data_cbk); 
    os_mutex_post(&g_audio_mutex);
}

//=================================================================================================
// ASR data queue
//=================================================================================================
// 录音数据回调，写入 g_asr_data_queue
static void recorder_data_to_data_queue(void *priv, void *data, u32 size)
{
    ASSERT(size == ASR_DATA_CHUNK_SIZE);

    AsrData *asr_data = (AsrData *)data;
    int enqueue_ret = my_blocking_queue_try_enqueue(&g_asr_data_queue, asr_data);
    if (enqueue_ret == MYERROR_Is_Full) {
        AsrData dropped;
        int drop_count = 0;
        while (drop_count < 4 &&
               MYERROR_Success == my_blocking_queue_try_dequeue(&g_asr_data_queue, &dropped)) {
            drop_count++;
        }
        log_i("[AUDIO]asr queue overflow: dropped %d frames (%d/%d).",
              drop_count,
              my_blocking_queue_size(&g_asr_data_queue),
              MY_ASR_DATA_QUEUE_CAPACITY);
        enqueue_ret = my_blocking_queue_try_enqueue(&g_asr_data_queue, asr_data);
        if (enqueue_ret == MYERROR_Is_Full) {
            log_i("[AUDIO]asr data queue still full, discard incoming frame.");
        }
    } else if (enqueue_ret == MYERROR_Success) {
        int curr = my_blocking_queue_size(&g_asr_data_queue);
        if (curr >= (MY_ASR_DATA_QUEUE_CAPACITY * 3) / 4) {
            log_i("[AUDIO]asr queue high water mark %d/%d.", curr, MY_ASR_DATA_QUEUE_CAPACITY);
        }
    }
}

// 录音数据回调，写入循环队列缓存
static void recorder_data_to_ringbuffer(void *priv, void *data, u32 size)
{
    ASSERT(size == ASR_DATA_CHUNK_SIZE);

    AsrData *asr_data = (AsrData *)data;
    my_ring_buffer_write(&g_asr_ringbuffer, asr_data);
}

// 将ASR数据发送到服务器
static int wss_asr_data_to_server(AsrData *asr_data)
{
    uint8_t *pdata = (uint8_t *)asr_data;
    return send_bin_data(pdata, ASR_DATA_CHUNK_SIZE); // 修改: 返回 send_bin_data 的结果 (0成功, -1失败)
}

/**
 * @brief 音频上传主循环任务（VAD触发后调用）
 * @details
 *   1. 设置录音数据回调为 recorder_data_to_data_queue（数据进入 g_asr_data_queue）
 *   2. 优先上传 ringbuffer 中残留数据，再上传 data queue 数据
 *   3. 若检测到 g_recording_vad == false（VAD结束），
 *      - 切回 ringbuffer 回调
 *      - 发送音频结束状态
 *      - 设置 LED
 *      - 投递 wait_vad_task 任务
 *   4. 每轮循环 sleep 10ms
 * @param priv 未使用
 */
static void start_audio_upload()
{
    log_i("[AUDIO]Start audio upload.");
    set_led_asr_upload(); /* 音频上传时灯光变绿色 */
    my_blocking_queue_clear(&g_asr_data_queue); /* 清空 g_asr_data_queue */
    wss_app_recorder_data_upstream(recorder_data_to_data_queue); /* 录音数据 -> asr_data_queue */
    g_recording_uploading = true;

    while (is_mic_on() && g_recording_vad && g_recording_upload) {
        int ret = 0;
        /* 从ASR缓冲取数据发送给服务器，先将ringbuffer中的数据上传，再处理上传data queue中的数据 */
        if (MYRBERROR_Success == my_ring_buffer_read(&g_asr_ringbuffer, &asr_data_frame)) {
            ret = wss_asr_data_to_server(&asr_data_frame);
        } else if (MYERROR_Success == my_blocking_queue_try_dequeue(&g_asr_data_queue, &asr_data_frame)) {
            ret = wss_asr_data_to_server(&asr_data_frame);
        }
        
        // 修改: 如果发送失败(连接断开)，退出循环
        if (ret != 0) {
            log_i("[AUDIO]Upload failed (ret=%d), break loop.", ret);
            g_recording_upload = false; // 停止上传标志
            break;
        }

        msleep(10);
    }
    g_recording_uploading = false;
    wss_app_recorder_data_upstream(recorder_data_to_ringbuffer);
    if (g_recording_vad == false){/* VAD结束 */
        log_i("[AUDIO]Stop audio_upload because VAD END.");
    } else if (g_recording_upload == false) {
        log_i("[AUDIO]Stop audio_upload because ASR start.");
    } else {
        log_i("[AUDIO]Stop audio_upload because wss_mic_stop.");
    }
    send_audio_status(OAST_ASR, OASE_END);
}

/**
 * @brief 等待VAD触发的独立线程
 * @details
 *   1. 等待 vad_stop_time 毫秒内，如果检测到VAD有响应，则重新开始音频上传
 *   2. 等待超时后，关闭录音，重置相关状态
 */
void wait_vad_task(void *priv)
{
    log_i("[AUDIO]start VAD until %d\n", vad_stop_time);
    while (is_mic_on() == false && g_recording_upload) {
        msleep(30);
    }
    if (g_recording_upload) {
        // my_blocking_queue_clear(&g_asr_ringbuffer); /* 清空 g_asr_data_queue */
        wss_app_recorder_data_upstream(recorder_data_to_ringbuffer); /* 录音数据 -> asr_ringbuffer */
        set_led_mic_on(); // 开启录音时灯光变橙色
        while (vad_start_time + VAD_START_THRESHOLD + VAD_START_HEAT_TIMEOUT_MS > get_time_stamp() && g_recording_upload) {
            msleep(20);
        }

        while (get_time_stamp() < vad_stop_time && g_recording_upload && is_mic_on()) {
            msleep(10);
            if (g_recording_vad == true) {
                if (send_audio_status(OAST_ASR, OASE_START) == -1) {
                    break;
                }
                start_audio_upload();
                set_led_mic_on(); // 开启录音时灯光变橙色
            }
        }
        set_led_idle();
        wss_app_recorder_data_upstream(NULL);
        if (!is_mic_on()) {
            log_i("[AUDIO]Stop VAD because wss_mic_stop.");
        } else if (!g_recording_upload) {
            log_i("[AUDIO]Stop VAD because ASR start.");
        } else {
            log_i("[AUDIO]Stop VAD timeout at %d > %d.", get_time_stamp(), vad_stop_time);
        }
    }
    vad_stop_time = 0;
}

/**
 * @brief 等待按键触发ASR上传的独立线程
 * @details
 *   1. 延时500ms，未收到结束请求，则上传音频ASR
 *   2. 
 */
void wait_asr_task(void *priv)
{
    my_blocking_queue_clear(&g_asr_ringbuffer); /* 清空 g_asr_data_queue */
    wss_app_recorder_data_upstream(recorder_data_to_ringbuffer); /* 录音数据 -> asr_ringbuffer */
    my_blocking_queue_clear(&g_asr_data_queue); /* 清空 g_asr_data_queue */
    set_led_mic_on(); // 开启录音时灯光变橙色
    u32 asr_start_time = get_time_stamp();
    log_i("[AUDIO]start ASR task %d\n", asr_start_time);
    while ((asr_start_time + TOUCH_KEY_HEAT_TIMEOUT_MS > get_time_stamp() || is_mic_on() == false) && g_recording) {
        msleep(20);
    }

    if (g_recording) {
        log_i("[AUDIO]ASR upload started\n");
        send_audio_status(OAST_STREAM_AUDIO, OASE_START);
        set_led_asr_upload(); /* 音频上传时灯光变绿色 */
        wss_app_recorder_data_upstream(recorder_data_to_data_queue); /* 录音数据 -> asr_data_queue */
        g_recording_uploading = true;
    }

    while (is_mic_on() && g_recording) {
        int ret = 0;
        /* 从ASR缓冲取数据发送给服务器，先将ringbuffer中的数据上传，再处理上传data queue中的数据 */
        if (MYRBERROR_Success == my_ring_buffer_read(&g_asr_ringbuffer, &asr_data_frame)) {
            ret = wss_asr_data_to_server(&asr_data_frame);
        } else if (MYERROR_Success == my_blocking_queue_try_dequeue(&g_asr_data_queue, &asr_data_frame)) {
            ret = wss_asr_data_to_server(&asr_data_frame);
        }
        
        // 修改: 如果发送失败，退出循环
        if (ret != 0) {
            log_i("[AUDIO]ASR Upload failed, break loop.");
            break;
        }
        msleep(10);
    }

    if (g_recording_uploading) {
        g_recording_uploading = false;
        send_audio_status(OAST_STREAM_AUDIO, OASE_END);
        wss_app_recorder_data_upstream(NULL);
        set_led_idle();
    }
}

//=================================================================================================
// MUSIC response callback
//=================================================================================================
void app_music_play_end_cb(void *priv)
{
    log_i("[AUDIO]app_music_play_end_cb");
    os_mutex_pend(&g_audio_mutex, 0);
    g_audio_play_ongoing = false; // 音乐播放结束标志
    g_audio_playing = false;
    set_led_idle(); /* 音频播放结束时灯光变白色 */
    os_mutex_post(&g_audio_mutex);
}

/**
 * @brief 音频数据流回调（供ODM协议层注册）
 * @details
 *   当收到音频数据流（如TTS、ASR等）时，通过此回调将数据分片写入播放器进行流式播放。
 *   - data: 指向音频数据的指针
 *   - len:  音频数据长度（字节）
 *   内部会自动分片写入播放器，支持大数据分段处理。
 * @param data 音频数据指针
 * @param len  音频数据长度
 * @return 总是返回0（可根据需要返回写入状态）
 */
static uint32_t odmp_audio_data_cbk(uint8_t *data, uint32_t len)
{
    const uint32_t MAX_CHUNK_SIZE = 16000;
    uint32_t idx = 0;

    if (g_audio_off) {
        return 0;
    }
    log_i("[AUDIO]odmp_audio_data_cbk len=%u", len);

    /* 快速路径：如果已经进入播放并且正在写流，直接写入流（按原实现） */
    if (g_audio_play_ongoing) {
        goto stream_write;
    }

    /* 未进入播放：使用预缓冲逻辑 */
    if (g_prebuffer && g_prebuffer_pos < PREBUFFER_SIZE) {
        /* 开始预缓冲计时（第一次写入） */
        if (!g_prebuffer_active) {
            g_prebuffer_active = true;
            g_prebuffer_pos = 0;
            log_i("[AUDIO]prebuffer start");
        }

        /* 把数据尽可能写入预缓冲（若超过则截断剩余写入流启动阶段） */
        uint32_t copy_len = (len <= (PREBUFFER_SIZE - g_prebuffer_pos)) ? len : (PREBUFFER_SIZE - g_prebuffer_pos);
        memcpy(g_prebuffer + g_prebuffer_pos, data, copy_len);
        g_prebuffer_pos += copy_len;
        total_written += copy_len; /* 统计缓存到的字节也计入 total_written */
        log_d("[AUDIO]prebuffered %u/%u bytes", g_prebuffer_pos, PREBUFFER_SIZE);

        /* 如果已达阈值，开始播放并把预缓冲数据刷入播放器 */
        if (g_prebuffer_pos >= PREBUFFER_SIZE || (get_time_stamp() > audio_start_time)) {
            log_i("[AUDIO]prebuffer ready (pos=%u), start playback", g_prebuffer_pos);
            /* 准备播放：启动播放器 */
            music_play_stop();
            stop_prompt();
            g_audio_play_ongoing = true;
            set_led_tts_play();
            audio_req_time = get_time_stamp();

            app_music_play_stream_music_with_callback(g_app_ctx->music_player, app_music_play_end_cb, NULL);
            set_speaker();

            /* 把预缓冲的数据写入播放器流 */
            uint32_t written_all = 0;
            while (written_all < g_prebuffer_pos) {
                uint32_t chunk = (g_prebuffer_pos - written_all > MAX_CHUNK_SIZE) ? MAX_CHUNK_SIZE : (g_prebuffer_pos - written_all);
                uint32_t written = app_music_play_stream_write(g_app_ctx->music_player, g_prebuffer + written_all, chunk);
                if (written == 0) {
                    log_e("[AUDIO]Failed to write prebuffer chunk");
                    break;
                }
                written_all += written;
                log_d("[AUDIO]Flushed prebuffer chunk %u/%u", written_all, g_prebuffer_pos);
                /* small sleep to let player consume */
                msleep(10);
            }
            /* reset prebuffer */
            g_prebuffer_pos = 0;
            g_prebuffer_active = false;
        }

        /* 如果当前收到的数据还有剩余没有写入预buffer（被截断），把剩余交由流写入 */
        if (len > copy_len) {
            /* 调整 data/len 到剩余部分并走到流写路径 */
            data += copy_len;
            len -= copy_len;
            /* 继续到流写逻辑 */
            goto stream_write;
        } else {
            return 0;
        }
    } else {
        /* 没有预缓冲或已满但还未开始播放，直接尝试启动播放（回退策略） */
        if (!g_prebuffer) {
            log_w("[AUDIO]no prebuffer allocated, starting streaming immediately");
        } else {
            log_w("[AUDIO]prebuffer full but not started, force start");
        }
        return -1;
    }

stream_write:
    /* 这里 data/len 是需要直接写入播放器流的部分（被保护外部互斥） */
    while (len > 0 && !g_audio_off) {
        uint32_t chunk_size = (len > MAX_CHUNK_SIZE) ? MAX_CHUNK_SIZE : len;
        uint32_t written = app_music_play_stream_write(g_app_ctx->music_player, data + idx, chunk_size);
        if (written == 0) {
            log_e("Failed to write audio data chunk,odmp_audio_data_cbk");
            break;
        }
        idx += written;
        total_written += written;
        log_d("Written chunk: %u bytes (total: %u / %u)", written, len, total_written);
        if (idx == len || g_audio_off)  break;
        
        msleep(10);
    }
    if (g_audio_playing == false) {
        if (app_music_play_get_status(g_app_ctx->music_player) == AUDIO_DEC_START) {
            log_i("[AUDIO] start to play...");
            g_audio_playing = true;
        } else {
            log_i("[AUDIO] not playing yet...");
        }
    }
    log_i("[AUDIO] Total stream write: %u / %u bytes -> %u", idx, len, total_written);
    return 0;
}

/**
 * @brief 录音VAD（语音活动检测）回调
 * @details
 *   当录音模块检测到人声开始或结束时自动调用。
 *   - type == 1：检测到人声开始（VAD start），设置 g_recording_vad = true
 *   - type == 0：检测到人声结束（VAD stop），设置 g_recording_vad = false
 *   该回调用于驱动音频上传流程的启动与终止。
 * @param priv 未使用
 * @param type 1=人声开始，0=人声结束
 */
static void recorder_vad_cb(void* priv, int type)
{
    lwl_app_context_t *context = lwl_app_core_context_get_instance();
    
    switch (type) {
    case 0: /* VAD speaker stop */
        if (g_recording_vad == true) {
            g_recording_vad = false;
        }
        break;
    case 1: /* VAD speaker start */
        if (g_recording_vad == false) {
            g_recording_vad = true;
        }
        break;
    }
}

void wss_audio_init()
{
    log_i("[AUDIO]Init WSS audio\n");
    if (!os_mutex_valid(&g_audio_mutex)) {
        os_mutex_create(&g_audio_mutex);
    }
    my_blocking_queue_init_static(&g_asr_data_queue, g_asr_data_queue_buffer, sizeof(AsrData), MY_ASR_DATA_QUEUE_CAPACITY);
    my_ring_buffer_init_static(&g_asr_ringbuffer, g_asr_ringbuffer_buffer, sizeof(AsrData), MY_ASR_RING_BUFFER_CAPACITY);

    g_app_ctx = lwl_app_core_context_get_instance();
    lwl_peri_dev_7016_on(true);  // 开启 7016

    /* UART指令开启C端le audio，并指定监听的 mac 地址*/
    uart_send_open_le_audio(g_app_ctx->mac);
    wss_mic_start(); // 开启录音

    /* 设置VAD回调，触发音频上传 */
    recorder_set_vad_cbk(g_app_ctx->recorder, recorder_vad_cb, NULL);

    g_recording_upload = false;
    g_recording = false;

    /* 清除录音数据写入回调，默认不上传 */
    wss_app_recorder_data_upstream(NULL);
}

void wss_audio_uninit()
{
    log_i("[AUDIO]Uninit WSS audio\n");
    os_mutex_pend(&g_audio_mutex, 0);
    
    lwl_peri_dev_7016_on(false);  // 关闭 7016
    /* 取消录音上传回调 */
    wss_app_recorder_data_upstream(NULL);
    /* 取消VAD回调 */
    recorder_set_vad_cbk(g_app_ctx->recorder, NULL, NULL);

    wss_ble_audio_stop(); // 停止音频播放
    wss_mic_stop(); // 停止录音

    my_blocking_queue_destroy_static(&g_asr_data_queue);
    my_ring_buffer_destroy_static(&g_asr_ringbuffer);
    os_mutex_post(&g_audio_mutex);
}
