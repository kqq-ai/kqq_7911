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
void wss_mic_start(const char *stream_id) {
    log_mqtt("🔥🔥🔥 开始麦克风录音");
    if (stream_id == NULL) {
        log_e("[wss_mic]wss_mic_start: stream_id is NULL");
        return;
    }
    send_audio_stream_start(stream_id, "uplink");
    os_mutex_pend(&g_mic_mutex, 0);
    app_recorder_en(ENABLE);
    vad_start_time = timer_get_ms();
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

    log_i("[WSS_MIC]VAD callback disabled: app layer no longer starts mic/upload");

    /* 确保上传标志为false */
    g_recording_upload = false;
    g_recording = false;
    g_recording_uploading = false;
    g_recording_vad = false;

    /* 清除录音数据写入回调，默认不上传 */
    recorder_data_upstream(NULL);
}

void wss_asr_stop()
{
    log_mqtt("💧💧💧 停止ASR");
    wss_mic_stop();
}

void wss_mic_uninit()
{
    log_i("[WSS_MIC]Uninit WSS wss_mic\n");
    os_mutex_pend(&g_mic_mutex, 0);
    
    wss_mic_stop(); // 停止录音
    
    g_recording_upload = false;
    g_recording = false;

    my_blocking_queue_destroy_static(&g_asr_data_queue);
    my_ring_buffer_destroy_static(&g_asr_ringbuffer);
    os_mutex_post(&g_mic_mutex);
}
