#include "function_test.h"
#include "cpu.h"

#include "util/cqueue.h"
#include "bt_ble/client/ble_distance.h"
#include "async_msg_loop.h"
#include "app_mode/lwl_app_core.h"
#include "gpio.h"
#include "device.h"
#include "app_config.h"
#include "protocol/cp/c_uart_p.h"

#define ENABLE 1
#define DISABLE 0


static u32 g_curr_time = 0;

static bool g_test_mode = false;

/* Recorder VAD */
static int g_vad_state = 0;
static int g_vad_state_curr = 0;

/* Base64  */
#define ASR_DATA_CHUNK_SIZE 1280
#define BASE64_BUF_MAX_SIZE 1709    // ((ASR_DATA_CHUNK_SIZE + 2) / 3) * 4 = 1708
static char base64_buf[BASE64_BUF_MAX_SIZE];

static void wss_asr_data(void *data, u32 size);

//=================================================================================================
// Recorder
//=================================================================================================

static void recorder_data_cb(void* priv, void *data, u32 size) {
    wss_asr_data(data, size);
}

static void recorder_vad_cb(void* priv, int type)
{
    lwl_app_context_t *context = lwl_app_core_context_get_instance();
    
    if (type == 1) {
        g_vad_state_curr = 1;
    } else {
        g_vad_state_curr = 0;
    }
}

/**
 * @name wss_app_recorder_en
 * @brief 开启/停止 录音编码器
 * @details 编码器默认打开VAD，可通过recorder_reg_vad_callback注册VAD回调
 *   停止编码器后，VAD将不再触发，直到重新开启
 */
static void wss_app_recorder_en(u8 en)
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
 * @name wss_app_recorder_data_upstream_en
 * @brief 设置/取消 录音数据存入缓冲
 * @details 设置后所有录音数据将会存入g_asr_data_queue
 *   设置后，应用程序需要手动监听缓冲数据并取出处理
 */
static void wss_app_recorder_data_upstream_en(u8 en)
{
    lwl_app_context_t *context = lwl_app_core_context_get_instance();
    if (context && context->recorder) {
        if (en) {
            recorder_set_data_cb(context->recorder, recorder_data_cb, NULL); /* 设置录音上传回调 */
        } else {
            recorder_set_data_cb(context->recorder, NULL, NULL); /* 取消录音上传回调 */
        }
    }
}

/**
 * @name wss_app_set_vad_callback
 * @brief 设置/取消 VAD事件回调
 */
static void wss_app_set_vad_callback(u8 en)
{
    lwl_app_context_t *context = lwl_app_core_context_get_instance();
    if (context && context->recorder) {
        if (en) {
            recorder_set_vad_cbk(context->recorder, recorder_vad_cb, NULL); /* 设置VAD事件回调 */
        } else {
            recorder_set_vad_cbk(context->recorder, NULL, NULL); /* 取消VAD事件回调 */
        }
    }
}

static void wss_app_vad_update_loop()
{
    if (g_vad_state == 0 && g_vad_state_curr == 1) {
        log_i("VAD speaker start");
    }
    if (g_vad_state == 1 && g_vad_state_curr == 0) {
        log_i("VAD speaker stop");
    }
    g_vad_state = g_vad_state_curr;
}

//=================================================================================================
// ASR data queue
//=================================================================================================
typedef struct {
    u8 data[ASR_DATA_CHUNK_SIZE];
} AsrData;

#define MY_ASR_DATA_QUEUE_CAPACITY 10
static MyBlockingQueue g_asr_data_queue; // MyBlockingQueue<AsrData>
static u8 g_asr_data_queue_buffer[MY_ASR_DATA_QUEUE_CAPACITY * sizeof(AsrData)];

