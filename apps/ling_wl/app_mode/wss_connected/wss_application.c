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

#include "protocol/odm/odm_secure.h"

static void wss_app_recorder_data_upstream(recorder_data_cb_t cb);
static void recorder_data_to_data_queue(void* priv, void *data, u32 size);
static void recorder_data_to_ringbuffer(void* priv, void *data, u32 size);
static void wss_asr_data(void *data, u32 size);
static void app_record_task_start();
static void app_task_break();
static void start_audio_upload();
static void wss_application_event_to_req(lwl_eend_dev_t *p_dev, int msg_event);

#define ENABLE 1
#define DISABLE 0


static u32 g_curr_time = 0;
#define HEAT_TIMEOUT 5000
static u32 g_heat_timer = 0;
#define WAIT_AUDIO_RESP_TIMEOUT 5000
static u32 g_wait_audio_resp_timer = 0;

static bool g_test_mode = false;

static int g_exception_exit = false;

static lwl_app_context_t *g_app_ctx = NULL;

static lwl_eend_dev_t *g_edev_closest = NULL; // C 本地维护
static lwl_eend_dev_t *g_edev_last = NULL; // C 本地维护
static lwl_eend_dev_t *g_edev_target = NULL; // 服务器维护
static lwl_eend_dev_t *g_edev_connected = NULL; // E 已连接


/* Task state */
typedef enum {
    STATE_IDLE,
    STATE_ONLINE,

    STATE_PLAYINTRO_SWITCH,
    STATE_PLAYINTRO_PLAYING,
    STATE_PLAYINTRO_VAD,

    STATE_ONEONONE_PLAYING,
    STATE_ONEONONE_SWITCH,
    STATE_ONEONONE_VAD,

    STATE_COMMAND_BUTTON,
    STATE_COMMAND_WAITING,

} AppTaskStepState;

static int g_mode_state = STATE_IDLE;

#define STEP_TO_NEXT_STEP(cur,next) if (g_mode_state == cur) {g_mode_state = next;wss_application_print_state("STEP_TO");}

static int g_audio_playing = false;
static int g_audio_ready = false;
static int g_audio_play_ok = false;

static void wss_application_print_state(char* tag)
{
    switch (g_mode_state) {
    case STATE_IDLE:
        log_i("[%s]STATE_IDLE", tag);
        break;
    case STATE_ONLINE:
        log_i("[%s]STATE_ONLINE", tag);
        break;
    case STATE_PLAYINTRO_SWITCH:
        log_i("[%s]STATE_PLAYINTRO_SWITCH", tag);
        break;
    case STATE_PLAYINTRO_PLAYING:
        log_i("[%s]STATE_PLAYINTRO_PLAYING", tag);
        break;
    case STATE_PLAYINTRO_VAD:
        log_i("[%s]STATE_PLAYINTRO_VAD", tag);
        break;
    case STATE_ONEONONE_SWITCH:
        log_i("[%s]STATE_ONEONONE_SWITCH", tag);
        break;
    case STATE_ONEONONE_PLAYING:
        log_i("[%s]STATE_ONEONONE_PLAYING", tag);
        break;
    case STATE_ONEONONE_VAD:
        log_i("[%s]STATE_ONEONONE_VAD", tag);
        break;
    case STATE_COMMAND_BUTTON:
        log_i("[%s]STATE_COMMAND_BUTTON", tag);
        break;
    case STATE_COMMAND_WAITING:
        log_i("[%s]STATE_COMMAND_WAITING", tag);
        break;
    }
}

