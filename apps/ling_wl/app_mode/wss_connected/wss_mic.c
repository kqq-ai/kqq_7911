#include "wss_mic.h"
#include "wss_app_util.h"
#include "app_cookie.h"

#include "util/cqueue.h"
#include "util/ringbuffer.h"
#include "app_mode/lwl_app_core.h"
#include "util/threadpool_tasks.h"

#include "protocol/odm/odmp.h"
#include "LED/LED.h"

#define TOUCH_KEY_HEAT_TIMEOUT_MS 500
#define VAD_START_HEAT_TIMEOUT_MS 100

static lwl_app_context_t *g_app_ctx = NULL;
static OS_MUTEX g_mic_mutex;


static void set_led_mic_on(void)    { set_rgb_ON(COLOR_WHITE,1); }
static void set_led_mic_off(void)   { set_rgb_OFF(COLOR_WHITE,1); }
static void set_led_vad_on(void)    { set_rgb_ON(COLOR_YELLOW,2); }
static void set_led_vad_off(void)   { set_rgb_OFF(COLOR_YELLOW,2); }

/*=================================================================================================
 * Vad timer
 *=================================================================================================*/
u32 vad_stop_time = 0;  // 0表示未设置，超时停止VAD

/*=================================================================================================
 * Record app context
 *=================================================================================================*/
static int g_vad_on = false;                // VAD状态标志
static int g_recording = false;             // MIC开
static int g_recording_vad = false;         // VAD状态标志（根据麦克风数据判断）
static int g_recording_uploading = false;   // 录音上传状态标志（WSS 状态）

/**
 * @name app_recorder_en
 * @brief 开启/停止 录音编码器
 * @details 编码器默认打开VAD，可通过recorder_reg_vad_callback注册VAD回调
 *   停止编码器后，VAD将不再触发，直到重新开启
 */
#define ENABLE 1
#define DISABLE 0
void app_recorder_en(u8 en)
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

// 获取当前麦克风状态
static bool is_mic_on() {
    lwl_app_context_t *context = lwl_app_core_context_get_instance();
    bool recording = (recorder_get_play_pause_state(context->recorder) == 1);
    return recording;
}

//=================================================================================================
// ASR data queue
//=================================================================================================
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
        log_i("[WSS_MIC]asr queue overflow: dropped %d frames (%d/%d).",
              drop_count,
              my_blocking_queue_size(&g_asr_data_queue),
              MY_ASR_DATA_QUEUE_CAPACITY);
        enqueue_ret = my_blocking_queue_try_enqueue(&g_asr_data_queue, asr_data);
        if (enqueue_ret == MYERROR_Is_Full) {
            log_i("[WSS_MIC]asr data queue still full, discard incoming frame.");
        }
    } else if (enqueue_ret == MYERROR_Success) {
        int curr = my_blocking_queue_size(&g_asr_data_queue);
        if (curr >= (MY_ASR_DATA_QUEUE_CAPACITY * 3) / 4) {
            log_i("[WSS_MIC]asr queue high water mark %d/%d.", curr, MY_ASR_DATA_QUEUE_CAPACITY);
        }
    }
}

// 录音数据回调，如果VAD开启则写入循环队列缓存，否则直接写入 data queue
static void recorder_data_to_buffer(void *priv, void *data, u32 size)
{
    ASSERT(size == ASR_DATA_CHUNK_SIZE);

    AsrData *asr_data = (AsrData *)data;
    if (g_vad_on && g_recording_vad == false) {
        // VAD状态下，写入环形缓冲区
        my_ring_buffer_write(&g_asr_ringbuffer, asr_data);
    } else {
        while (MYRBERROR_Success == my_ring_buffer_read(&g_asr_ringbuffer, &asr_data_frame)) {
            recorder_data_to_data_queue(NULL, &asr_data_frame, ASR_DATA_CHUNK_SIZE);
        }
        recorder_data_to_data_queue(NULL, asr_data, ASR_DATA_CHUNK_SIZE);
    }
}

/**
 * @name recorder_data_upstream
 * @brief 设置 录音数据回调
 * @details 设置后所有录音数据将会存入g_asr_data_queue
 *   设置后，应用程序需要手动监听缓冲数据并取出处理
 */
