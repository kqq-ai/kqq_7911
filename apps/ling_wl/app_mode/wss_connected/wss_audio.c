#include "wss_audio.h"
#include "wss_app_util.h"
#include "cpu.h"

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
#include "protocol/odm/odm_util.h"

#include "protocol/odm/odm_secure.h"

static lwl_app_context_t *g_app_ctx = NULL;

/*=================================================================================================
 * Vad timer
 *=================================================================================================*/
int vad_start_time = 0;
void update_time(){
    vad_start_time = timer_get_ms();
}
/*=================================================================================================
 * Audio dec app context
 *=================================================================================================*/
static int g_audio_play_end = false;
static int g_audio_play_ongoing_num = 0;
/*=================================================================================================
 * Record app context
 *=================================================================================================*/
static int g_recording = false;

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

#define MY_ASR_RING_BUFFER_CAPACITY 20
static MyRingBuffer g_asr_ringbuffer;    // MyRingBuffer<AsrData>
static u8 g_asr_ringbuffer_buffer[MY_ASR_RING_BUFFER_CAPACITY * sizeof(AsrData)];

/**
 * @name wss_app_recorder_en
 * @brief 开启/停止 录音编码器
 * @details 编码器默认打开VAD，可通过recorder_reg_vad_callback注册VAD回调
 *   停止编码器后，VAD将不再触发，直到重新开启
 */
