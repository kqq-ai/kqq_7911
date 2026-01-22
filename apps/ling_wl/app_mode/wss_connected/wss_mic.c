#include "wss_mic.h"
#include "app_cookie.h"
#include "wss_app_util.h"

#include "app_mode/lwl_app_core.h"
#include "util/cqueue.h"
#include "util/ringbuffer.h"
#include "util/threadpool_tasks.h"

#include "LED/LED.h"
#include "protocol/odm/odmp.h"

#define TOUCH_KEY_HEAT_TIMEOUT_MS 500

static lwl_app_context_t *g_app_ctx = NULL;
static OS_MUTEX g_mic_mutex;

static void set_led_mic_on(void) { set_rgb_ON(COLOR_WHITE, 1); }
static void set_led_mic_off(void) { set_rgb_OFF(COLOR_WHITE, 1); }

/*=================================================================================================
 * Push-to-talk recording state
 *=================================================================================================*/
static volatile int g_mic_task_running = false;

/*=================================================================================================
 * Record app context
 *=================================================================================================*/
static int g_recording = false;           // MIC开
static int g_recording_uploading = false; // 录音上传状态标志（WSS 状态）

/**
 * @name app_recorder_en
 * @brief 开启/停止 录音编码器
 * @details 编码器默认打开VAD，可通过recorder_reg_vad_callback注册VAD回调
 *   停止编码器后，VAD将不再触发，直到重新开启
 */