static void wss_application_print_event(int msg_event, char* tag)
{
    switch (msg_event) {
    case ORMET_INITIAL_SETUP:
        log_i("[%s]ORMET_INITIAL_SETUP", tag);
        break;
    case ORMET_NEAR_TRIGGER:
        log_i("[%s]ORMET_NEAR_TRIGGER", tag);
        break;
    case ORMET_NEW_DEVICE_IN:
        log_i("[%s]ORMET_NEW_DEVICE_IN", tag);
        break;
    case ORMET_SWITCH_SUCCESS:
        log_i("[%s]ORMET_SWITCH_SUCCESS", tag);
        break;
    case ORMET_SWITCH_FAILED:
        log_i("[%s]ORMET_SWITCH_FAILED", tag);
        break;
    case ORMET_BUTTON_TRIGGER:
        log_i("[%s]ORMET_BUTTON_TRIGGER", tag);
        break;
    case ORMET_BUTTON_PRESS:
        log_i("[%s]ORMET_BUTTON_PRESS", tag);
        break;
    case ORMET_BUTTON_DOUBLE_PRESS:
        log_i("[%s]ORMET_BUTTON_DOUBLE_PRESS", tag);
        break;
    case ORMET_BUTTON_START:
        log_i("[%s]ORMET_BUTTON_START", tag);
        break;
    case ORMET_BUTTON_END:
        log_i("[%s]ORMET_BUTTON_END", tag);
        break;
    case ORMET_MOVE_PATROL:
        log_i("[%s]ORMET_MOVE_PATROL", tag);
        break;
    case ORMET_MOVE_STOP:
        log_i("[%s]ORMET_MOVE_STOP", tag);
        break;
    case ORMET_MOVE_SHAKE:
        log_i("[%s]ORMET_MOVE_SHAKE", tag);
        break;
    case ORMET_VAD_SPEAKER_START:
        log_i("[%s]ORMET_VAD_SPEAKER_START", tag);
        break;
    case ORMET_VAD_SPEAKER_END:
        log_i("[%s]ORMET_VAD_SPEAKER_END", tag);
        break;
    case ORMET_PLAY_START:
        log_i("[%s]ORMET_PLAY_START", tag);
        break;
    case ORMET_PLAY_NEAR_END:
        log_i("[%s]ORMET_PLAY_NEAR_END", tag);
        break;
    case ORMET_PLAY_END:
        log_i("[%s]ORMET_PLAY_END", tag);
        break;
    }
}

/*=================================================================================================
 * E switch context
 *=================================================================================================*/
static int g_backend_switch_session_ok = false;
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

int vad_start_time = 0;
int vad_timeout = 0;

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

static void start_audio_upload(void *priv)
{
    log_i("Start audio upload.");
    int recording_upload_package_index = 0;
    wss_app_recorder_data_upstream(recorder_data_to_data_queue); /* 录音数据 -> asr_data_queue */
    if (send_audio_status(DEVICE_ID, OAST_ASR, OASE_START)) {
        wss_application_exception_exit();
        return;
    }

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
            
            if (send_audio_status(DEVICE_ID, OAST_ASR, OASE_END)) {
                wss_application_exception_exit();
                return;
            }
            wss_app_recorder_en(DISABLE);
            return;
        }
        msleep(10);
    }
}

static void wait_vad_task(void *priv)
{
    wss_app_recorder_en(ENABLE);
    log_i("Wait VAD start %d\n", vad_timeout - timer_get_ms());

    while (vad_timeout >= timer_get_ms()) {
        msleep(10);

        if (g_recording == true) {
            threadpool_add_task(g_app_ctx->threadpool, start_audio_upload, NULL);
            return;
        }
    }

    log_i("Wait VAD timeout.");
    wss_app_recorder_en(DISABLE);
    g_recording = false;
    // g_edev_last = NULL;
    wss_application_event_to_req(NULL, ORMET_VAD_SPEAKER_END);
}

static int odmp_audio_data_cbk(uint8_t *data, uint32_t len)
{
    int ret = app_music_play_stream_write(g_app_ctx->music_player, data, len);
    log_d("stream write: %d\n", ret);
    return 0;
}


