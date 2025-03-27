#include "wss_application.h"
#include "wss_app_util.h"
#include "cpu.h"

#include "util/cqueue.h"
#include "util/ringbuffer.h"
#include "protocol/lingos/loswssp.h"
#include "bt_ble/client/ble_distance.h"
#include "async_msg_loop.h"
#include "app_mode/lwl_app_core.h"
#include "gpio.h"
#include "device.h"
#include "util/threadpool_tasks.h"

#include "ota/http_ota.h"
#include "uart.h"

static void wss_app_recorder_data_upstream(recorder_data_cb_t cb);
static void recorder_data_to_data_queue(void* priv, void *data, u32 size);
static void recorder_data_to_ringbuffer(void* priv, void *data, u32 size);
static void wss_application_switch_test_mode();
static void wss_asr_data(void *data, u32 size);
static void wss_asr_data_to_ringbuffer(void *data, u32 size);
static void app_record_task_start();
static void app_task_break();

void *g_hdl;

#define ENABLE 1
#define DISABLE 0

#define DUMMY_SN "000000000000000"

typedef enum {
    STATE_MODE_IDLE,
    STATE_MODE_CREATE_SESSION,
    STATE_MODE_PLAYINTRO,
    STATE_MODE_ONEONONE,
    STATE_MODE_ONEONMANY,
} AppModeState;

typedef enum {
    STATE_CREATE_ROLE_SESSION_STARTUP,
    STATE_CREATE_ROLE_SESSION_CREATING,
    STATE_CREATE_ROLE_SESSION_OVER,

    STATE_PLAYINTRO_STARTUP,
    STATE_PLAYINTRO_SWITCH,
    STATE_PLAYINTRO_PLAYING,
    STATE_PLAYINTRO_WAIT_VAD,

    STATE_ONEONONE_WAIT_VAD,
    STATE_ONEONONE_RECORDING,
    STATE_ONEONONE_PLAYING,

    STATE_ONEONMANY_IDLE,

} AppTaskStepState;

typedef struct {
    lwl_eend_dev_t *pdev;
    int ip_id;
    int session_id;
    int session_create_ok;
    int hello_flag; //表示已经打过招呼了
    int distance_type;  // DISTANCE_0：小于2米，DISTANCE_1：2米到5米，DISTANCE_2：大于5米，DISTANCE_3：RSSI等于-128或没有检测到
    s8  rssi;
} communication_edev_t;

static u32 g_curr_time = 0;
#define HEAT_TIMEOUT 5000
static u32 g_heat_timer = 0;
#define WAIT_AUDIO_RESP_TIMEOUT 5000
static u32 g_wait_audio_resp_timer = 0;

static bool g_test_mode = false;

static int g_exception_exit = false;

static lwl_app_context_t *g_app_ctx = NULL;

/* Task state */
static int g_mode_state = STATE_MODE_IDLE;

/* Create session context */
static int g_session_create_state = STATE_CREATE_ROLE_SESSION_STARTUP;
static int g_session_create_idx = 0;

/* Playintro context */
static int g_playintro_state = STATE_PLAYINTRO_SWITCH;
static int g_move_more_than_one_meter = false;

/* Oneonone context */
static int g_oneonone_state = STATE_ONEONONE_WAIT_VAD;
static int g_wait_resp_flag = false;

/* Oneonmany context */
static int g_oneonmany_state = STATE_ONEONMANY_IDLE; 


/* LingOS 对话上下文 */
#define COMMUNICATION_AUDIO_LIST_MAX_SIZE 10
static int  g_communication_message_id = 0;
static char g_communication_audio_list[COMMUNICATION_AUDIO_LIST_MAX_SIZE][256];
static int  g_communication_audio_cnt = 0;

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
static int g_curr_edev_idx = -1;
static int g_next_edev_idx = -1;

/*=================================================================================================
 * Audio dec app context
 *=================================================================================================*/
static int g_audio_play_end = false;
static int g_audio_play_ongoing_num = 0;
static int g_backend_message_end_ok = false;    // lingos message end event

/*=================================================================================================
 * Audio enc app context
 *=================================================================================================*/
#define ASR_DATA_CHUNK_SIZE 1280
typedef struct {
    u8 data[ASR_DATA_CHUNK_SIZE];
} AsrData;

#define MY_ASR_DATA_QUEUE_CAPACITY 24
static MyBlockingQueue g_asr_data_queue; // MyBlockingQueue<AsrData>
static u8 g_asr_data_queue_buffer[MY_ASR_DATA_QUEUE_CAPACITY * sizeof(AsrData)];

#define MY_ASR_RING_BUFFER_CAPACITY 24   // 24 = floor(((1000ms / 1s) * 2byte * 16000Hz) / 1280byte)
static MyRingBuffer g_asr_ringbuffer;    // MyRingBuffer<AsrData>
static u8 g_asr_ringbuffer_buffer[MY_ASR_RING_BUFFER_CAPACITY * sizeof(AsrData)];

