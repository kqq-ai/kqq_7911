#include "wss_application.h"
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

static void wss_app_recorder_data_upstream(recorder_data_cb_t cb);
static void recorder_data_to_data_queue(void* priv, void *data, u32 size);
static void recorder_data_to_ringbuffer(void* priv, void *data, u32 size);
static void wss_application_switch_test_mode();
static void wss_asr_data(void *data, u32 size);
static void app_record_task_start();
static void app_task_break();

#define ENABLE 1
#define DISABLE 0

#define DUMMY_SN "000000000000000"

typedef enum {
    STATE_MODE_IDLE,
    STATE_MODE_ONEONONE,

    STATE_MODE_TEST,
} AppModeState;

typedef enum {
    STATE_PLAYINTRO_STARTUP,
    STATE_PLAYINTRO_SWITCH,
    STATE_PLAYINTRO_PLAYING,
    STATE_PLAYINTRO_WAIT_VAD,

    STATE_ONEONONE_WAIT_VAD,
    STATE_ONEONONE_RECORDING,
    STATE_ONEONONE_PLAYING,

    STATE_ONEONMANY_IDLE,

    STATE_TEST_STEP_1,
    STATE_TEST_STEP_2,
    STATE_TEST_STEP_3,
    STATE_TEST_STEP_4,
    STATE_TEST_STEP_5,
    STATE_TEST_STEP_6,
    STATE_TEST_STEP_7,
    STATE_TEST_STEP_8,
    STATE_TEST_STEP_9,
    STATE_TEST_STEP_10,

} AppTaskStepState;


static u32 g_curr_time = 0;
#define HEAT_TIMEOUT 5000
static u32 g_heat_timer = 0;
#define WAIT_AUDIO_RESP_TIMEOUT 5000
static u32 g_wait_audio_resp_timer = 0;

static bool g_test_mode = false;

static int g_exception_exit = false;

static lwl_app_context_t *g_app_ctx = NULL;

static lwl_eend_dev_t *g_edev_closest = NULL;


/* Task state */
static int g_mode_state = STATE_MODE_IDLE;

/* Oneonone context */
static int g_oneonone_state = STATE_ONEONONE_WAIT_VAD;
static int g_wait_resp_flag = false;

/* Test context */
static int g_test_state = STATE_TEST_STEP_1;
static int g_setup_complete = false;
static int g_tts_bind_flag = false;
static int g_audio_playing = false;
static int g_audio_ready = false;
static int g_audio_play_ok = false;

const char test_target_id[] = { 
    0x74, 0x65, 0x73, 0x74, 0x5F, // test_
    0xE5, 0xB0, 0x8F,             // 小
    0xE7, 0x86, 0x8A,             // 熊
    0x00                          // 结束符
};

/*=================================================================================================
 * break
 *=================================================================================================*/
static int g_message_stop_ok = false;
static int g_message_stop_message_id = 0;


/*=================================================================================================
 * E switch context
 *=================================================================================================*/
static int g_backend_switch_session_ok = false;
static int g_ble_edev_switch_ok = false;
static lwl_eend_dev_t *g_curr_edev = NULL;
static lwl_eend_dev_t *g_next_edev = NULL;

/*=================================================================================================
 * Audio dec app context
 *=================================================================================================*/
static int g_audio_play_end = false;
static int g_audio_play_ongoing_num = 0;

/*=================================================================================================
 * Audio enc app context
 *=================================================================================================*/
#define ASR_DATA_CHUNK_SIZE 144
typedef struct {
    u8 data[ASR_DATA_CHUNK_SIZE];
} AsrData;

#define MY_ASR_DATA_QUEUE_CAPACITY 20
static MyBlockingQueue g_asr_data_queue; // MyBlockingQueue<AsrData>
static u8 g_asr_data_queue_buffer[MY_ASR_DATA_QUEUE_CAPACITY * sizeof(AsrData)];