//=================================================================================================
// MUSIC response callback
//=================================================================================================
void app_music_play_end_cb(void *priv)
{
    log_i("g_audio_playing End");
    // uart_send_set_sn("000000000000000");
    wss_application_event_to_req(NULL, ORMET_PLAY_END);
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

//=================================================================================================
// pDev response callback
//=================================================================================================
static void e_turn_on_cb(bka_cmd_code_e code, void *priv)
{
    lwl_eend_dev_t *pdev = (lwl_eend_dev_t*)(priv);
    
    char sn_string[SN_SIZE * 2 + 1] = "(NULL)";
    if (pdev) {
        pdev->get_string(pdev, sn_string, sizeof(sn_string));
    }
    switch(code) {
    case BKA_CMD_CODE_COMPL:

        log_i("Switch PortE %s OK\n", sn_string);
        wss_application_event_to_req(pdev, ORMET_SWITCH_SUCCESS);
        break;
    case BKA_CMD_CODE_DEVLOST:
        log_i("Switch PortE Dev Lost\n");
        break;
    case BKA_CMD_CODE_BUFFER_FULL:
        log_i("Switch PortE BLE TX Buffer Full\n");
        break;
    case BKA_CMD_CODE_SEARCH_PROFILE_TIMEOUT:
        log_i("Switch PoertE search GATT timeout\n");
        break;
    case BKA_CMD_CODE_E_TURN_ON_TIMEOUT:
        log_i("Switch PortE Timeout\n");
        break;
    case BKA_CMD_CODE_E_TURN_ON_ALREADY_BOUND:
        log_i("Switch PortE Failed: E is already bound\n");
        break;
    case BKA_CMD_CODE_UNEXPECTED:
        log_i("Switch PortE Error\n");
        break;
    }
    if (code != BKA_CMD_CODE_COMPL) {
        wss_application_event_to_req(pdev, ORMET_SWITCH_FAILED);
    }
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
        STEP_TO_NEXT_STEP(STATE_IDLE,                       STATE_ONLINE);
        break;
    case OR_DEVICE_SWITCH_RESPONSE:
        log_i("DEVICE_SWITCH_RESPONSE\n");
        log_i("device_id: %s\n", pkg->device_switch_response.device_id);
        log_i("status: %s\n", pkg->device_switch_response.status);
        break;
    case OR_TTS_BIND: {
        log_i("TTS_BIND\n");
        log_i("c_id: %s\n", pkg->tts_bind.c_id);
        log_i("send_id: %s\n", pkg->tts_bind.send_id);
        log_i("wait_time: %d\n", pkg->tts_bind.wait_time);

        if (pkg->tts_bind.wait_time > 0) {
            vad_timeout = vad_start_time + pkg->tts_bind.wait_time;
        }

        // send_id 和E端序列号的关系?
        // 1. 把 send_id 默认为E端的序列号
        // 2. 把 send_id 映射为E端的序列号

        char *sn = pkg->tts_bind.send_id;
        // if (!sn) break;

        lwl_eend_dev_t *pdev = find_eend_dev_by_sn(sn);
        pdev->handshake_info.need_handshake = true;

        if (pdev == NULL) {
            log_e("dev not exists: %s", sn);
        }else {
            uint32_t cmd_id;
            g_edev_target = pdev;
            STEP_TO_NEXT_STEP(STATE_ONLINE,                 STATE_ONEONONE_SWITCH);// Turn to One on one mode when use C
            // STEP_TO_NEXT_STEP(STATE_ONLINE,                 STATE_PLAYINTRO_SWITCH);
            STEP_TO_NEXT_STEP(STATE_PLAYINTRO_VAD,          STATE_ONEONONE_SWITCH);
            STEP_TO_NEXT_STEP(STATE_ONEONONE_VAD,           STATE_ONEONONE_SWITCH);
            // bka_e_turn_on(pdev, e_turn_on_cb, pdev, &cmd_id);
            // log_d("turn on cmd_id: %d", cmd_id);
            log_i("Switch PortC OK\n");
            wss_application_event_to_req(pdev, ORMET_SWITCH_SUCCESS);
        }
    }   break;
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
                g_audio_playing = true;
                STEP_TO_NEXT_STEP(STATE_PLAYINTRO_SWITCH,   STATE_PLAYINTRO_PLAYING);
                STEP_TO_NEXT_STEP(STATE_ONEONONE_SWITCH,    STATE_ONEONONE_PLAYING);
                app_music_play_stream_music_with_callback(g_app_ctx->music_player, app_music_play_end_cb, NULL);
            } else if (pkg->audio_status.event == OASE_END) {
                log_i("AUDIO_STATUS: OASE_END\n");
            } else if (pkg->audio_status.event == OASE_ABNORMAL) {
                if (g_audio_playing) {
                    // TODO STOP talking
                }
            } else if (pkg->audio_status.event == OASE_SENTENCE_START) {
            } else if (pkg->audio_status.event == OASE_SENTENCE_END) {
            }
        }
        break;
    case OR_MESSAGE_TEXT:
        log_i("MESSAGE_TEXT %s %s\n", pkg->message_text.c_id, pkg->message_text.text);
        break;
    case OR_EXECUTION_TIME:
        log_i("EXECUTION_TIME %s %d\n", pkg->execution_time.event, pkg->execution_time.execution_time);
        break;
    case OR_PONG:
        log_i("LR_PONG\n");
        break;
    }
}