static int g_recording = false;
static AsrData asr_data_frame;

#define BASE64_BUF_MAX_SIZE 1709    // ((ASR_DATA_CHUNK_SIZE + 2) / 3) * 4 = 1708
static char base64_buf[BASE64_BUF_MAX_SIZE];

static int g_recording_upload_package_index = 0;

/*=================================================================================================
 * message queue
 *=================================================================================================*/
typedef enum {
    APPMSG_SHORT_CLICK,
} AppMessageEnum;

typedef struct {
    int type;
} AppMessage;

#define MY_MESSAGE_QUEUE_CAPACITY 10
static MyBlockingQueue g_msg_queue; // MyBlockingQueue<AsrData>
static u8 g_msg_queue_buffer[MY_MESSAGE_QUEUE_CAPACITY * sizeof(AppMessage)];

/*=================================================================================================
 * App edev table
 *=================================================================================================*/
static int g_edev_closest_idx = 0;  // 当前距离更近的设备索引
static int g_edev_count = 0;
static communication_edev_t g_edev_table[EEND_MAX_CNT];

static void add_edev(lwl_eend_dev_t *p_eend_dev, int ip_id)
{
    if (g_edev_count >= EEND_MAX_CNT) {
        log_e("eend max cnt: %d", EEND_MAX_CNT);
        return;
    }
    int i = g_edev_count++;
    g_edev_table[i].pdev = p_eend_dev;
    g_edev_table[i].ip_id = ip_id;
    g_edev_table[i].session_id = 0;
    g_edev_table[i].session_create_ok = false;
    g_edev_table[i].hello_flag = false;
    g_edev_table[i].distance_type = -1;
}

static uint32_t get_edev_count()
{
    return g_edev_count;
}

static int find_edev_idx(lwl_eend_dev_t *p_eend_dev) 
{
    for (int i = 0; i < g_edev_count; ++i) {
        if (g_edev_table[i].pdev == p_eend_dev) {
            return i;
        }
    }
    return -1;
}

static int  get_2m_edev_count()
{
    int n = 0;
    for (int i = 0; i < g_edev_count; ++i) {
        if ((g_edev_table[i].distance_type == DISTANCE_0)) {
            n++;
        }
    }
    return n;
}

static int  get_5m_edev_count()
{
    int n = 0;
    for (int i = 0; i < g_edev_count; ++i) {
        if ((g_edev_table[i].distance_type == DISTANCE_0) ||
            (g_edev_table[i].distance_type == DISTANCE_1)) {
            n++;
        }
    }
    return n;
}

static bool is_edev_far_away(int devIndex)
{
    return (g_edev_table[devIndex].distance_type == DISTANCE_2) || (g_edev_table[devIndex].distance_type == DISTANCE_3);
}

static void wss_application_exception_exit()
{
    g_exception_exit = true;
}

static bool wss_application_check_exit()
{
    return g_exception_exit;
}

/*=================================================================================================
 * Timeout timer
 *=================================================================================================*/
static bool g_timeout_timer_ongoing = false;
static bool g_timeout_timer_is_timeout = false;
static u32  g_timeout_timer_start_time = 0;
static u32  g_timeout_timer_limit = 0;

static void timeout_timer_init(u32 msec)
{
    g_timeout_timer_start_time = timer_get_ms();
    g_timeout_timer_limit = msec;
    g_timeout_timer_is_timeout = false;
    g_timeout_timer_ongoing = true;
}

static void timeout_timer_update()
{
    if ((g_curr_time - g_timeout_timer_start_time) >= g_timeout_timer_limit) {
        g_timeout_timer_ongoing = false;
        g_timeout_timer_is_timeout = true;
    }
}

static bool timeout_timer_is_timeout()
{
    return g_timeout_timer_is_timeout;
}