#define MY_ASR_RING_BUFFER_CAPACITY 20
static MyRingBuffer g_asr_ringbuffer;    // MyRingBuffer<AsrData>
static u8 g_asr_ringbuffer_buffer[MY_ASR_RING_BUFFER_CAPACITY * sizeof(AsrData)];

static int g_recording = false;
static AsrData asr_data_frame;

static int g_recording_upload_package_index = 0;

static bool is_edev_far_away(lwl_eend_dev_t *pdev)
{
    return (pdev->distance_info.distance_type == DISTANCE_2) || (pdev->distance_info.distance_type == DISTANCE_3);
}

static void wss_application_exception_exit()
{
    g_exception_exit = true;
}

static bool wss_application_check_exit()
{
    return g_exception_exit;
}


static void switch_end_callback(int code, pl_switch_info_t *switch_info, void *priv)
{
    switch (code) {
    case SWITCH_ONLY_CODE_TURNOFF_TIMEOUT:
    case SWITCH_ONLY_CODE_TURNON_TIMEOUT:
    case SWITCH_ONLY_CODE_UNEXPECTED_ERROR:
    case SWITCH_ONLY_CODE_SUCCESS:
        log_i("Switch PortE OK %d\n", code);
        g_ble_edev_switch_ok = true;
        break;
    }
}

static void app_music_play_end_cb(void *priv)
{
    log_i("app_music_play_end_cb\n");
}

//=================================================================================================
// WSS response callback
//=================================================================================================
static int odmp_rsp_cbk(int type, odmp_package_t *pkg)
{
    switch (type) {
    case OR_INITIAL_SETUP_RESPONSE:
        log_i("INITIAL_SETUP_RESPONSE\n");
        log_i("setup_complete: %d\n", pkg->initial_setup_response.setup_complete);
        g_setup_complete = true;
        break;
    case OR_DEVICE_SWITCH_RESPONSE:
        log_i("DEVICE_SWITCH_RESPONSE\n");
        log_i("device_id: %s\n", pkg->device_switch_response.device_id);
        log_i("status: %s\n", pkg->device_switch_response.status);
        break;
    case OR_TTS_BIND:
        log_i("TTS_BIND\n");
        log_i("c_id: %s\n", pkg->tts_bind.c_id);
        log_i("send_id: %s\n", pkg->tts_bind.send_id);
        log_i("wait_time: %d\n", pkg->tts_bind.wait_time);
        g_tts_bind_flag = true;
        break;
    case OR_AUDIO_STATUS:
        log_i("AUDIO_STATUS\n");
        log_i("c_id: %s\n", pkg->audio_status.c_id);
        log_i("type: %s\n", odmp_ast_to_text(pkg->audio_status.type));
        log_i("event: %s\n", odmp_ase_to_text(pkg->audio_status.event));
        log_i("format: %s\n", pkg->audio_status.audio_param.format);
        log_i("sample_rate: %d\n", pkg->audio_status.audio_param.sample_rate);
        log_i("channels: %d\n", pkg->audio_status.audio_param.channels);
        log_i("frame_duration: %d\n", pkg->audio_status.audio_param.frame_duration);
        
        if (pkg->audio_status.type == OAST_ASR) {
        } else if (pkg->audio_status.type == OAST_TTS) {
            if (pkg->audio_status.event == OASE_START) {
                log_i("AUDIO_STATUS: OASE_START\n");
                app_music_play_stream_music_with_callback(g_app_ctx->music_player, app_music_play_end_cb, NULL);
            } else if (pkg->audio_status.event == OASE_END) {
                log_i("AUDIO_STATUS: OASE_END\n");
            } else if (pkg->audio_status.event == OASE_ABNORMAL) {
            } else if (pkg->audio_status.event == OASE_SENTENCE_START) {
            } else if (pkg->audio_status.event == OASE_SENTENCE_END) {
            }
        }
        break;
    case OR_MESSAGE_TEXT:
        log_i("MESSAGE_TEXT %s %s\n", pkg->message_text.send_id, pkg->message_text.text);
        break;
    case OR_EXECUTION_TIME:
        log_i("EXECUTION_TIME %s %d\n", pkg->execution_time.event, pkg->execution_time.execution_time);
        break;
    case OR_PONG:
        log_i("LR_PONG\n");
        break;
    }
}