void wss_app_recorder_en(u8 en)
{
    lwl_app_context_t *context = lwl_app_core_context_get_instance();
    if (context && context->recorder) {
        if (en && recorder_get_play_pause_state(context->recorder) == 0) {
            recorder_start(context->recorder); /* 开启录音编码 */
            uart_send_set_led_color(7500, 7000, 9500);//开启麦克风
        } else if(!en && recorder_get_play_pause_state(context->recorder) == 1) {
            recorder_stop(context->recorder); /* 停止录音编码 */
            uart_send_set_led_color((7500, 7000, 6000));//关闭麦克风
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

//=================================================================================================
// ASR data queue
//=================================================================================================
static void recorder_data_to_data_queue(void *priv, void *data, u32 size)
{
    ASSERT(size == ASR_DATA_CHUNK_SIZE);
    // log_i("vfs: %d", size);

    AsrData *asr_data = (AsrData *)data;
    if (MYERROR_Is_Full == my_blocking_queue_try_enqueue(&g_asr_data_queue, asr_data)) {
        log_e("[WSSAPP]asr data qeue full, failed to enquque asr data.");
    }
}

static void recorder_data_to_ringbuffer(void *priv, void *data, u32 size)
{
    ASSERT(size == ASR_DATA_CHUNK_SIZE);

    AsrData *asr_data = (AsrData *)data;
    my_ring_buffer_write(&g_asr_ringbuffer, asr_data);
}

static void wss_asr_data_to_server(AsrData *asr_data)
{
    // /* 调试输出数据量 */
    // static int cnt1 = 0;
    // cnt1 += ASR_DATA_CHUNK_SIZE;
    // log_d("cnt1: %d\n", cnt1);

    log_d(">>");
    uint8_t *pdata = (uint8_t *)asr_data;
    if (send_bin_data(pdata, ASR_DATA_CHUNK_SIZE)) {
        // wss_application_exception_exit();
    }
    log_d("--");
}



static void start_audio_upload(void *priv)
{
    log_i("Start audio upload.");
    int recording_upload_package_index = 0;
    wss_app_recorder_data_upstream(recorder_data_to_data_queue); /* 录音数据 -> asr_data_queue */
    if (send_audio_status(OAST_ASR, OASE_START)) {
        // wss_application_exception_exit();
        return;
    }
    uart_send_set_led_color(10000, 6000, 10000);
    while (true) {
        // if (is_edev_far_away(g_curr_edev)) { /* 当前设备距离 > 5 */
        //     wss_app_recorder_data_upstream(recorder_data_to_ringbuffer); /* 录音数据 -> asr_ringbuffer */
        //     if (recording_upload_package_index > 0) { /* 已经上传了部分asr data时，发送message stop事件结束 */
        //         if (send_audio_status(DEVICE_ID, OAST_ASR, OASE_END)) {
        //             wss_application_exception_exit();
        //             return;
        //         }
        //     }
        //     return;
        // }

        /* 从ASR缓冲取数据发送给服务器，先将ringbuffer中的数据上传，再处理上传data queue中的数据 */
        if (MYRBERROR_Success == my_ring_buffer_read(&g_asr_ringbuffer, &asr_data_frame)) {
            wss_asr_data_to_server(&asr_data_frame);
        } else if (MYERROR_Success == my_blocking_queue_try_dequeue(&g_asr_data_queue, &asr_data_frame)) {
            wss_asr_data_to_server(&asr_data_frame);
        }
        recording_upload_package_index++;

        if (g_recording == false) { /* VAD结束 */
            wss_app_recorder_data_upstream(recorder_data_to_ringbuffer); /* 录音数据 -> asr_ringbuffer */
            
            if (send_audio_status(OAST_ASR, OASE_END)) {
                // wss_application_exception_exit();
                return;
            }
            uart_send_set_led_color(7500, 7000, 6000); /// baby talk finish
            threadpool_add_task(g_app_ctx->threadpool, wait_vad_task, NULL);
            return;
        }
        msleep(10);
    }
}

void wait_vad_task(void *priv)
{
    int vad_timeout = *((int*)priv);
    log_i("Wait VAD start %d\n", vad_timeout);
    update_time();

    while (vad_start_time + vad_timeout >= timer_get_ms()) {
        msleep(10);

        if (g_recording == true) {
            threadpool_add_task(g_app_ctx->threadpool, start_audio_upload, NULL);
            return;
        }
    }

    log_i("Wait VAD timeout.");
    wss_app_recorder_en(DISABLE);
    g_recording = false;
    vad_timeout = 0;
    // reply_tts_bind(g_edev_last);
    // g_edev_last = NULL;
    // wss_application_event_to_req(NULL, ORMET_VAD_SPEAKER_END);
}

static uint32_t odmp_audio_data_cbk(uint8_t *data, uint32_t len)
{
    const uint32_t MAX_CHUNK_SIZE = 50000;
    uint32_t total_written = 0;
    uint32_t remaining = len;
    
    while (remaining > 0) {
        uint32_t chunk_size = (remaining > MAX_CHUNK_SIZE) ? MAX_CHUNK_SIZE : remaining;
        uint32_t written = app_music_play_stream_write(
            g_app_ctx->music_player, 
            data + total_written, 
            chunk_size
        );
        
        if (written == 0) {
            log_e("Failed to write audio data chunk,odmp_audio_data_cbk");
            break; // 错误处理，可根据需求调整
        }
        
        total_written += written;
        remaining -= written;
        log_d("Written chunk: %u bytes (total: %u / %u)", written, total_written, len);
    }
    
    log_i("Total stream write: %u / %u bytes", total_written, len);
    return 0;///(total_written == len) ? 0 : 1; // 返回，1表示部分失败, 0表示成功
}

//=================================================================================================
// Recorder
//=================================================================================================
static void recorder_vad_cb(void* priv, int type)
{
    lwl_app_context_t *context = lwl_app_core_context_get_instance();
    
    switch (type) {
    case 0: /* VAD speaker stop */
        if (g_recording == true) {
            g_recording = false;
        }
        break;
    case 1: /* VAD speaker start */
        if (g_recording == false) {
            g_recording = true;
        }
        break;
    }
}

void wss_audio_init()
{
    log_i("Init WSS audio\n");
    my_blocking_queue_init_static(&g_asr_data_queue, g_asr_data_queue_buffer, sizeof(AsrData), MY_ASR_DATA_QUEUE_CAPACITY);
    my_ring_buffer_init_static(&g_asr_ringbuffer, g_asr_ringbuffer_buffer, sizeof(AsrData), MY_ASR_RING_BUFFER_CAPACITY);

    g_app_ctx = lwl_app_core_context_get_instance();
    

    /* UART指令开启C端le audio */
    uart_send_open_le_audio(g_app_ctx->mac);

    /* 注册音频数据回调 */
    odmp_register_audio_data_cbk(odmp_audio_data_cbk); 
    /* 设置VAD回调 */
    recorder_set_vad_cbk(g_app_ctx->recorder, recorder_vad_cb, NULL);

    /* 默认不设置录音上传回调 */
    wss_app_recorder_data_upstream(recorder_data_to_ringbuffer);

    /* 关闭录音 */
    wss_app_recorder_en(DISABLE);
}

void wss_audio_uninit()
{
    log_i("Uninit WSS audio\n");
    my_blocking_queue_destroy_static(&g_asr_data_queue);
    my_ring_buffer_destroy_static(&g_asr_ringbuffer);

    /* 取消音频数据回调 */
    odmp_register_audio_data_cbk(NULL); 
    /* 取消VAD回调 */
    recorder_set_vad_cbk(g_app_ctx->recorder, NULL, NULL);
    /* 取消录音上传回调 */
    wss_app_recorder_data_upstream(NULL);
    /* 开启录音 */
    wss_app_recorder_en(DISABLE);
}