//=================================================================================================
// Async task callback
//=================================================================================================
static void audio_play_end_callback(int state, pl_music_info_t *music_info, void *priv)
{
    communication_edev_t *pedev = (communication_edev_t *)priv;
    switch (state) {
    case PLAY_AUDIO_STATE_BEFORE_DEC_START:
        log_i("Audio play begin\n");
        uart_send_set_sn(pedev->pdev->sn);  /* 广播开始播放 */
        break;
    case PLAY_AUDIO_STATE_AFTER_DEC_END:
        log_i("Audio play over\n");
        uart_send_set_sn(DUMMY_SN);  /* 广播停止播放 */
        g_audio_play_end = true;
        g_audio_play_ongoing_num--;
        log_i("Audio play ongoing num: %d\n", g_audio_play_ongoing_num);
        break;
    case PLAY_AUDIO_STATE_CANCEL:
        log_i("Audio play cancel\n");
        g_audio_play_ongoing_num--;
        break;
    }
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

//=================================================================================================
// WSS response callback
//=================================================================================================
static int loswssp_rsp_cbk(int type, loswssp_package_t *pkg)
{
    switch (type) {
    case LR_SESSION:
        log_i("LR_SESSION\n");
        if (pkg->session.new_session) {
            int session_id = pkg->session.session_id;

            if (g_mode_state == STATE_MODE_CREATE_SESSION) {
                if (g_session_create_state == STATE_CREATE_ROLE_SESSION_CREATING) {
                    log_i("get role[%d] session id: %d\n", g_session_create_idx, session_id);
                    g_edev_table[g_session_create_idx].session_id = session_id;
                }
            }
        } else {
            /* 请求后端切换session时，若session已存在，则new_session字段为False */
            if (g_mode_state == STATE_MODE_PLAYINTRO) {
                if (g_playintro_state == STATE_PLAYINTRO_SWITCH) {
                    g_backend_switch_session_ok = true;
                }
            }
        }
        break;
    case LR_MESSAGE_ASR_DATA:
        log_i("LR_MESSAGE_ASR_DATA\n");
        break;
    case LR_MESSAGE_ASR_END: {
        log_i("LR_MESSAGE_ASR_END\n");
        int vmessage_id = pkg->message_asr_end.message_id;
        log_i("message id     :%d", vmessage_id);
        g_communication_message_id = vmessage_id;
    }   break;
    case LR_MESSAGE_DELTA:
        log_i("LR_MESSAGE_DELTA\n");
        break;
    case LR_MESSAGE_AUDIO: {
        log_i("LR_MESSAGE_AUDIO\n");
        int vmessage_id = pkg->message_audio.message_id;
        log_i("message id     :%d", vmessage_id);
        int audio_num = pkg->message_audio.audio_num;
        log_i("audio num      :%d", audio_num);
        for (int i = 0; i < audio_num; ++i) {
            char* vcontent = pkg->message_audio.audios[i].content;
            char* vurl = pkg->message_audio.audios[i].url;

            if (g_mode_state == STATE_MODE_ONEONONE) {
                if (g_oneonone_state == STATE_ONEONONE_PLAYING) {
                    /* 保存对话内容，用于之后和另一个角色对话 */
                    log_i("latest content :%s", vcontent);
                    log_i("latest url     :%s", vurl);

                    /* 保存到记录，用于按键重播 */
                    strncpy(g_communication_audio_list[g_communication_audio_cnt], vurl, sizeof(g_communication_audio_list[0]));
                    g_communication_audio_cnt++;

                    /* 执行任务播放音乐，并注册回调，播放完成时处理 */
                    my_task_play_one_lingos(vmessage_id, vurl, audio_play_end_callback, &g_edev_table[g_curr_edev_idx]);
                    g_audio_play_ongoing_num++;

                    g_wait_resp_flag = true;
                }
            } else if (g_mode_state == STATE_MODE_CREATE_SESSION) {
                if (g_session_create_state == STATE_CREATE_ROLE_SESSION_CREATING) {
                    /* 保存开场白 */
                    lwl_app_context_t *ctx = lwl_app_core_context_get_instance();
                    save_role_opening_task_param_t *save_opening_param = (save_role_opening_task_param_t *)malloc(sizeof(save_role_opening_task_param_t));
                    save_opening_param->lfs = ctx->lfs_hdl;
                    save_opening_param->url = (char *)malloc(strlen(vurl) + 1);
                    if (save_opening_param->url == NULL) {
                        printf("Failed to allocate memory for vurl\n");
                        free(save_opening_param);
                        break;
                    }
                    strcpy(save_opening_param->url, vurl);
                    save_opening_param->role_name = g_edev_table[g_session_create_idx].ip_id;
                    sprintf(save_opening_param->opening_filename, "opening_%d.mp3", g_edev_table[g_session_create_idx].ip_id);
                    if (!threadpool_add_task(ctx->threadpool, save_role_opening_file_task, save_opening_param))
                    {
                        printf("!!!!!!!!!!!!!!!!!Failed to add save_role_opening_file_task  !!!!!!!!!!!!!!!!!!!!!\r\n");
                        free(save_opening_param->url);
                        free(save_opening_param); 
                        break;                  
                    }
                }
            }
        }
    }   break;
    case LR_MESSAGE_FILE:
        log_i("LR_MESSAGE_FILE\n");
        break;
    case LR_MESSAGE_END:
        log_i("LR_MESSAGE_END\n");

        if (pkg->errno != 0) {
            
            lwl_app_context_t *context = lwl_app_core_context_get_instance();
            log_i("url: %s", context->wss_url);

            log_i("Errno: %d", pkg->errno);
            if (pkg->errmsg) {
                log_i("Error message: %s", pkg->errmsg);
            }
        }

        if (g_mode_state == STATE_MODE_PLAYINTRO) {
            if (g_playintro_state == STATE_PLAYINTRO_PLAYING) {
                g_backend_message_end_ok = true;
            }
        } else if (g_mode_state == STATE_MODE_ONEONONE) {
            if (g_oneonone_state == STATE_ONEONONE_PLAYING) {
                g_backend_message_end_ok = true;
            }
        } else if (g_mode_state == STATE_MODE_CREATE_SESSION) {
            if (g_session_create_state == STATE_CREATE_ROLE_SESSION_CREATING) {
                g_edev_table[g_session_create_idx].session_create_ok = true;
            }
        }
        break;
    case LR_MESSAGE_STOP:
        log_i("LR_MESSAGE_STOP\n");
        log_i("stop message id: %d\n", pkg->message_stop.message_id);
        g_message_stop_ok = true;
        g_message_stop_message_id = pkg->message_stop.message_id;
        break;
    case LR_MESSAGE_POINT_DELTA:
        log_i("LR_MESSAGE_POINT_DELTA\n");
        break;
    case LR_PONG:
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
    switch (e->event_type) {
    case LWL_EVENT_TYPE_UART_IO_KEY_SHORT_CLICK: {
        log_i("SHORT_CLICK");

        AppMessage msg;
        msg.type = APPMSG_SHORT_CLICK;
        if (MYERROR_Is_Full == my_blocking_queue_try_enqueue(&g_msg_queue, &msg)) {
            log_e("Failed to add message APPMSG_SHORT_CLICK");
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
        g_move_more_than_one_meter = true;
        break;
    }
}

static bool wss_app_msg_short_click_handler() 
{
    struct websocket_struct *websockets_info = g_app_ctx->websockets_info;
    switch (g_mode_state) {
    case STATE_MODE_PLAYINTRO:
        switch (g_playintro_state) {
        case STATE_PLAYINTRO_PLAYING:
            app_music_play_stop(g_app_ctx->music_player);  /* 停止当前正在进行的解码 */
            
            g_recording = false; 

            /* 创建超时定时器 */
            timeout_timer_init(10 * 1000);

            /* 使能7016移动距离检测 */
            // TODO: 发送uart指令使能检测
            g_move_more_than_one_meter = false;

            log_i("[PLAYINTRO] PLAY_AUDIO -> WAIT_VAD\n");
            g_playintro_state = STATE_PLAYINTRO_WAIT_VAD;
            break;
        }
        break;
    case STATE_MODE_ONEONONE:
        switch (g_oneonone_state) {
        case STATE_ONEONONE_PLAYING:
            if (AUDIO_DEC_START == app_music_play_get_status(g_app_ctx->music_player)) { /* 解码播放器播放中 */
                if (communication_message_stop(websockets_info) != 0) { /* 发送message stop */
                    wss_application_exception_exit();
                    return true;
                }
                disable_specific_audio_message(g_communication_message_id); /* 禁用播放队列中相同message_id的网络音频播放任务(LingOS) */
                app_music_play_stop(g_app_ctx->music_player);  /* 停止当前正在进行的解码 */
                g_recording = false;
                log_i("[ONEONONE] PLAYING -> WAIT_VAD\n");
                g_oneonone_state = STATE_ONEONONE_WAIT_VAD;
                break;
            }
        case STATE_ONEONONE_RECORDING:
            wss_app_recorder_data_upstream(recorder_data_to_ringbuffer); /* 录音数据 -> asr_ringbuffer */
            if (g_recording_upload_package_index > 0) { /* 已经上传了部分asr data时，发送message stop事件结束 */
                if (communication_message_stop(websockets_info) != 0) { /* 发送message stop */
                    wss_application_exception_exit();
                    return true;
                }
            }
        case STATE_ONEONONE_WAIT_VAD:
            g_backend_message_end_ok = true;    /* 重播时忽略服务器状态 */
            g_wait_resp_flag = true;            /* 重播时忽略服务器状态 */
            
            g_audio_play_end = false;
            g_audio_play_ongoing_num = 0;

            for (int i = 0; i < g_communication_audio_cnt; ++i) {
                /* 执行任务播放音乐，并注册回调，播放完成时处理 */
                my_task_play_one_lingos(g_communication_message_id, g_communication_audio_list[i], audio_play_end_callback, &g_edev_table[g_curr_edev_idx]);
                g_audio_play_ongoing_num++;
                log_i("audio_url : %s\n", g_communication_audio_list[i]);
            }

            log_i("[ONEONONE] RECORDING -> PLAYING\n");
            g_oneonone_state = STATE_ONEONONE_PLAYING;
            break;
        }
        break;
    }
    return false;
}

//=================================================================================================
// Recorder
//=================================================================================================

static void recorder_data_to_data_queue(void* priv, void *data, u32 size) {
    wss_asr_data(data, size);
}

static void recorder_data_to_ringbuffer(void* priv, void *data, u32 size) {
    wss_asr_data_to_ringbuffer(data, size);
}

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
static void wss_asr_data(void *data, u32 size)
{
    log_i("vfs: %d", size);
    ASSERT(size == ASR_DATA_CHUNK_SIZE);

    // /* 调试输出数据量 */
    // static int cnt2 = 0;
    // cnt2 += ASR_DATA_CHUNK_SIZE;
    // log_d("cnt2: %d\n", cnt2);

    AsrData *asr_data = (AsrData *)data;
    if (MYERROR_Is_Full == my_blocking_queue_try_enqueue(&g_asr_data_queue, asr_data)) {
        log_e("[WSSAPP]asr data qeue full, failed to enquque asr data.");
    }
}

static void wss_asr_data_to_ringbuffer(void *data, u32 size)
{
    ASSERT(size == ASR_DATA_CHUNK_SIZE);

    // /* 调试输出数据量 */
    // static int cnt2 = 0;
    // cnt2 += ASR_DATA_CHUNK_SIZE;
    // log_d("cnt2: %d\n", cnt2);

    AsrData *asr_data = (AsrData *)data;
    my_ring_buffer_write(&g_asr_ringbuffer, asr_data);
}

static void wss_asr_data_to_server(struct websocket_struct *websockets_info, AsrData *asr_data)
{
    // /* 调试输出数据量 */
    // static int cnt1 = 0;
    // cnt1 += ASR_DATA_CHUNK_SIZE;
    // log_d("cnt1: %d\n", cnt1);
    g_recording_upload_package_index++;

    size_t write_len = 0;
    memset(base64_buf, 0, sizeof(base64_buf));
    int ret = mbedtls_base64_encode(base64_buf, BASE64_BUF_MAX_SIZE, &write_len, asr_data->data, ASR_DATA_CHUNK_SIZE);
    ASSERT(ret == 0);
    ASSERT(write_len == (BASE64_BUF_MAX_SIZE - 1));

    dev_write(g_hdl, asr_data, ASR_DATA_CHUNK_SIZE);
    log_i(">>");
    if (communication_message_asr_data(websockets_info, base64_buf) != 0) {
        wss_application_exception_exit();
        return;
    }
    log_i("--");
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

//=================================================================================================
// Distance measurement
//=================================================================================================
void app_function(disatance_info_t *distance_info)
{
    log_i("notify_function my\n");

    int idx = find_edev_idx(distance_info->p_eend_dev);
    g_edev_closest_idx = idx;
}

void self_function(disatance_info_t *distance_info, int distance_type)
{
    log_i("distance_type: %d %d\n", distance_type, distance_info->average_rssi);

    int idx = find_edev_idx(distance_info->p_eend_dev);
    if (idx == -1) {
        return;
    }
    g_edev_table[idx].distance_type = distance_type;
    g_edev_table[idx].rssi = distance_info->functions_rssi;
}

//=================================================================================================
// Idle
//=================================================================================================
static bool app_idle_loop()
{
    if (0 < get_2m_edev_count()) {
        g_next_edev_idx = g_edev_closest_idx;
        /* 初始化 playintro 任务上下文 */
        g_playintro_state = STATE_PLAYINTRO_STARTUP;
    
        log_i("IDLE -> PLAYINTRO\n");
        g_mode_state = STATE_MODE_PLAYINTRO;
    }
    return false;
}

//=================================================================================================
// Create session
//=================================================================================================
static bool app_create_session_loop()
{
    struct websocket_struct *websockets_info = g_app_ctx->websockets_info;
    switch (g_session_create_state) {
    case STATE_CREATE_ROLE_SESSION_STARTUP:
        /* wss 发起创建会话 */
        for (;g_session_create_idx < get_edev_count(); g_session_create_idx++) {
            if (g_edev_table[g_session_create_idx].session_create_ok == false && 
                g_edev_table[g_session_create_idx].ip_id != -1) {
                if (communication_init_role(websockets_info, g_edev_table[g_session_create_idx].ip_id) != 0) {
                    wss_application_exception_exit();
                    return true;
                }
                break;
            }
        }
        
        log_i("[Create session %d] STARTUP -> CREATING\n", g_session_create_idx);
        g_session_create_state = STATE_CREATE_ROLE_SESSION_CREATING;
        break;
    case STATE_CREATE_ROLE_SESSION_CREATING:
        if (g_edev_table[g_session_create_idx].session_create_ok) {
            /* 当前会话创建成功 */
            /* 切换到下一个设备关联的角色 */
            g_session_create_idx++;
            if (g_session_create_idx == get_edev_count()) {
                /* 全部创建完毕 */
                log_i("[Create session %d] CREATING -> OVER\n", g_session_create_idx);
                g_session_create_state = STATE_CREATE_ROLE_SESSION_OVER;
            } else {
                log_i("[Create session %d] CREATING -> STARTUP\n", g_session_create_idx);
                g_session_create_state = STATE_CREATE_ROLE_SESSION_STARTUP;
            }
        }
        break;
    case STATE_CREATE_ROLE_SESSION_OVER:
        g_mode_state = STATE_MODE_IDLE;
        log_i("CREATE_SESSION(OVER) -> IDLE\n");
        return true;
    }
    return false;
}

//=================================================================================================
// Playintro
//=================================================================================================
static bool app_playintro_loop()
{
    struct websocket_struct *websockets_info = g_app_ctx->websockets_info;
    switch (g_playintro_state) {
    case STATE_PLAYINTRO_STARTUP:
        /* 切换服务端会话 */
        g_backend_switch_session_ok = false;
        if (wss_2ip_switch(websockets_info, g_edev_table[g_next_edev_idx].session_id, 
            g_edev_table[g_next_edev_idx].ip_id) != 0) {
            wss_application_exception_exit();
            return true;
        }

        /* 切换 E端 */
        g_ble_edev_switch_ok = false;
        my_task_switch_only(g_edev_table[g_next_edev_idx].pdev->sn, 200, switch_end_callback, NULL);

        log_i("[PLAYINTRO] STARTUP -> SWITCH (%d -> %d)\n", g_curr_edev_idx, g_next_edev_idx);
        g_playintro_state = STATE_PLAYINTRO_SWITCH;
        break;
    case STATE_PLAYINTRO_SWITCH:
        if (g_backend_switch_session_ok && g_ble_edev_switch_ok) {
            /* 切换完毕 */
            g_curr_edev_idx = g_next_edev_idx;

            /* 清空按键重播的音频链接列表 */
            g_communication_audio_cnt = 0;

            /* 等待音频播放完毕 */
            g_audio_play_end = false;
            g_audio_play_ongoing_num = 0;

            /* 播放开场白 */
            char fname[16];
            snprintf(fname, sizeof(fname), "opening_%d.mp3", g_edev_table[g_curr_edev_idx].ip_id);
            my_task_play_one_lfs(fname, audio_play_end_callback, &g_edev_table[g_curr_edev_idx]);
            g_audio_play_ongoing_num++;
            
            log_i("[PLAYINTRO] SWITCH -> PLAY_AUDIO\n");
            g_playintro_state = STATE_PLAYINTRO_PLAYING;
        }
        break;
    case STATE_PLAYINTRO_PLAYING:
        if (g_audio_play_end == true && g_audio_play_ongoing_num == 0) {
            /* 音频播放完毕 */
            g_recording = false;

            /* 创建超时定时器 */
            timeout_timer_init(10 * 1000);

            /* 使能7016移动距离检测 */
            // TODO: 发送uart指令使能检测
            g_move_more_than_one_meter = false;

            log_i("[PLAYINTRO] PLAY_AUDIO -> WAIT_VAD\n");
            g_playintro_state = STATE_PLAYINTRO_WAIT_VAD;
        }
        break;
    case STATE_PLAYINTRO_WAIT_VAD:
        if (g_move_more_than_one_meter == true) { /* 设备移动大于1米 */
            if (g_curr_edev_idx != g_edev_closest_idx) { /* 最近设备不是当前对话设备，重新切换设备 */
                g_next_edev_idx = g_edev_closest_idx;
                log_i("[PLAYINTRO] WAIT_VAD -> STARTUP\n");
                g_playintro_state = STATE_PLAYINTRO_WAIT_VAD;
            }
        } else if (g_recording == true) { /* 触发VAD，开始一对一对话 */
            /* 清空asr data queue */
            my_blocking_queue_clear(&g_asr_data_queue);

            /* 设置录音上传回调 */
            wss_app_recorder_data_upstream(recorder_data_to_data_queue);
            g_recording_upload_package_index = 0;
            g_recording = true;

            log_i("PLAYINTRO -> ONEONONE(RECORDING)\n");
            g_mode_state = STATE_MODE_ONEONONE;
            g_oneonone_state = STATE_ONEONONE_RECORDING;
            return true;

        } else if (timeout_timer_is_timeout()) {  /* VAD 超时 */
            int around_edev_count = get_2m_edev_count();
            if (around_edev_count == 1) {   /* 周围只有一个设备，进入一对一对话 （TODO: 是否是当前对话的这一个？） */
                log_i("PLAYINTRO -> ONEONONE(WAIT_VAD)\n");
                g_mode_state = STATE_MODE_ONEONONE;
                g_oneonone_state = STATE_ONEONONE_WAIT_VAD;
                return true;
            } else {    /* 周围有多个设备，进入一对多对话 */
                log_i("PLAYINTRO -> ONEONMANY(IDLE)\n");
                g_mode_state = STATE_MODE_ONEONMANY;
                g_oneonmany_state = STATE_ONEONMANY_IDLE;
                return true;
            }
        }
        break;
    }
    return false;
}

//=================================================================================================
// One on one
//=================================================================================================
static bool app_oneonone_loop()
{
    struct websocket_struct *websockets_info = g_app_ctx->websockets_info;

    switch (g_oneonone_state) {
    case STATE_ONEONONE_WAIT_VAD:
        if (is_edev_far_away(g_curr_edev_idx)) { /* 当前设备距离 > 5 */
            g_mode_state = STATE_MODE_IDLE;
            log_i("ONEONONE(WAIT_VAD) -> IDLE\n");
            return true;
        }
        if (g_recording == true) {
            my_blocking_queue_clear(&g_asr_data_queue); /* 清空asr data queue */

            wss_app_recorder_data_upstream(recorder_data_to_data_queue); /* 录音数据 -> asr_data_queue */
            g_recording_upload_package_index = 0;
            g_recording = true;

            log_i("[ONEONONE] WAIT_VAD -> RECORDING\n");
            g_oneonone_state = STATE_ONEONONE_RECORDING;
            break;
        }
        break;
    case STATE_ONEONONE_RECORDING:
        if (is_edev_far_away(g_curr_edev_idx)) { /* 当前设备距离 > 5 */
            wss_app_recorder_data_upstream(recorder_data_to_ringbuffer); /* 录音数据 -> asr_ringbuffer */
            if (g_recording_upload_package_index > 0) { /* 已经上传了部分asr data时，发送message stop事件结束 */
                if (communication_message_stop(websockets_info) != 0) { /* 发送message stop */
                    wss_application_exception_exit();
                    return true;
                }
            }
            g_mode_state = STATE_MODE_IDLE;
            log_i("ONEONONE(RECORDING) -> IDLE\n");
            return true;
        }

        if (g_recording_upload_package_index == 0) { /* 发送第一包数据前，先发送asr事件和asr start事件 */
            if (communication_message_asr(websockets_info) != 0) { /* 发送asr */
                wss_application_exception_exit();
                return true;
            }
            if (communication_message_asr_start(websockets_info) != 0) { /* 发送asr start */
                wss_application_exception_exit();
                return true;
            }
        }
        /* 从ASR缓冲取数据发送给服务器，先将ringbuffer中的数据上传，再处理上传data queue中的数据 */
        log_i("AAA");
        if (MYRBERROR_Success == my_ring_buffer_read(&g_asr_ringbuffer, &asr_data_frame)) {
            log_i("BBB");
            wss_asr_data_to_server(websockets_info, &asr_data_frame);
        } else if (MYERROR_Success == my_blocking_queue_try_dequeue(&g_asr_data_queue, &asr_data_frame)) {
            log_i("CCC");
            wss_asr_data_to_server(websockets_info, &asr_data_frame);
        }

        if (g_recording == false) { /* VAD结束 */
            wss_app_recorder_data_upstream(recorder_data_to_ringbuffer); /* 录音数据 -> asr_ringbuffer */
            
            if (communication_message_asr_end(websockets_info) != 0) { /* 发送asr end */
                wss_application_exception_exit();
                return true;
            }

            /* 清空按键重播的音频链接列表 */
            g_communication_audio_cnt = 0;
            
            /* 等待音频链接回复 */
            g_wait_audio_resp_timer = g_curr_time;
            g_wait_resp_flag = false;

            /* 等待音频播放完毕 */
            g_audio_play_end = false;
            g_audio_play_ongoing_num = 0;
            g_backend_message_end_ok = false;

            log_i("[ONEONONE] RECORDING -> PLAYING\n");
            g_oneonone_state = STATE_ONEONONE_PLAYING;
        }
        break;
    case STATE_ONEONONE_PLAYING:
        if (is_edev_far_away(g_curr_edev_idx)) { /* 当前设备距离 > 5 */
            if (communication_message_stop(websockets_info) != 0) { /* 发送message stop */
                wss_application_exception_exit();
                return true;
            }
            disable_specific_audio_message(g_communication_message_id); /* 禁用播放队列中相同message_id的网络音频播放任务(LingOS) */
            app_music_play_stop(g_app_ctx->music_player);  /* 停止当前正在进行的解码 */
            g_mode_state = STATE_MODE_IDLE;
            log_i("ONEONONE(PLAYING) -> IDLE\n");
            return true;
        }

        if ((g_wait_resp_flag == false) && (g_curr_time - g_wait_audio_resp_timer >= WAIT_AUDIO_RESP_TIMEOUT)) { /* 等待回复超时，直接结束 */
            log_i("[ONEONONE] wait audio response timeout.\n");
            g_recording = false;
            log_i("[ONEONONE] PLAYING -> WAIT_VAD\n");
            g_oneonone_state = STATE_ONEONONE_WAIT_VAD;
        }
        
        if (g_backend_message_end_ok && g_audio_play_end && g_audio_play_ongoing_num == 0) { /* 音频播放完毕 */
            g_recording = false;
            log_i("[ONEONONE] PLAYING -> WAIT_VAD\n");
            g_oneonone_state = STATE_ONEONONE_WAIT_VAD;
        }
        break;
    }
    return false;
}

//=================================================================================================
// One on many
//=================================================================================================
static bool app_oneonmany_loop()
{
    struct websocket_struct *websockets_info = g_app_ctx->websockets_info;
    switch (g_oneonmany_state) {
    case STATE_ONEONMANY_IDLE:
        if (0 == get_5m_edev_count()) { /* 5米内无设备 */
            g_mode_state = STATE_MODE_IDLE;
            log_i("ONEONMANY(IDLE) -> IDLE\n");
        }
        break;
    }
}

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
    my_blocking_queue_init_static(&g_msg_queue, g_msg_queue_buffer, sizeof(AppMessage), MY_MESSAGE_QUEUE_CAPACITY);
    my_blocking_queue_init_static(&g_asr_data_queue, g_asr_data_queue_buffer, sizeof(AsrData), MY_ASR_DATA_QUEUE_CAPACITY);
    my_ring_buffer_init_static(&g_asr_ringbuffer, g_asr_ringbuffer_buffer, sizeof(AsrData), MY_ASR_RING_BUFFER_CAPACITY);

    g_app_ctx = lwl_app_core_context_get_instance();
    add_edev(&g_app_ctx->eend_dev_list[0], 45);
    add_edev(&g_app_ctx->eend_dev_list[1], 5);

    g_hdl = dev_open("uart2", NULL);
    if (!g_hdl) {
        printf("open uart err !!!\n");
        return;
    }
    /* 4 . 使能特殊串口,启动收发数据 */
    dev_ioctl(g_hdl, UART_START, 0);

    for (int i; i < get_edev_count(); ++i) {
        g_edev_table[i].session_create_ok = false;
    }

    /* 注册lwl事件处理回调 */
    lwl_event_handler_add(wss_app_lwl_event_handler_cb);
    /* 注册loswssp事件回调 */
    loswssp_register_rsp_cbk(loswssp_rsp_cbk);
    /* 设置VAD回调 */
    recorder_set_vad_cbk(g_app_ctx->recorder, recorder_vad_cb, NULL);
    /* 默认不设置录音上传回调 */
    wss_app_recorder_data_upstream(recorder_data_to_ringbuffer);
    /* 开启录音 */
    wss_app_recorder_en(ENABLE);

    /* 重置 */
    g_exception_exit = false;
    
    ASSERT(g_mode_state == STATE_MODE_IDLE);
    log_i("Start create session task.\n");

    /* 初始化 Create session 任务上下文 */
    g_session_create_state = STATE_CREATE_ROLE_SESSION_STARTUP;
    g_session_create_idx = 0;

    log_i("IDLE -> CREATE_SESSION\n");
    g_mode_state = STATE_MODE_CREATE_SESSION;
}

void wss_application_uninit()
{
    log_i("Uninit WSS application\n");
    my_blocking_queue_destroy_static(&g_msg_queue);
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

static void wss_application_test_loop(struct websocket_struct *websockets_info)
{
    if (MYERROR_Success == my_blocking_queue_try_dequeue(&g_asr_data_queue, &asr_data_frame)) { /* 从ASR缓冲取数据，不允许发生阻塞 */
        wss_asr_data_to_console(&asr_data_frame);
    }
}

static void wss_application_normal_loop(struct websocket_struct *websockets_info)
{
    AppMessage msg;
    if (MYERROR_Success == my_blocking_queue_try_dequeue(&g_msg_queue, &msg)) {
        log_i("get msg type: %d", msg.type);
        switch (msg.type) {
        case APPMSG_SHORT_CLICK:
            wss_app_msg_short_click_handler();
            break;
        }
        return;
    }

    switch (g_mode_state) {
    case STATE_MODE_IDLE:
        if (app_idle_loop()) {
        }
        break;
    case STATE_MODE_CREATE_SESSION:
        if (app_create_session_loop()) {
        }
        break;
    case STATE_MODE_PLAYINTRO:
        if (app_playintro_loop()) {
        }
        break;
    case STATE_MODE_ONEONONE:
        if (app_oneonone_loop()) {
        }
        break;
    case STATE_MODE_ONEONMANY:
        if (app_oneonmany_loop()) {
        }
        break;
    }
}

bool wss_application_loop(struct websocket_struct *websockets_info)
{
    g_curr_time = timer_get_ms();
    ASSERT(websockets_info != NULL);

    /* 超时定时器状态更新 */
    timeout_timer_update();

    if (g_test_mode) {
        wss_application_test_loop(websockets_info);
    } else {
        wss_application_normal_loop(websockets_info);
    }

    if (g_curr_time - g_heat_timer >= HEAT_TIMEOUT) {
        if (wss_ping_send(websockets_info) != 0) {
            wss_application_exception_exit();
            return true;
        }

        int pp = 2;
        lwl_app_context_t *context = lwl_app_core_context_get_instance();
        if (context && context->recorder) {
            pp = recorder_get_play_pause_state(context->recorder);
        }
        log_i("%d %d %d %d %d %d %d", g_test_mode, g_mode_state, 
            g_session_create_state, g_playintro_state, g_oneonone_state, g_oneonmany_state, pp);

        g_heat_timer = g_curr_time;
    }

    if (wss_application_check_exit()) {
        return true;
    }
    return false;
}
