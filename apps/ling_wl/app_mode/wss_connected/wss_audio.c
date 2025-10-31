#include "wss_audio.h"
#include "wss_app_util.h"
#include "cpu.h"
#include "app_lingwl.h"

#include "util/cqueue.h"
#include "util/ringbuffer.h"
#include "bt_ble/client/ble_distance.h"
#include "async_msg_loop.h"
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

static lwl_app_context_t *g_app_ctx = NULL;
static OS_MUTEX g_audio_mutex;

static void set_led_mic_on(void)        { uart_send_set_led_color(COLOR_ORANGE); }  //麦克风开启时灯光变橙色
static void set_led_asr_upload(void)    { uart_send_set_led_color(COLOR_GREEN); }   //音频上传时灯光变绿色
static void set_led_idle(void)          { uart_send_set_led_color(COLOR_WHITE); }   //音频下载时灯光变白色

/*=================================================================================================
 * Vad timer
 *=================================================================================================*/
u32 vad_start_time = 0;
u32 vad_stop_time = 0;
uint32_t total_written = 0;
u32 audio_req_time = 0;

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
static int g_recording_vad = false; // VAD状态标志（根据麦克风数据判断）
static int g_recording_upload = false; // 录音上传状态标志（WSS 状态）
static int g_recording_uploading = false; // 录音上传状态标志（WSS 状态）

/*=================================================================================================
 * Audio enc app context
 *=================================================================================================*/
#define ASR_DATA_CHUNK_SIZE 144
typedef struct {
    u8 data[ASR_DATA_CHUNK_SIZE];
} AsrData;
static AsrData asr_data_frame;

#define MY_ASR_DATA_QUEUE_CAPACITY 20
static MyBlockingQueue g_asr_data_queue; // MyBlockingQueue<AsrData>
static u8 g_asr_data_queue_buffer[MY_ASR_DATA_QUEUE_CAPACITY * sizeof(AsrData)];

#define MY_ASR_RING_BUFFER_CAPACITY 10
static MyRingBuffer g_asr_ringbuffer;    // MyRingBuffer<AsrData>
static u8 g_asr_ringbuffer_buffer[MY_ASR_RING_BUFFER_CAPACITY * sizeof(AsrData)];


static uint32_t odmp_audio_data_cbk(uint8_t *data, uint32_t len);
void wait_vad_task(void *priv);

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

// 获取当前音频上传状态
bool is_recording_uploading() {
    os_mutex_pend(&g_audio_mutex, 0);
    bool uploading = (g_recording_uploading == true);
    os_mutex_post(&g_audio_mutex);
    return uploading;
}

// 获取当前麦克风状态
bool is_mic_on() {
    os_mutex_pend(&g_audio_mutex, 0);
    lwl_app_context_t *context = lwl_app_core_context_get_instance();
    bool recording = (recorder_get_play_pause_state(context->recorder) == 1);
    os_mutex_post(&g_audio_mutex);
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
    os_mutex_post(&g_audio_mutex);
}

// 停止麦克风录音，可选择是否上传剩余音频数据
void wss_vad_stop() {
    log_i("[AUDIO]wss_vad_stop");
    os_mutex_pend(&g_audio_mutex, 0);
    g_recording_upload = false; // 停止上传
    os_mutex_post(&g_audio_mutex);
}

// 开始麦克风录音，设置VAD超时时间
void wss_vad_start(uint timestamp) {
    os_mutex_pend(&g_audio_mutex, 0);
    log_i("[AUDIO]wss_vad_start set timeout %d", timestamp);
    g_recording_upload = true;
    vad_stop_time = timestamp; // 设置VAD超时时间
    if (vad_start_time == 0) {
        vad_start_time = get_time_stamp();
        threadpool_add_task(g_app_ctx->threadpool, wait_vad_task, NULL);
    }
    os_mutex_post(&g_audio_mutex);
}