static int odmp_audio_data_cbk(uint8_t *data, uint32_t len)
{
    int ret = app_music_play_stream_write(g_app_ctx->music_player, data, len);
    log_d("stream write: %d\n", ret);
    return 0;
}

//=================================================================================================
// LWL event callback
//=================================================================================================
static void wss_app_lwl_event_handler_cb(lwl_event_t *e)
{
    static int en = 1;
    switch (e->event_type) {
    case LWL_EVENT_TYPE_EEND_DEV_CLOSEST_CHANGE: {
        lwl_eend_dev_t *p_eend_dev = e->distance_change.pdev;
        
        g_edev_closest = p_eend_dev;
        
        char sn_string[SN_SIZE * 2 + 1] = "(NULL)";
        p_eend_dev->get_string(p_eend_dev, sn_string, sizeof(sn_string));
        log_i("[MEASDIST]ble rssi: %d --- ble name  : %s closer ! =========================\n", p_eend_dev->distance_info.rssi, sn_string);
    }   break;
    case LWL_EVENT_TYPE_EEND_DEV_DISTANCE_CHANGE: {
        lwl_eend_dev_t *p_eend_dev = e->distance_change.pdev;
        char sn_string[SN_SIZE * 2 + 1] = "(NULL)";
        p_eend_dev->get_string(p_eend_dev, sn_string, sizeof(sn_string));
        log_i("[MEASDIST] %s: %d\r\n", sn_string, p_eend_dev->distance_info.rssi);

        switch (p_eend_dev->distance_info.distance_type) {
        case DISTANCE_0:
            log_d("[MEASDIST] distance < 2m : do things1 here --- %s\r\n", sn_string);
            if (send_req_message("CC1CC1CC1CC1", p_eend_dev->sn, false, ORMET_SWITCH_SUCCESS)) {
                wss_application_exception_exit();
                return;
            }
            break;
        case DISTANCE_1:
            log_d("[MEASDIST] distance > 2m  < 5m do things2 here ---%s\r\n", sn_string);
            break;
        case DISTANCE_2:
            log_d("[MEASDIST] distance > 5m do things3 here ---%s\r\n", sn_string);
            break;
        case DISTANCE_3:
            log_d("[MEASDIST] not found ---%s\r\n", sn_string);
            break;
        }
    }   break;
    case LWL_EVENT_TYPE_UART_IO_KEY_SHORT_CLICK: {
        log_i("SHORT_CLICK");

        if (g_edev_closest != NULL) {
            my_task_switch_only(g_edev_closest->sn, 0, switch_end_callback, NULL);
        }

    }   break;
    case LWL_EVENT_TYPE_UART_SYNC:
        log_i("SYNC");
        log_i("battery: %d", e->uart_cmd_sync.battery);
        uart_response_synch_status(g_mode_state != STATE_MODE_IDLE, 8);
        break;
    case LWL_EVENT_TYPE_UART_TOUCH_KEY_DOWN:
        log_i("TOUCH KEY DOWN");
        break;
    case LWL_EVENT_TYPE_UART_SHUTDOWN:
        log_i("SHUTDOWN");
        break;
    case LWL_EVENT_TYPE_UART_MOVE_MORE_THAN_ONE_METER:
        log_i("MOVE_MORE_THAN_ONE_METER");
        break;
    }
}

