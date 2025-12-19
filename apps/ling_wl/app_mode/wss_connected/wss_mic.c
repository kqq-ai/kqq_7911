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
u32 vad_start_time = 0;
u32 vad_stop_time = 0;  // 0表示未设置，超时停止VAD

/*=================================================================================================
 * Record app context
 *=================================================================================================*/
static int g_recording = false;             // ASR强制上传标志忽略VAD，优先级更高
static int g_recording_vad = false;         // VAD状态标志（根据麦克风数据判断），发送ASR结束（1，2）
static int g_recording_upload = false;      // VAD开关录音上传状态标志（WSS 状态），发送ASR异常（1，3）
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


// 录音数据回调，写入循环队列缓存
static void recorder_data_to_ringbuffer(void *priv, void *data, u32 size)
{
    ASSERT(size == ASR_DATA_CHUNK_SIZE);

    AsrData *asr_data = (AsrData *)data;
    my_ring_buffer_write(&g_asr_ringbuffer, asr_data);
}

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
static void start_recorder_upload()
{
    log_i("[WSS_MIC]🎙️🎙️🎙️Start wss_mic upload.");
    my_blocking_queue_clear(&g_asr_data_queue); /* 清空 g_asr_data_queue */
    recorder_data_upstream(recorder_data_to_data_queue); /* 录音数据 -> asr_data_queue */
    g_recording_uploading = true;

    while (is_mic_on() && g_recording_vad && g_recording_upload) {
        int ret = 0;
        /* 从ASR缓冲取数据发送给服务器，先将ringbuffer中的数据上传，再处理上传data queue中的数据 */
        if (MYRBERROR_Success == my_ring_buffer_read(&g_asr_ringbuffer, &asr_data_frame)) {
            ret = asr_data_to_server(&asr_data_frame);
        } else if (MYERROR_Success == my_blocking_queue_try_dequeue(&g_asr_data_queue, &asr_data_frame)) {
            ret = asr_data_to_server(&asr_data_frame);
        }
        
        // 修改: 如果发送失败(连接断开)，退出循环
        if (ret != 0) {
            log_i("[WSS_MIC]Upload failed (ret=%d), break loop.", ret);
            g_recording_upload = false; // 停止上传标志
            break;
        }

        msleep(10);
    }
    g_recording_uploading = false;
    recorder_data_upstream(recorder_data_to_ringbuffer);
    if (g_recording_vad == false){/* VAD结束 */
        log_i("[WSS_MIC]🎙️🎙️🎙️Stop wss_mic_upload because VAD END.");
    } else if (g_recording_upload == false) {
        log_i("[WSS_MIC]🎙️🎙️🎙️Stop wss_mic_upload because ASR start.");
    } else {
        log_i("[WSS_MIC]🎙️🎙️🎙️Stop wss_mic_upload because wss_mic_stop.");
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
    log_i("[WSS_MIC]🎙️🎙️🎙️start VAD until %d\n", vad_stop_time);
    while (is_mic_on() == false && g_recording_upload) {
        msleep(30);
    }
    if (g_recording_upload) {
        // my_blocking_queue_clear(&g_asr_ringbuffer); /* 清空 g_asr_data_queue */
        recorder_data_upstream(recorder_data_to_ringbuffer); /* 录音数据 -> asr_ringbuffer */
        set_led_mic_on(); // 开启录音时灯光变橙色
        while (vad_start_time + VAD_START_THRESHOLD + VAD_START_HEAT_TIMEOUT_MS > timer_get_ms() && g_recording_upload) {
            msleep(20);
        }

        while (timer_get_ms() < vad_stop_time && g_recording_upload && is_mic_on()) {
            msleep(10);
            if (g_recording_vad == true) {
                if (send_audio_status(OAST_ASR, OASE_START) == -1) {
                    // break; // 防止发送失败时，关闭音频上传
                }
                start_recorder_upload();
            }
        }
        set_led_mic_off();
        recorder_data_upstream(NULL);
        if (!is_mic_on()) {
            log_i("[WSS_MIC]🎙️🎙️🎙️Stop VAD because wss_mic_stop.");
        } else if (!g_recording_upload) {
            log_i("[WSS_MIC]🎙️🎙️🎙️Stop VAD because ASR start.");
        } else {
            log_i("[WSS_MIC]🎙️🎙️🎙️Stop VAD timeout at %d > %d.", timer_get_ms(), vad_stop_time);
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
    recorder_data_upstream(recorder_data_to_ringbuffer); /* 录音数据 -> asr_ringbuffer */
    my_blocking_queue_clear(&g_asr_data_queue); /* 清空 g_asr_data_queue */
    set_led_mic_on(); // 开启录音时灯光变橙色
    u32 asr_start_time = timer_get_ms();
    log_i("[WSS_MIC]🎙️🎙️🎙️start ASR task %d\n", asr_start_time);
    while ((asr_start_time + TOUCH_KEY_HEAT_TIMEOUT_MS > timer_get_ms() || is_mic_on() == false) && g_recording) {
        msleep(20);
    }

    if (g_recording) {
        log_i("[WSS_MIC]🎙️🎙️🎙️ASR upload started\n");
        send_audio_status(OAST_STREAM_AUDIO, OASE_START);
        recorder_data_upstream(recorder_data_to_data_queue); /* 录音数据 -> asr_data_queue */
        g_recording_uploading = true;
    }

    while (is_mic_on() && g_recording) {
        int ret = 0;
        /* 从ASR缓冲取数据发送给服务器，先将ringbuffer中的数据上传，再处理上传data queue中的数据 */
        if (MYRBERROR_Success == my_ring_buffer_read(&g_asr_ringbuffer, &asr_data_frame)) {
            ret = asr_data_to_server(&asr_data_frame);
        } else if (MYERROR_Success == my_blocking_queue_try_dequeue(&g_asr_data_queue, &asr_data_frame)) {
            ret = asr_data_to_server(&asr_data_frame);
        }
        
        // 修改: 如果发送失败，退出循环
        if (ret != 0) {
            log_i("[WSS_MIC]ASR Upload failed, break loop.");
            break;
        }
        msleep(10);
    }
    set_led_mic_off();

    u32 asr_end_time = timer_get_ms();
    log_i("[WSS_MIC]🎙️🎙️🎙️end ASR task %d, duration %d ms\n", asr_end_time, asr_end_time - asr_start_time);
    if (g_recording_uploading) {
        g_recording_uploading = false;
        send_audio_status(OAST_STREAM_AUDIO, OASE_END);
        recorder_data_upstream(NULL);
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
    lwl_app_context_t *context = lwl_app_core_context_get_instance();
    
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

// 获取当前音频上传状态
bool is_recording_uploading() {
    os_mutex_pend(&g_mic_mutex, 0);
    bool uploading = (g_recording_uploading == true);
    os_mutex_post(&g_mic_mutex);
    return uploading;
}

// 停止麦克风录音，可选择是否上传剩余音频数据
void wss_mic_stop() {
    log_mqtt("💧💧💧 停止麦克风录音");
    os_mutex_pend(&g_mic_mutex, 0);
    recorder_data_upstream(NULL);
    app_recorder_en(DISABLE);
    os_mutex_post(&g_mic_mutex);
}

// 开始麦克风录音，设置VAD超时时间
void wss_mic_start() {
    log_mqtt("🔥🔥🔥 开始麦克风录音");
    os_mutex_pend(&g_mic_mutex, 0);
    app_recorder_en(ENABLE);
    vad_start_time = timer_get_ms();
    os_mutex_post(&g_mic_mutex);
}

// 开始麦克风录音，设置VAD超时时间
void wss_vad_start(uint timestamp) {
    log_i("[WSS_MIC]wss_vad_start set timeout %d", timestamp);
    os_mutex_pend(&g_mic_mutex, 0);
    if (vad_stop_time == 0 && !g_recording) {
        g_recording_upload = true;
        vad_stop_time = timer_get_ms() + timestamp; // 设置VAD超时时间
        threadpool_add_task(g_app_ctx->threadpool, wait_vad_task, NULL);
    } else if (vad_stop_time != 0) {
        vad_stop_time = timer_get_ms() + timestamp; // 设置VAD超时时间
    }
    os_mutex_post(&g_mic_mutex);
}

// 开始麦克风录音，准备上传ASR
void wss_asr_start() {
    log_i("[WSS_MIC]wss_asr_start");
    os_mutex_pend(&g_mic_mutex, 0);
    if (!g_recording) {
        g_recording_upload = false; // 停止VAD上传
        g_recording = true;
        threadpool_add_task(g_app_ctx->threadpool, wait_asr_task, NULL);
    }
    os_mutex_post(&g_mic_mutex);
}

// 取消上传ASR
void wss_asr_stop() {
    os_mutex_pend(&g_mic_mutex, 0);
    g_recording = false;
    os_mutex_post(&g_mic_mutex);
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

    wss_mic_start(); // 开启录音

    /* 设置VAD回调，触发音频上传 */
    recorder_set_vad_cbk(g_app_ctx->recorder, recorder_vad_cb, NULL);

    g_recording_upload = false;
    g_recording = false;

    /* 清除录音数据写入回调，默认不上传 */
    recorder_data_upstream(NULL);
}

void wss_mic_uninit()
{
    log_i("[WSS_MIC]Uninit WSS wss_mic\n");
    os_mutex_pend(&g_mic_mutex, 0);
    
    /* 取消录音上传回调 */
    recorder_data_upstream(NULL);
    /* 取消VAD回调 */
    recorder_set_vad_cbk(g_app_ctx->recorder, NULL, NULL);

    wss_mic_stop(); // 停止录音

    my_blocking_queue_destroy_static(&g_asr_data_queue);
    my_ring_buffer_destroy_static(&g_asr_ringbuffer);
    os_mutex_post(&g_mic_mutex);
}