static void wss_asr_data(void *data, u32 size)
{
    ASSERT(size == ASR_DATA_CHUNK_SIZE);

    /* 调试输出数据量 */
    static int cnt2 = 0;
    cnt2 += ASR_DATA_CHUNK_SIZE;
    log_d("cnt2: %d\n", cnt2);

    AsrData *asr_data = (AsrData *)data;
    my_blocking_queue_enqueue(&g_asr_data_queue, asr_data);
}

static void wss_asr_data_to_console(AsrData *asr_data)
{
    #define AUDIO_DATA_BUF_SIZE (32000 * 10)    //5s
    static uint8_t *audio_data_buf = NULL;
    if (audio_data_buf == NULL) {
        audio_data_buf = malloc(AUDIO_DATA_BUF_SIZE);
    }
    static int audio_data_count = 0;

    memcpy(&audio_data_buf[audio_data_count], asr_data->data, ASR_DATA_CHUNK_SIZE);
    audio_data_count += ASR_DATA_CHUNK_SIZE;

    if (audio_data_count >= AUDIO_DATA_BUF_SIZE) {
        wdt_disable();
        local_irq_disable();
        printf_buf(audio_data_buf, AUDIO_DATA_BUF_SIZE);
        local_irq_enable();
        wdt_enable();
        cpu_reset();
    }
}

static void start_mic_record_test(void *priv) 
{
    msleep(10000);
}

static void function_test_lwl_event_handler_cb(lwl_event_t *e)
{
    switch (e->event_type) {
    case LWL_EVENT_TYPE_UART_IO_KEY_SHORT_CLICK:
        log_i("switch to test mode")
        /* 清空asr data queue */
        my_blocking_queue_clear(&g_asr_data_queue);
        /* 取消VAD回调 */
        wss_app_set_vad_callback(DISABLE);
        /* 设置录音上传回调 */
        wss_app_recorder_data_upstream_en(ENABLE);
        /* 开启录音 */
        wss_app_recorder_en(ENABLE);
        /* 切换测试模式 */
        g_test_mode = true;
        break;
    case LWL_EVENT_TYPE_UART_SYNC:
        log_i("SYNC");
        log_i("SYNC response by wss_app_lwl_event_handler_cb (1, 16)");
        uart_response_synch_status(1, 16);
        break;
    case LWL_EVENT_TYPE_UART_TOUCH_KEY_DOWN:
        log_i("TOUCH KEY DOWN");
        break;
    case LWL_EVENT_TYPE_UART_TOUCH_KEY_UP:
        log_i("TOUCH KEY UP");
        break;
    case LWL_EVENT_TYPE_UART_SHUTDOWN:
        log_i("SHUTDOWN");
        power_set_soft_poweroff();
        break;
    }
}

void function_test_app_init()
{
    log_i("Init FT application\n");
    my_blocking_queue_init_static(&g_asr_data_queue, g_asr_data_queue_buffer, sizeof(AsrData), MY_ASR_DATA_QUEUE_CAPACITY);

    /* 设置VAD回调 */
    wss_app_set_vad_callback(ENABLE);
    /* 默认不设置录音上传回调 */
    wss_app_recorder_data_upstream_en(DISABLE);
    /* 开启录音 */
    wss_app_recorder_en(ENABLE);
    
    /* 默认2秒后开启录音测试 */
    lwl_app_context_t *context = lwl_app_core_context_get_instance();
    threadpool_add_task(context->threadpool, start_mic_record_test, NULL);

    /* 注册lwl事件处理回调 */
    lwl_event_handler_add(function_test_lwl_event_handler_cb);
}

void function_test_app_uninit()
{
    log_i("Uninit FT application\n");
    my_blocking_queue_destroy_static(&g_asr_data_queue);
    
    /* 设置VAD回调 */
    wss_app_set_vad_callback(DISABLE);
    /* 取消录音上传回调 */
    wss_app_recorder_data_upstream_en(DISABLE);
    /* 开启录音 */
    wss_app_recorder_en(DISABLE);
    
    /* 取消注册lwl事件处理回调 */
    lwl_event_handler_remove(function_test_lwl_event_handler_cb);
}