#define G_MAX_VOLUME 70
// 设置播放器音量(1~70)
void set_Pi_volume(int volume) {
    if (volume < 1) volume = 1;
    if (volume > G_MAX_VOLUME) volume = G_MAX_VOLUME; // 保护E的电池，需要设置最高默认音量为 G_MAX_VOLUME = 70
    set_dec_volume(g_app_ctx->music_player, volume);
}

// 获取当前音频播放状态
bool is_audio_playing(){
    os_mutex_pend(&g_audio_mutex, 0);
    bool playing = g_audio_playing || (g_audio_play_ongoing && 
        total_written + 1000 > get_time_stamp()-audio_req_time); // 数据长度小于当前流逝时间，可能无法启动播放
    os_mutex_post(&g_audio_mutex);
    return playing;
}

// 停止蓝牙音频播放，并发送异常状态给服务器
void wss_ble_audio_stop() {
    os_mutex_pend(&g_audio_mutex, 0);
    if (g_audio_play_ongoing) {
        send_audio_status(OAST_TTS, OASE_ABNORMAL); // 通知TTS异常
        g_audio_off = true;
        g_audio_play_ongoing = false; // 停止音频播放
        app_music_play_stop(g_app_ctx->music_player);
    }
    os_mutex_post(&g_audio_mutex);
}