static void recorder_data_upstream(recorder_data_cb_t cb)
{
    lwl_app_context_t *context = lwl_app_core_context_get_instance();
    if (context && context->recorder) {
        recorder_set_data_cb(context->recorder, cb, NULL); /* 设置录音上传回调 */
    }
}

// 将ASR数据发送到服务器
static int asr_data_to_server(AsrData *asr_data)
{
    uint8_t *pdata = (uint8_t *)asr_data;
    return send_bin_data(pdata, ASR_DATA_CHUNK_SIZE); // 修改: 返回 send_bin_data 的结果 (0成功, -1失败)
}

/**
 * @brief 等待按键触发VAD上传的独立线程
 */
void wait_vad_task(void *priv)
{
    u32 vad_start_time = timer_get_ms();
    log_i("[WSS_MIC]🎙️🎙️🎙️start VAD until %d\n", vad_stop_time);
    my_blocking_queue_clear(&g_asr_ringbuffer);
    my_blocking_queue_clear(&g_asr_data_queue);
    recorder_data_upstream(recorder_data_to_buffer); /* 录音数据 -> asr_data_queue */
    while (vad_start_time + VAD_START_THRESHOLD + VAD_START_HEAT_TIMEOUT_MS > timer_get_ms() && g_recording) {
        msleep(20);
    }
    set_led_mic_on(); // 开启录音时灯光变橙色

    if (g_recording) {
        log_i("[WSS_MIC]🎙️🎙️🎙️ASR upload started\n");
        send_device_status(DEVICE_STATUS_RECORDING); // 正在录音
    }
    g_recording_uploading = false;
    while (is_mic_on() && g_recording && vad_stop_time > timer_get_ms()) {
        if (MYERROR_Success == my_blocking_queue_try_dequeue(&g_asr_data_queue, &asr_data_frame)) {
            if (g_recording_uploading == false) {
                send_audio_status(OAST_ASR, OASE_START);
                g_recording_uploading = true;
            }
            int ret = asr_data_to_server(&asr_data_frame);
            if (ret != 0) {
                log_i("[WSS_MIC]ASR Upload failed, break loop.");
                break;
            }
        } else if (g_recording_vad == false) {
            if (g_recording_uploading == true) {
                send_audio_status(OAST_ASR, OASE_END);
                g_recording_uploading = false;
            }
        }
        msleep(10);
    }
    set_led_mic_off();
    wss_mic_stop();

    log_i("[WSS_MIC]🎙️🎙️🎙️end VAD task %d\n", timer_get_ms());
    if (g_recording_uploading) {
        g_recording_uploading = false;
        send_audio_status(OAST_ASR, OASE_END);
    }
    vad_stop_time = 0;
}

/**
 * @brief 等待按键触发ASR上传的独立线程
 * @details
 *   1. 延时500ms，未收到结束请求，则上传音频ASR
 */
void wait_asr_task(void *priv)
{
    u32 asr_start_time = timer_get_ms();
    log_i("[WSS_MIC]🎙️🎙️🎙️start ASR task %d\n", asr_start_time);
    
    my_blocking_queue_clear(&g_asr_data_queue);
    recorder_data_upstream(recorder_data_to_data_queue); /* 录音数据 -> asr_data_queue */
    while ((asr_start_time + TOUCH_KEY_HEAT_TIMEOUT_MS > timer_get_ms() || is_mic_on() == false) && g_recording) {
        msleep(20);
    }
    set_led_mic_on(); // 开启录音时灯光变橙色

    if (g_recording) {
        log_i("[WSS_MIC]🎙️🎙️🎙️ASR upload started\n");
        send_audio_status(OAST_STREAM_AUDIO, OASE_START);
        g_recording_uploading = true;
        send_device_status(DEVICE_STATUS_RECORDING); // 正在录音
    }

    while (is_mic_on()) {
        if (MYERROR_Success == my_blocking_queue_try_dequeue(&g_asr_data_queue, &asr_data_frame)) {
            int ret = asr_data_to_server(&asr_data_frame);
            if (ret != 0) {
                log_i("[WSS_MIC]ASR Upload failed, break loop.");
                break;
            }
        } else if (g_recording == false) {
            break;
        }
        msleep(10);
    }
    set_led_mic_off();
    wss_mic_stop();

    u32 asr_end_time = timer_get_ms();
    log_i("[WSS_MIC]🎙️🎙️🎙️end ASR task %d, duration %d ms\n", asr_end_time, asr_end_time - asr_start_time);
    if (g_recording_uploading) {
        g_recording_uploading = false;
        send_audio_status(OAST_STREAM_AUDIO, OASE_END);
    }
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
    switch (type) {
    case 0: /* VAD speaker stop */
        if (g_recording_vad == true) {
            g_recording_vad = false;
            set_led_vad_off();
        }
        break;
    case 1: /* VAD speaker start */
        if (g_recording_vad == false) {
            g_recording_vad = true;
            set_led_vad_on();
        }
        break;
    }
}

