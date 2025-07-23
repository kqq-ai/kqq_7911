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
#include "protocol/cp/c_uart_p.h"

static lwl_app_context_t *g_app_ctx = NULL;
            
static void set_led_mic_on(void)        { uart_send_set_led_color(COLOR_ORANGE); }  //麦克风开启时灯光变橙色
static void set_led_asr_upload(void)    { uart_send_set_led_color(COLOR_GREEN); }   //音频上传时灯光变绿色
static void set_led_tts_wait(void)      { uart_send_set_led_color(COLOR_WHITE); }   //音频下载时灯光变白色
static void set_led_tts_play(void)      { uart_send_set_led_color(COLOR_BLUE); }    //音频播放时灯光变蓝色

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



/**
 * @brief 音频上传主循环任务（VAD触发后调用）
 * @details
 *   1. 设置录音数据回调为 recorder_data_to_data_queue（数据进入 g_asr_data_queue）
 *   2. 发送音频开始状态
 *   3. 优先上传 ringbuffer 中残留数据，再上传 data queue 数据
 *   4. 若检测到 g_recording == false（VAD结束），
 *      - 切回 ringbuffer 回调
 *      - 发送音频结束状态
 *      - 设置 LED
 *      - 投递 wait_vad_task 任务
 *   5. 每轮循环 sleep 10ms
 * @param priv 未使用
 */
static void start_audio_upload(void *priv)
{
    log_i("Start audio upload.");
    set_led_asr_upload(); /* 音频上传时灯光变绿色 */
    wss_app_recorder_data_upstream(recorder_data_to_data_queue); /* 录音数据 -> asr_data_queue */
    if (send_audio_status(OAST_ASR, OASE_START)) {
        // wss_application_exception_exit();
        return;
    }
    while (true) {
        /* 从ASR缓冲取数据发送给服务器，先将ringbuffer中的数据上传，再处理上传data queue中的数据 */
        if (MYRBERROR_Success == my_ring_buffer_read(&g_asr_ringbuffer, &asr_data_frame)) {
            wss_asr_data_to_server(&asr_data_frame);
        } else if (MYERROR_Success == my_blocking_queue_try_dequeue(&g_asr_data_queue, &asr_data_frame)) {
            wss_asr_data_to_server(&asr_data_frame);
        }

        if (g_recording == false) { /* VAD结束 */
            wss_app_recorder_data_upstream(recorder_data_to_ringbuffer); /* 录音数据 -> asr_ringbuffer */
            set_led_tts_wait();
            wss_app_recorder_en(DISABLE);
            
            if (send_audio_status(OAST_ASR, OASE_END)) {
                // wss_application_exception_exit();
                return;
            }
            return;
        }
        msleep(10);
    }
}

/**
 * @brief 等待VAD触发的独立线程
 * @details
 *   1. 等待 vad_timeout 毫秒内，如果检测到VAD有响应，则重新开始音频上传
 *   2. 等待超时后，关闭录音，重置相关状态
 * @param priv 指向 int 类型的 vad_timeout 毫秒数
 */
void wait_vad_task(void *priv)
{
    int vad_timeout = *((int*)priv);
    log_i("Wait VAD start %d\n", vad_timeout);
    update_time();
    set_led_mic_on(); // 开启录音时灯光变橙色

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
    set_led_tts_wait();
    // reply_tts_bind(g_edev_last);
    // g_edev_last = NULL;
    // wss_application_event_to_req(NULL, ORMET_VAD_SPEAKER_END);
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
    const uint32_t MAX_CHUNK_SIZE = 50000;
    uint32_t total_written = 0;
    uint32_t remaining = len;
    set_led_tts_play();
    
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

/**
 * @brief 录音VAD（语音活动检测）回调
 * @details
 *   当录音模块检测到人声开始或结束时自动调用。
 *   - type == 1：检测到人声开始（VAD start），设置 g_recording = true
 *   - type == 0：检测到人声结束（VAD stop），设置 g_recording = false
 *   该回调用于驱动音频上传流程的启动与终止。
 * @param priv 未使用
 * @param type 1=人声开始，0=人声结束
 */
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

    gpio_direction_output(IO_PORTA_00, 0);  //Disable C speaker
    gpio_direction_output(IO_PORTB_02, 1);  //Enable E speaker
    

    /* UART指令开启C端le audio，并指定监听的 mac 地址*/
    uart_send_open_le_audio(g_app_ctx->mac);

    /* 注册音频数据回调，用于本地播放 */
    odmp_register_audio_data_cbk(odmp_audio_data_cbk); 

    /* 设置VAD回调，触发音频上传 */
    recorder_set_vad_cbk(g_app_ctx->recorder, recorder_vad_cb, NULL);

    /* 循环录音数据写入回调，默认不上传 */
    wss_app_recorder_data_upstream(recorder_data_to_ringbuffer);

    /* 关闭录音 */
    wss_app_recorder_en(DISABLE);
}

void wss_audio_uninit()
{
    log_i("Uninit WSS audio\n");
    
    gpio_direction_output(IO_PORTA_00, 1);  //Enable C speaker
    gpio_direction_output(IO_PORTB_02, 0);  // Disable E speaker

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