//=================================================================================================
// LWL event callback
//=================================================================================================
static void wss_app_lwl_event_handler_cb(lwl_event_t *e)
{
    static int en = 1;
    lwl_eend_dev_t *p_eend_dev = e->distance_change.pdev;
    switch (e->event_type) {
    case LWL_EVENT_TYPE_EEND_DEV_CLOSEST_CHANGE: {
        if (vad_timeout < timer_get_ms()) {
            g_edev_closest = p_eend_dev;
        }
    }
    // break;
    case LWL_EVENT_TYPE_EEND_DEV_DISTANCE_CHANGE: {
        char sn_string[SN_SIZE * 2 + 1] = "(NULL)";
        p_eend_dev->get_string(p_eend_dev, sn_string, sizeof(sn_string));
        log_i("[MEASDIST] %s: %d %d\r\n", sn_string, p_eend_dev->distance_info.rssi, p_eend_dev->distance_info.distance_type);

        switch (p_eend_dev->distance_info.distance_type) {
        case DISTANCE_0:
            log_d("[MEASDIST] distance < 2m : do things1 here --- %s\r\n", sn_string);

            wss_application_event_to_req(p_eend_dev, ORMET_NEAR_TRIGGER);
            break;
        case DISTANCE_1:
            log_d("[MEASDIST] distance > 2m  < 5m do things2 here ---%s\r\n", sn_string);
            break;
        case DISTANCE_2:
            log_d("[MEASDIST] distance > 5m do things3 here ---%s\r\n", sn_string);

            if (p_eend_dev == g_edev_last) {
                if (g_audio_playing) {
                    // TODO STOP talking
                }
                wss_application_event_to_req(p_eend_dev, ORMET_DEVICE_LEAVE);
                g_edev_last = NULL;
            } 
            break;
        case DISTANCE_3:
            log_d("[MEASDIST] not found ---%s\r\n", sn_string);
            
            if (p_eend_dev == g_edev_last) {
                wss_application_event_to_req(p_eend_dev, ORMET_DEVICE_LEAVE);
                g_edev_last = NULL;
            }
            break;
        }
    }   break;
    case LWL_EVENT_TYPE_EEND_DEV_NEW_IN:
        log_i("ORMET_NEW_DEVICE_IN");
        wss_application_event_to_req(p_eend_dev, ORMET_NEW_DEVICE_IN);
        break;
    case LWL_EVENT_TYPE_UART_IO_KEY_SHORT_CLICK: {
        log_i("SHORT_CLICK");

    }   break;
    case LWL_EVENT_TYPE_UART_SYNC:
        log_i("SYNC");
        log_i("battery: %d", e->uart_cmd_sync.battery);
        uart_response_synch_status(1, 12);
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


void wss_application_init()
{
    log_i("Init WSS application\n");
    my_blocking_queue_init_static(&g_asr_data_queue, g_asr_data_queue_buffer, sizeof(AsrData), MY_ASR_DATA_QUEUE_CAPACITY);
    my_ring_buffer_init_static(&g_asr_ringbuffer, g_asr_ringbuffer_buffer, sizeof(AsrData), MY_ASR_RING_BUFFER_CAPACITY);

    g_app_ctx = lwl_app_core_context_get_instance();

    /* UART指令开启C端le audio */
    uart_send_open_le_audio(g_app_ctx->mac);

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
    wss_app_recorder_en(DISABLE);
    g_audio_playing = false;

    /* 重置 */
    g_exception_exit = false;
    g_edev_last = NULL; 

    g_mode_state = STATE_IDLE;
    wss_application_event_to_req(NULL, ORMET_INITIAL_SETUP);
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
        dump_all_eend_info();
        g_heat_timer = g_curr_time;
    }

    if (wss_application_check_exit()) {
        return true;
    }
    return false;
}

static void wss_application_event_to_req(lwl_eend_dev_t *p_dev, int msg_event)
{
    wss_application_print_event(msg_event, "wss_application_event_to_req");
    switch (g_mode_state) {
    case STATE_IDLE:
        if (msg_event != ORMET_INITIAL_SETUP &&
            msg_event != ORMET_NEAR_TRIGGER) return;
            STEP_TO_NEXT_STEP(STATE_IDLE,                   STATE_ONLINE);// TODO
            gpio_direction_output(IO_PORTA_00, 1);  //enable C speaker
            gpio_direction_output(IO_PORTB_02, 0);  // disable E speaker
        break;
    case STATE_ONLINE:
        if (msg_event != ORMET_BUTTON_TRIGGER &&
            msg_event != ORMET_NEAR_TRIGGER) return;
        if (g_edev_closest == NULL) return;
        if (p_dev == g_edev_closest && g_edev_last != p_dev) {
            g_edev_last = p_dev;
        } else return;
        break;
    case STATE_PLAYINTRO_SWITCH:
        if (msg_event != ORMET_BUTTON_TRIGGER &&
            msg_event != ORMET_SWITCH_SUCCESS &&
            msg_event != ORMET_SWITCH_FAILED) return;
        if (msg_event == ORMET_SWITCH_FAILED) {
            if (p_dev == g_edev_target) g_edev_target = NULL;
            if (p_dev == g_edev_last  ) g_edev_last   = NULL;
            STEP_TO_NEXT_STEP(STATE_PLAYINTRO_SWITCH,       STATE_ONLINE);
        } 
        if (msg_event == ORMET_SWITCH_SUCCESS) {
            g_edev_last = p_dev;
            // gpio_direction_output(IO_PORTA_00, 0);  //Disable C speaker
            // gpio_direction_output(IO_PORTB_02, 1);
        }
            
        // if (!g_edev_target == p_dev) return;
        break;
    case STATE_PLAYINTRO_PLAYING:
        if (msg_event != ORMET_BUTTON_TRIGGER &&
            msg_event != ORMET_PLAY_NEAR_END &&
            msg_event != ORMET_PLAY_END) return;
        if (msg_event == ORMET_PLAY_END) {
            g_audio_playing = false;
            
            // 等待 VAD 超时
            log_i("ENABLE VAD");
            vad_start_time = timer_get_ms();
            vad_timeout = vad_start_time + 5000;
            threadpool_add_task(g_app_ctx->threadpool, wait_vad_task, NULL);
            STEP_TO_NEXT_STEP(STATE_PLAYINTRO_PLAYING,      STATE_PLAYINTRO_VAD);
        }
        break;
    case STATE_PLAYINTRO_VAD:
        if (msg_event != ORMET_BUTTON_TRIGGER &&
            msg_event != ORMET_VAD_SPEAKER_END &&
            msg_event != ORMET_NEW_DEVICE_IN &&
            msg_event != ORMET_DEVICE_LEAVE) return;

        if (msg_event == ORMET_VAD_SPEAKER_END) {// TODO
            STEP_TO_NEXT_STEP(STATE_PLAYINTRO_VAD,          STATE_ONLINE);
            return;
        }
        break;
    case STATE_ONEONONE_SWITCH:
        if (msg_event != ORMET_BUTTON_TRIGGER &&
            msg_event != ORMET_SWITCH_SUCCESS &&
            msg_event != ORMET_SWITCH_FAILED) return;
        if (msg_event == ORMET_SWITCH_FAILED || g_edev_last != p_dev) {
            if (p_dev == g_edev_target) g_edev_target = NULL;
            if (p_dev == g_edev_last  ) g_edev_last   = NULL;
            STEP_TO_NEXT_STEP(STATE_ONEONONE_SWITCH,        STATE_ONLINE);
        } 
        break;
    case STATE_ONEONONE_PLAYING:
        if (msg_event != ORMET_BUTTON_TRIGGER &&
            msg_event != ORMET_PLAY_NEAR_END &&
            msg_event != ORMET_PLAY_END) return;
        if (msg_event == ORMET_PLAY_END) {
            g_audio_playing = false;
            
            // 等待 VAD 超时
            log_i("ENABLE LONG VAD");
            vad_start_time = timer_get_ms();
            vad_timeout = vad_start_time + 60000;
            threadpool_add_task(g_app_ctx->threadpool, wait_vad_task, NULL);
            STEP_TO_NEXT_STEP(STATE_ONEONONE_PLAYING,       STATE_ONEONONE_VAD);
        }
        break;
    case STATE_ONEONONE_VAD:
        if (msg_event != ORMET_BUTTON_TRIGGER &&
            msg_event != ORMET_DEVICE_LEAVE) return;
        break;
    case STATE_COMMAND_BUTTON:
        if (msg_event != ORMET_BUTTON_PRESS &&
            msg_event != ORMET_BUTTON_DOUBLE_PRESS &&
            msg_event != ORMET_BUTTON_START &&
            msg_event != ORMET_BUTTON_END) return;
        break;
    case STATE_COMMAND_WAITING:
        break;
    }
    while (send_req_message(DEVICE_ID, g_edev_last ? g_edev_last->sn : "", g_audio_playing, msg_event)) {
        wss_server_reconnect();
        // wss_application_exception_exit();
        // return;
    }
}