// 停止麦克风录音，可选择是否上传剩余音频数据
void wss_mic_stop() {
    log_mqtt("💧💧💧 停止麦克风录音");
    os_mutex_pend(&g_mic_mutex, 0);
    recorder_data_upstream(NULL);
    app_recorder_en(DISABLE);
    g_recording = false;
    os_mutex_post(&g_mic_mutex);
}

// 开始麦克风录音，设置VAD超时时间
void wss_mic_start() {
    log_mqtt("🔥🔥🔥 开始麦克风录音");
    os_mutex_pend(&g_mic_mutex, 0);
    app_recorder_en(ENABLE);
    os_mutex_post(&g_mic_mutex);
}

// 开始麦克风录音，设置VAD超时时间
void wss_vad_start(uint timestamp) {
    log_i("[WSS_MIC]wss_vad_start set timeout %d", timestamp);
    os_mutex_pend(&g_mic_mutex, 0);
    g_vad_on = true; // 开启VAD
    if (!g_recording) {
        g_recording = true;
        vad_stop_time = timer_get_ms() + timestamp; // 设置VAD超时时间
        threadpool_add_task(g_app_ctx->threadpool, wait_vad_task, NULL);
    } else {
        vad_stop_time = timer_get_ms() + timestamp; // 设置VAD超时时间
    }
    os_mutex_post(&g_mic_mutex);
}

// 开始麦克风录音，准备上传ASR
void wss_asr_start() {
    log_i("[WSS_MIC]wss_asr_start");
    os_mutex_pend(&g_mic_mutex, 0);
    g_vad_on = false; // 关闭VAD
    if (!g_recording) {
        g_recording = true;
        threadpool_add_task(g_app_ctx->threadpool, wait_asr_task, NULL);
    }
    os_mutex_post(&g_mic_mutex);
}

// 取消上传ASR, 获取当前音频上传状态
bool wss_asr_stop() {
    os_mutex_pend(&g_mic_mutex, 0);
    bool uploading = (g_recording_uploading == true);
    recorder_data_upstream(NULL);
    g_recording = false;
    os_mutex_post(&g_mic_mutex);
    while (vad_stop_time > 0 || g_recording_uploading) {
        msleep(10);
    }
    return uploading;
}

void wss_mic_init()
{
    log_i("[WSS_MIC]Init WSS wss_mic\n");
    if (!os_mutex_valid(&g_mic_mutex)) {
        os_mutex_create(&g_mic_mutex);
    }
    my_blocking_queue_init_static(&g_asr_data_queue, g_asr_data_queue_buffer, sizeof(AsrData), MY_ASR_DATA_QUEUE_CAPACITY);
    my_ring_buffer_init_static(&g_asr_ringbuffer, g_asr_ringbuffer_buffer, sizeof(AsrData), MY_ASR_RING_BUFFER_CAPACITY);

    g_app_ctx = lwl_app_core_context_get_instance();

    /* 设置VAD回调，触发音频上传 */
    recorder_set_vad_cbk(g_app_ctx->recorder, recorder_vad_cb, NULL);
    g_vad_on = true;
    wss_mic_start(); // 开启录音

    g_recording = false;
}

void wss_mic_uninit()
{
    log_i("[WSS_MIC]Uninit WSS wss_mic\n");
    os_mutex_pend(&g_mic_mutex, 0);
    
    /* 取消VAD回调 */
    recorder_set_vad_cbk(g_app_ctx->recorder, NULL, NULL);
    // g_recording_vad = false;
    set_led_vad_off();

    wss_mic_stop(); // 停止录音
    
    g_recording = false;

    my_blocking_queue_destroy_static(&g_asr_data_queue);
    my_ring_buffer_destroy_static(&g_asr_ringbuffer);
    os_mutex_post(&g_mic_mutex);
}