#define ENABLE 1
#define DISABLE 0
void app_recorder_en(u8 en) {
  lwl_app_context_t *context = lwl_app_core_context_get_instance();
  if (context && context->recorder) {
    if (en && recorder_get_play_pause_state(context->recorder) == 0) {
      recorder_start(context->recorder); /* 开启录音编码 */
    } else if (!en && recorder_get_play_pause_state(context->recorder) == 1) {
      recorder_stop(context->recorder); /* 停止录音编码 */
    }
  }
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
static MyRingBuffer g_asr_ringbuffer; // MyRingBuffer<AsrData>
static u8
    g_asr_ringbuffer_buffer[MY_ASR_RING_BUFFER_CAPACITY * sizeof(AsrData)];

// 录音数据回调，写入 g_asr_data_queue
static void recorder_data_to_data_queue(void *priv, void *data, u32 size) {
  ASSERT(size == ASR_DATA_CHUNK_SIZE);

  AsrData *asr_data = (AsrData *)data;
  int enqueue_ret = my_blocking_queue_try_enqueue(&g_asr_data_queue, asr_data);
  if (enqueue_ret == MYERROR_Is_Full) {
    AsrData dropped;
    int drop_count = 0;
    while (drop_count < 4 &&
           MYERROR_Success ==
               my_blocking_queue_try_dequeue(&g_asr_data_queue, &dropped)) {
      drop_count++;
    }
    log_i("[WSS_MIC]asr queue overflow: dropped %d frames (%d/%d).", drop_count,
          my_blocking_queue_size(&g_asr_data_queue),
          MY_ASR_DATA_QUEUE_CAPACITY);
    enqueue_ret = my_blocking_queue_try_enqueue(&g_asr_data_queue, asr_data);
    if (enqueue_ret == MYERROR_Is_Full) {
      log_i("[WSS_MIC]asr data queue still full, discard incoming frame.");
    }
  } else if (enqueue_ret == MYERROR_Success) {
    int curr = my_blocking_queue_size(&g_asr_data_queue);
    if (curr >= (MY_ASR_DATA_QUEUE_CAPACITY * 3) / 4) {
      log_i("[WSS_MIC]asr queue high water mark %d/%d.", curr,
            MY_ASR_DATA_QUEUE_CAPACITY);
    }
  }
}

/**
 * @name recorder_data_upstream
 * @brief 设置 录音数据回调
 * @details 设置后所有录音数据将会存入g_asr_data_queue
 *   设置后，应用程序需要手动监听缓冲数据并取出处理
 */
static void recorder_data_upstream(recorder_data_cb_t cb) {
  lwl_app_context_t *context = lwl_app_core_context_get_instance();
  if (context && context->recorder) {
    recorder_set_data_cb(context->recorder, cb, NULL); /* 设置录音上传回调 */
  }
}

// 将ASR数据发送到服务器
static int asr_data_to_server(AsrData *asr_data) {
  uint8_t *pdata = (uint8_t *)asr_data;
  return send_bin_data(
      pdata,
      ASR_DATA_CHUNK_SIZE); // 修改: 返回 send_bin_data 的结果 (0成功, -1失败)
}

/**
 * @brief 等待按键触发ASR上传的独立线程
 * @details
 *   1. 延时500ms，未收到结束请求，则上传音频ASR
 */
void wait_asr_task(void *priv) {
  g_mic_task_running = true;
  u32 asr_start_time = timer_get_ms();
  my_blocking_queue_clear(&g_asr_data_queue);
  recorder_data_upstream(
      recorder_data_to_data_queue); /* 录音数据 -> asr_data_queue */
  while (asr_start_time + TOUCH_KEY_HEAT_TIMEOUT_MS > timer_get_ms() &&
         g_recording) {
    msleep(20);
  }
  set_led_mic_on(); // 开启录音时灯光变橙色

  if (g_recording) {
    log_mqtt("【flow】500ms到了🎙️🎙️🎙️音频实际上传开始");
    send_audio_stream_start(wss_request_get_stream_id(), "uplink");
    g_recording_uploading = true;
    send_device_status(DEVICE_STATUS_RECORDING); // 正在录音
  }

  while (g_recording) {
    if (MYERROR_Success ==
        my_blocking_queue_try_dequeue(&g_asr_data_queue, &asr_data_frame)) {
      // log_mqtt("【flow】上传音频数据，音频长度: %d 字节",
      // ASR_DATA_CHUNK_SIZE);
      int ret = asr_data_to_server(&asr_data_frame);
      if (ret != 0) {
        log_i("[WSS_MIC]ASR Upload failed, break loop.");
        log_i("【debug】ret: %d", ret);
        break;
      }
    }
    msleep(10);
  }
  set_led_mic_off();
  recorder_data_upstream(NULL);

  u32 asr_end_time = timer_get_ms();
  log_i("[WSS_MIC]🎙️🎙️🎙️end ASR task %d, duration %d ms\n", asr_end_time,
        asr_end_time - asr_start_time);
  if (g_recording_uploading) {
    g_recording_uploading = false;
    send_audio_stream_end(wss_request_get_stream_id(), "normal_complete");
  }
  g_recording = false;
  app_recorder_en(DISABLE); // stop recorder when key released / task ends
  g_mic_task_running = false;
}

// 停止麦克风录音，可选择是否上传剩余音频数据
void wss_mic_stop() {
  log_mqtt("❌❌❌ 停止麦克风收音");
  app_recorder_en(DISABLE);
}

// 开始麦克风录音，设置VAD超时时间
void wss_mic_start() {
  log_mqtt("🎤🎤🎤开启麦克风收音");
  app_recorder_en(ENABLE);
}

// 开始麦克风录音，准备上传ASR
void wss_mic_upload_start() {
  log_mqtt("【flow】开始上传音频");
  os_mutex_pend(&g_mic_mutex, 0);
  if (!g_recording) {
    g_recording = true;
    app_recorder_en(ENABLE); // start recorder only when key pressed
    threadpool_add_task(g_app_ctx->threadpool, wait_asr_task, NULL);
  }
  os_mutex_post(&g_mic_mutex);
}

// 取消上传ASR, 获取当前音频上传状态，及是否是长按，满足500ms以上才算长按
bool wss_upload_stop() {
  log_i("🛑🛑🛑wss_upload_stop %d", g_recording_uploading);
  os_mutex_pend(&g_mic_mutex, 0);
  bool uploading = (g_recording_uploading == true);
  recorder_data_upstream(NULL);
  g_recording = false;
  os_mutex_post(&g_mic_mutex);
  // 等待上传完成，打印等待时长
  u32 start_time = timer_get_ms();
  while (g_mic_task_running || g_recording_uploading) {
    msleep(2);
  }
  u32 end_time = timer_get_ms();
  log_mqtt("【flow】上传音频结束, duration %d ms", end_time - start_time);
  return uploading;
}

void wss_mic_init() {
  log_i("[WSS_MIC]Init WSS wss_mic\n");
  if (!os_mutex_valid(&g_mic_mutex)) {
    os_mutex_create(&g_mic_mutex);
  }
  os_mutex_pend(&g_mic_mutex, 0);
  my_blocking_queue_init_static(&g_asr_data_queue, g_asr_data_queue_buffer,
                                sizeof(AsrData), MY_ASR_DATA_QUEUE_CAPACITY);
  my_ring_buffer_init_static(&g_asr_ringbuffer, g_asr_ringbuffer_buffer,
                             sizeof(AsrData), MY_ASR_RING_BUFFER_CAPACITY);

  g_app_ctx = lwl_app_core_context_get_instance();

  g_recording = false;
  os_mutex_post(&g_mic_mutex);
}

void wss_mic_uninit() {
  log_i("[WSS_MIC]Uninit WSS wss_mic\n");
  os_mutex_pend(&g_mic_mutex, 0);

  wss_mic_stop();
  wss_upload_stop();

  g_recording = false;

  my_blocking_queue_destroy_static(&g_asr_data_queue);
  my_ring_buffer_destroy_static(&g_asr_ringbuffer);
  os_mutex_post(&g_mic_mutex);
}