// 开始蓝牙音频播放
void wss_ble_audio_start() {
    os_mutex_pend(&g_audio_mutex, 0);
    g_audio_off = false;
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
    if (MYERROR_Is_Full == my_blocking_queue_try_enqueue(&g_asr_data_queue, asr_data)) {
        log_i("[AUDIO]asr data qeue full, failed to enquque asr data.");
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
static void wss_asr_data_to_server(AsrData *asr_data)
{
    uint8_t *pdata = (uint8_t *)asr_data;
    if (send_bin_data(pdata, ASR_DATA_CHUNK_SIZE)) {
    }
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
    while (is_mic_on()) {
        /* 从ASR缓冲取数据发送给服务器，先将ringbuffer中的数据上传，再处理上传data queue中的数据 */
        if (MYRBERROR_Success == my_ring_buffer_read(&g_asr_ringbuffer, &asr_data_frame)) {
            wss_asr_data_to_server(&asr_data_frame);
        } else if (MYERROR_Success == my_blocking_queue_try_dequeue(&g_asr_data_queue, &asr_data_frame)) {
            wss_asr_data_to_server(&asr_data_frame);
        }

        if (g_recording_vad == false || g_recording_upload == false) { /* VAD结束 */
            g_recording_uploading = false;
            wss_app_recorder_data_upstream(NULL);
            set_led_idle();
            if (g_recording_vad == false){
                log_i("[AUDIO]Stop VAD because VAD END.");
            } else {
                log_i("[AUDIO]Stop VAD because wss_mic_stop.");
            }
            send_audio_status(OAST_ASR, OASE_END);
            return;
        }
        msleep(10);
    }
    wss_app_recorder_data_upstream(NULL);
    send_audio_status(OAST_ASR, OASE_ABNORMAL);
    g_recording_uploading = false;
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
    vad_start_time = get_time_stamp();
    
    my_blocking_queue_clear(&g_asr_ringbuffer); /* 清空 g_asr_data_queue */
    wss_app_recorder_data_upstream(recorder_data_to_ringbuffer); /* 录音数据 -> asr_ringbuffer */
    set_led_mic_on(); // 开启录音时灯光变橙色
    msleep(500);

    while (get_time_stamp() < vad_stop_time && g_recording_upload) {
        msleep(10);
        if (g_recording_vad == true && is_mic_on()) {
            if (send_audio_status(OAST_ASR, OASE_START) == -1) {
                break;
            }
            start_audio_upload();
            my_blocking_queue_clear(&g_asr_ringbuffer); /* 清空 g_asr_data_queue */
            wss_app_recorder_data_upstream(recorder_data_to_ringbuffer); /* 录音数据 -> asr_ringbuffer */
            set_led_mic_on(); // 开启录音时灯光变橙色
        }
    }
    set_led_idle();
    wss_app_recorder_data_upstream(NULL);
    if (get_time_stamp() < vad_stop_time) {
        log_i("[AUDIO]Stop VAD timeout at %d.", get_time_stamp());
    } else {
        log_i("[AUDIO]Stop VAD because mic_off.");
    }
    vad_start_time = 0;
}

//=================================================================================================
// MUSIC response callback
//=================================================================================================
void app_music_play_end_cb(void *priv)
{
    os_mutex_pend(&g_audio_mutex, 0);
    g_audio_play_ongoing = false; // 音乐播放结束标志
    g_audio_playing = false;
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

    os_mutex_pend(&g_audio_mutex, 0);
    if (g_audio_off) {
        os_mutex_post(&g_audio_mutex);
        return 0;
    }

    if (g_audio_play_ongoing == false) {
        g_audio_play_ongoing = true;
        log_i("[AUDIO]set up to play.");
        total_written = 0;
        audio_req_time = get_time_stamp();
        app_music_play_stream_music_with_callback(g_app_ctx->music_player, app_music_play_end_cb, NULL);
    }
    
    os_mutex_post(&g_audio_mutex);
    while (1) {
        os_mutex_pend(&g_audio_mutex, 0);
        uint32_t chunk_size = (len - idx > MAX_CHUNK_SIZE) ? MAX_CHUNK_SIZE : len - idx;
        uint32_t written = app_music_play_stream_write(
            g_app_ctx->music_player, 
            data + idx, 
            chunk_size
        );
        edev_working();
        
        if (written == 0) {
            log_e("Failed to write audio data chunk,odmp_audio_data_cbk");
            break; // 错误处理，可根据需求调整
        }
        idx += written;
        total_written += written;
        log_d("Written chunk: %u bytes (total: %u / %u)", written, len, total_written);
        if (idx == len || g_audio_off)  break;
        
        os_mutex_post(&g_audio_mutex);
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
    os_mutex_post(&g_audio_mutex);
    log_i("[AUDIO] Total stream write: %u / %u bytes -> %u", idx, len, total_written);
    return 0;///(total_written == len) ? 0 : 1; // 返回，1表示部分失败, 0表示成功
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

#ifndef C_SPEAKLER
    gpio_direction_output(IO_PORTA_00, 0);  //Disable C speaker
    gpio_direction_output(IO_PORTB_02, 1);  //Enable E speaker
    /* UART指令开启C端le audio，并指定监听的 mac 地址*/
    uart_send_open_le_audio(g_app_ctx->mac);
#endif
    odmp_register_audio_data_cbk(odmp_audio_data_cbk); 

    wss_app_recorder_en(ENABLE);
    /* 设置VAD回调，触发音频上传 */
    recorder_set_vad_cbk(g_app_ctx->recorder, recorder_vad_cb, NULL);

    g_recording_upload = false;

    /* 清除录音数据写入回调，默认不上传 */
    wss_app_recorder_data_upstream(NULL);
}

void wss_audio_uninit()
{
    log_i("[AUDIO]Uninit WSS audio\n");
    wss_ble_audio_stop(); // 停止音频播放
    wss_mic_stop(); // 停止录音
    
    odmp_register_audio_data_cbk(NULL); 
    
    gpio_direction_output(IO_PORTA_00, 1);  //Enable C speaker
    gpio_direction_output(IO_PORTB_02, 0);  // Disable E speaker
    set_dec_volume(g_app_ctx->music_player, INIT_VOLUME_VALUE); // 恢复默认音量

    my_blocking_queue_destroy_static(&g_asr_data_queue);
    my_ring_buffer_destroy_static(&g_asr_ringbuffer);

    wss_app_recorder_en(DISABLE);
    /* 取消VAD回调 */
    recorder_set_vad_cbk(g_app_ctx->recorder, NULL, NULL);
    /* 取消录音上传回调 */
    wss_app_recorder_data_upstream(NULL);
    /* 开启录音 */
    os_mutex_del(&g_audio_mutex, 1);
}