//=================================================================================================
// Recorder
//=================================================================================================
static void recorder_vad_cb(void* priv, int type)
{
    lwl_app_context_t *context = lwl_app_core_context_get_instance();
    
    if (type == 1 && g_recording == false) {
        g_recording = true;
    }

    if (type == 0 && g_recording == true) {
        g_recording = false;
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
        wss_application_exception_exit();
    }
    log_d("--");
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

static void start_audio_upload()
{
    int recording_upload_package_index = 0;
    wss_app_recorder_data_upstream(recorder_data_to_data_queue); /* 录音数据 -> asr_data_queue */

    if (send_audio_status("CC1CC1CC1CC1", OAST_ASR, OASE_START)) {
        wss_application_exception_exit();
        return;
    }

    while (true) {
        if (is_edev_far_away(g_curr_edev)) { /* 当前设备距离 > 5 */
            wss_app_recorder_data_upstream(recorder_data_to_ringbuffer); /* 录音数据 -> asr_ringbuffer */
            if (recording_upload_package_index > 0) { /* 已经上传了部分asr data时，发送message stop事件结束 */
                if (send_audio_status("CC1CC1CC1CC1", OAST_ASR, OASE_END)) {
                    wss_application_exception_exit();
                    return;
                }
            }
            return;
        }

        /* 从ASR缓冲取数据发送给服务器，先将ringbuffer中的数据上传，再处理上传data queue中的数据 */
        log_i("AAA");
        if (MYRBERROR_Success == my_ring_buffer_read(&g_asr_ringbuffer, &asr_data_frame)) {
            log_i("BBB"); wss_asr_data_to_server(&asr_data_frame);
        } else if (MYERROR_Success == my_blocking_queue_try_dequeue(&g_asr_data_queue, &asr_data_frame)) {
            log_i("CCC"); wss_asr_data_to_server(&asr_data_frame);
        }
        recording_upload_package_index++;

        if (g_recording == false) { /* VAD结束 */
            wss_app_recorder_data_upstream(recorder_data_to_ringbuffer); /* 录音数据 -> asr_ringbuffer */
            
            if (send_audio_status("CC1CC1CC1CC1", OAST_ASR, OASE_END)) {
                wss_application_exception_exit();
                return;
            }
        }
    }
}

// //=================================================================================================
// // Test
// //=================================================================================================
// static bool app_test_loop()
// {
//     struct websocket_struct *websockets_info = g_app_ctx->websockets_info;
//     switch (g_test_state) {
//     case STATE_TEST_STEP_1:
//         if (g_setup_complete == true) {

//             const char test_target_id[] = { 
//                 0x74, 0x65, 0x73, 0x74, 0x5F, // test_
//                 0xE5, 0xB0, 0x8F,             // 小
//                 0xE7, 0x86, 0x8A,             // 熊
//                 0x00                          // 结束符
//             };
//             const char *targets[] = {test_target_id};
//             char *send_data = odmp_req_message_near_trigger(targets, 1, "CC1CC1CC1CC1", test_target_id);
//             wss_server_send(g_app_ctx->websockets_info, send_data, strlen(send_data));
//             log_i("%s", send_data);
//             free(send_data);
            
//             g_tts_bind_flag = false;
//             g_test_state = STATE_TEST_STEP_2;
//             log_i("TEST_STEP_1 -> TEST_STEP_2\n");
//         }
//         break;
//     case STATE_TEST_STEP_2:
//         if (g_tts_bind_flag == true) {

//             const char test_target_id[] = { 
//                 0x74, 0x65, 0x73, 0x74, 0x5F, // test_
//                 0xE5, 0xB0, 0x8F,             // 小
//                 0xE7, 0x86, 0x8A,             // 熊
//                 0x00                          // 结束符
//             };
//             const char *targets[] = {test_target_id};
//             char *send_data = odmp_req_message_switch_success(targets, 1, "CC1CC1CC1CC1", test_target_id);
//             wss_server_send(g_app_ctx->websockets_info, send_data, strlen(send_data));
//             log_i("%s", send_data);
//             free(send_data);

//             g_audio_play_ok = false;
//             g_test_state = STATE_TEST_STEP_3;
//             log_i("TEST_STEP_2 -> TEST_STEP_3\n");
//         }
//         break;
//     case STATE_TEST_STEP_3:
//         if (g_audio_play_ok == true) {
//             g_recording = false;
//             g_test_state = STATE_TEST_STEP_4;
//             log_i("TEST_STEP_3 -> TEST_STEP_4\n");
//         }
//         break;
//     case STATE_TEST_STEP_4:
//         if (g_recording == true) {
//             my_blocking_queue_clear(&g_asr_data_queue); /* 清空asr data queue */

//             wss_app_recorder_data_upstream(recorder_data_to_data_queue); /* 录音数据 -> asr_data_queue */
//             g_recording_upload_package_index = 0;
//             g_recording = true;

//             g_test_state = STATE_TEST_STEP_5;
//             log_i("TEST_STEP_4 -> TEST_STEP_5\n");
//             break;
//         }
//         break;
//     case STATE_TEST_STEP_5:
//         if (g_recording_upload_package_index == 0) { /* 发送第一包数据前，先发送asr事件和asr start事件 */

//             char *send_data = odmp_audio_status("CC1CC1CC1CC1", OAST_ASR, OASE_START);
//             wss_server_send(g_app_ctx->websockets_info, send_data, strlen(send_data));
//             log_i("%s", send_data);
//             free(send_data);

//         }
//         /* 从ASR缓冲取数据发送给服务器，先将ringbuffer中的数据上传，再处理上传data queue中的数据 */
//         log_i("AAA");
//         if (MYRBERROR_Success == my_ring_buffer_read(&g_asr_ringbuffer, &asr_data_frame)) {
//             log_i("BBB");
//             wss_asr_data_to_server(websockets_info, &asr_data_frame);
//         } else if (MYERROR_Success == my_blocking_queue_try_dequeue(&g_asr_data_queue, &asr_data_frame)) {
//             log_i("CCC");
//             wss_asr_data_to_server(websockets_info, &asr_data_frame);
//         }

//         if (g_recording == false) { /* VAD结束 */
//             wss_app_recorder_data_upstream(recorder_data_to_ringbuffer); /* 录音数据 -> asr_ringbuffer */
            
//             char *send_data = odmp_audio_status("CC1CC1CC1CC1", OAST_ASR, OASE_END);
//             wss_server_send(g_app_ctx->websockets_info, send_data, strlen(send_data));
//             log_i("%s", send_data);
//             free(send_data);

//             g_tts_bind_flag = false;

//             g_test_state = STATE_TEST_STEP_6;
//             log_i("TEST_STEP_5 -> TEST_STEP_6\n");
//         }
//         break;
//     case STATE_TEST_STEP_6:
//         if (g_tts_bind_flag == true) {
            
//             const char *targets[] = {test_target_id};

//             if (send_req_message_switch_success(targets, 1, "CC1CC1CC1CC1", test_target_id) != 0) {
//                 wss_application_exception_exit();
//                 return true;
//             }

//             g_audio_play_ok = false;
//             g_test_state = STATE_TEST_STEP_7;
//             log_i("TEST_STEP_6 -> TEST_STEP_7\n");
//         }
//         break;
//     case STATE_TEST_STEP_7:
//         if (g_audio_play_ok == true) {
//             g_recording = false;
//             g_test_state = STATE_TEST_STEP_4;
//             log_i("TEST_STEP_7 -> TEST_STEP_4\n");
//         }
//         break;
//     case STATE_TEST_STEP_8:
//         break;
//     case STATE_TEST_STEP_9:
//         break;
//     case STATE_TEST_STEP_10:
//         break;
//     }
//     return false;
// }

//=================================================================================================
// LOOP
//=================================================================================================

static void wss_application_switch_test_mode()
{
    if (!g_test_mode) {
        log_i("switch to test mode")
        /* 清空asr data queue */
        my_blocking_queue_clear(&g_asr_data_queue);
        /* 默认不设置录音上传回调 */
        wss_app_recorder_data_upstream(recorder_data_to_data_queue);
        /* 开启录音 */
        wss_app_recorder_en(ENABLE);
        /* 切换测试模式 */
        g_test_mode = true;
    } else {
        log_i("switch to normal mode")
        /* 默认不设置录音上传回调 */
        wss_app_recorder_data_upstream(recorder_data_to_ringbuffer);
        /* 开启录音 */
        wss_app_recorder_en(ENABLE);
        /* 切换正常模式 */
        g_test_mode = false;
    }
}

void wss_application_init()
{
    log_i("Init WSS application\n");
    my_blocking_queue_init_static(&g_asr_data_queue, g_asr_data_queue_buffer, sizeof(AsrData), MY_ASR_DATA_QUEUE_CAPACITY);
    my_ring_buffer_init_static(&g_asr_ringbuffer, g_asr_ringbuffer_buffer, sizeof(AsrData), MY_ASR_RING_BUFFER_CAPACITY);

    g_app_ctx = lwl_app_core_context_get_instance();

    /* 注册lwl事件处理回调 */
    lwl_event_handler_add(wss_app_lwl_event_handler_cb);
    /* 注册odmp事件回调 */
    odmp_register_rsp_cbk(odmp_rsp_cbk);
    odmp_register_audio_data_cbk(odmp_audio_data_cbk); /* 注册音频数据回调 */
    /* 设置VAD回调 */
    recorder_set_vad_cbk(g_app_ctx->recorder, recorder_vad_cb, NULL);
    /* 默认不设置录音上传回调 */
    wss_app_recorder_data_upstream(recorder_data_to_ringbuffer);
    /* 开启录音 */
    wss_app_recorder_en(ENABLE);

    /* 重置 */
    g_exception_exit = false;
    
    ASSERT(g_mode_state == STATE_MODE_IDLE);

#if 0
    log_i("Start create session task.\n");

    /* 初始化 Create session 任务上下文 */
    g_session_create_state = STATE_CREATE_ROLE_SESSION_STARTUP;
    g_session_create_idx = 0;

    log_i("IDLE -> CREATE_SESSION\n");
    g_mode_state = STATE_MODE_CREATE_SESSION;
#endif

    char *send_data = odmp_req_message_initial_setup(NULL, 0, "CC1CC1CC1CC1", "");
    wss_server_send(g_app_ctx->websockets_info, send_data, strlen(send_data));
    log_i("%s", send_data);
    free(send_data);

    g_mode_state = STATE_MODE_TEST;
    g_setup_complete = false;
    g_test_state = STATE_TEST_STEP_1;
    log_i("IDLE -> TEST(STEP_1)\n");
    
    // g_recording = false;
    // log_i("PLAYINTRO -> ONEONONE(WAIT_VAD)\n");
    // g_mode_state = STATE_MODE_ONEONONE;
    // g_oneonone_state = STATE_ONEONONE_WAIT_VAD;
}

void wss_application_uninit()
{
    log_i("Uninit WSS application\n");
    my_blocking_queue_destroy_static(&g_asr_data_queue);
    my_ring_buffer_destroy_static(&g_asr_ringbuffer);

    /* 取消注册lwl事件处理回调 */
    lwl_event_handler_remove(wss_app_lwl_event_handler_cb);
    /* 取消注册loswssp事件回调 */
    loswssp_register_rsp_cbk(NULL);
    /* 设置VAD回调 */
    recorder_set_vad_cbk(g_app_ctx->recorder, NULL, NULL);
    /* 取消录音上传回调 */
    wss_app_recorder_data_upstream(NULL);
    /* 开启录音 */
    wss_app_recorder_en(DISABLE);
}

bool wss_application_loop(struct websocket_struct *websockets_info)
{
    g_curr_time = timer_get_ms();
    ASSERT(websockets_info != NULL);

    msleep(10);

    if (g_curr_time - g_heat_timer >= HEAT_TIMEOUT) {

        int pp = 2;
        lwl_app_context_t *context = lwl_app_core_context_get_instance();
        if (context && context->recorder) {
            pp = recorder_get_play_pause_state(context->recorder);
        }

        g_heat_timer = g_curr_time;
    }

    if (wss_application_check_exit()) {
        return true;
    }
    return false;
}
