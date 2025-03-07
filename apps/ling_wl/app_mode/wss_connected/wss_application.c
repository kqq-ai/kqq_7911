#include "wss_application.h"
#include "wss_app_util.h"
#include "cpu.h"

#include "util/cqueue.h"
#include "protocol/lingos/loswssp.h"
#include "bt_ble/client/ble_distance.h"
#include "async_msg_loop.h"
#include "app_mode/lwl_app_core.h"
#include "gpio.h"
#include "device.h"
#include "util/threadpool_tasks.h"

static void wss_asr_data(void *data, u32 size);
static void app_record_task_start();
static void app_task_break();

#define ENABLE 1
#define DISABLE 0

typedef enum {
    STATE_NET_CONFIG,
    STATE_APPLICATION,
} AppModeState;

typedef enum {
    STATE_TASK_IDLE,
    STATE_TASK_CREATE_SESSION,
    STATE_TASK_SWITCH_SAY_HELLO,
    STATE_TASK_RECORD,
    STATE_TASK_BREAK,
} AppTaskState;

typedef enum {
    STATE_CREATE_ROLE_SESSION_STARTUP,
    STATE_CREATE_ROLE_SESSION_CREATING,
    STATE_CREATE_ROLE_SESSION_OVER,

    STATE_SWITCH_SAY_HELLO_STARTUP,
    STATE_SWITCH_SAY_HELLO_SWITCH,
    STATE_SWITCH_SAY_HELLO_PLAY_AUDIO,
    STATE_SWITCH_SAY_HELLO_OVER,

    STATE_BREAK_STARTUP,
    STATE_BREAK_MESSAGE_STOP,
    STATE_BREAK_OVER,

    STATE_RECORD_STARTUP,
    STATE_RECORD_RECORDING,
    STATE_RECORD_PLAY_AUDIO,
    STATE_RECORD_OVER,
} AppTaskStepState;

typedef enum {
    RESULT_SUCCESS,
    RESULT_TIMEOUT,
    RESULT_UNEXCEPTED_ERROR,
} AppTaskResult;


#define ADV_MAX_LEN 16
#define SN_MAX_SIZE 16
#define DEV_DISTANCE_HISTORY_CAP 10

typedef struct {
    int data[DEV_DISTANCE_HISTORY_CAP];
    int wp;
    int rp;
    int tmp_size;
} distance_type_history_t;

typedef struct {
    lwl_eend_dev_t *pdev;
    int ip_id;
    int session_id;
    int session_create_ok;
    int hello_flag; //表示已经打过招呼了
    distance_type_history_t distance_history;
} communication_edev_t;

int get_distance_by_rank(distance_type_history_t *history, int rank)
{
    if (rank >= history->tmp_size) {
        return -1;
    }
    int idx = history->wp - 1 - rank;
    if (idx < 0) {
        idx += DEV_DISTANCE_HISTORY_CAP;
    }
    return history->data[idx];
}

int distance_put(distance_type_history_t *history, int distance_type)
{
    if (history->tmp_size >= DEV_DISTANCE_HISTORY_CAP) {
        history->data[history->wp] = distance_type;
        history->rp = (history->rp == DEV_DISTANCE_HISTORY_CAP - 1) ? 0 : history->rp + 1;
        history->wp = (history->wp == DEV_DISTANCE_HISTORY_CAP - 1) ? 0 : history->wp + 1;
    } else {
        history->data[history->wp] = distance_type;
        history->wp = (history->wp == DEV_DISTANCE_HISTORY_CAP - 1) ? 0 : history->wp + 1;
        history->tmp_size++;
    }
    return 0;
}

static u32 g_curr_time = 0;
#define HEAT_TIMEOUT 5000
static u32 g_heat_timer = 0;

static bool g_test_mode = false;

typedef enum {
    REC_TRIG_KEY,
    REC_TRIG_VAD,
} RECORDER_REIGGER_MODE;
static int g_recorder_trigger_mode = REC_TRIG_VAD; //0:vad,1:key

static int g_exception_exit = false;

/* Task state */
static int g_task_state = STATE_TASK_IDLE;

/* Create session context */
static int g_session_create_state = STATE_CREATE_ROLE_SESSION_STARTUP;
static int g_session_create_result = RESULT_SUCCESS;
static int g_session_create_idx = 0;

/* Switch say hello context */
static int g_switch_say_hello_state = STATE_SWITCH_SAY_HELLO_STARTUP;
static int g_switch_say_hello_result = RESULT_SUCCESS;
static int g_curr_edev_idx = 0;
static int g_next_edev_idx = 1;
static int g_backend_switch_session_ok = false;
static int g_ble_porte_switch_ok = false;
static int g_audio_play_end = false;
static int g_audio_play_ongoing_num = 0;
static int g_backend_message_end_ok = false;

/* Break context */
static int g_break_state = STATE_RECORD_STARTUP;
static int g_break_start = 0;
static int g_message_stop_ok = false;
static int g_message_stop_message_id = 0;

/* Recorder context */
static int g_record_state = STATE_RECORD_STARTUP;
static int g_recording = false;
/* Recorder VAD */
static int g_vad_state = 0;
static int g_vad_state_curr = 0;

/* Base64  */
#define ASR_DATA_CHUNK_SIZE 1280
#define BASE64_BUF_MAX_SIZE 1709    // ((ASR_DATA_CHUNK_SIZE + 2) / 3) * 4 = 1708
static char base64_buf[BASE64_BUF_MAX_SIZE];

/* UTF-8 Content */
static const char g_default_content[] = {
    // // 今天森林里有没有下雨？
    // 0xe4, 0xbb, 0x8a, 0xe5, 0xa4, 0xa9, 0xe6, 0xa3, 0xae, 0xe6, 0x9e, 0x97, 0xe9, 0x87, 0x8c, 0xe6, 0x9c, 0x89, 0xe6, 0xb2, 0xa1, 0xe6, 0x9c, 0x89, 0xe4, 0xb8, 0x8b, 0xe9, 0x9b, 0xa8, 0xef, 0xbc, 0x9f,
    // 小熊喜欢吃小兔子的胡萝卜吗？
    0xe5, 0xb0, 0x8f, 0xe7, 0x86, 0x8a, 0xe5, 0x96, 0x9c, 0xe6, 0xac, 0xa2, 0xe5, 0x90, 0x83, 0xe5, 0xb0, 0x8f, 0xe5, 0x85, 0x94, 0xe5, 0xad, 0x90, 0xe7, 0x9a, 0x84, 0xe8, 0x83, 0xa1, 0xe8, 0x90, 0x9d, 0xe5, 0x8d, 0x9c, 0xe5, 0x90, 0x97, 0xef, 0xbc, 0x9f,
};
static const char g_hello_content[] = {
    // 你好呀！
    0xE4, 0xBD, 0xA0, 0xE5, 0xA5, 0xBD, 0xE5, 0x91, 0x80, 0xEF, 0xBC, 0x81
};

/* 设备列表 */
static int g_edev_closest_idx = 0;  // 当前距离更近的设备索引
static int g_edev_count = 0;
static communication_edev_t g_edev_table[EEND_MAX_CNT];
// static communication_edev_t g_edev_table[EEND_MAX_CNT] = {
//     [0] = {
//         .ip_id = 45,
//         .session_id = 0,
//         .hello_flag = 0,
//         .distance_history = {
//             .rp = 0,
//             .wp = 0,
//             .tmp_size = 0,
//         },
//     },
//     [1] = {
//         .ip_id = 5,
//         .session_id = 0,
//         .distance_history = {
//             .rp = 0,
//             .wp = 0,
//             .tmp_size = 0,
//         },
//     },
// };

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
    g_edev_table[i].distance_history.rp = 0;
    g_edev_table[i].distance_history.wp = 0;
    g_edev_table[i].distance_history.tmp_size = 0;
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

static void wss_application_exception_exit()
{
    g_exception_exit = true;
}

static bool wss_application_check_exit()
{
    return g_exception_exit;
}

/* Timeout context */
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
static void audio_play_end_callback(void *priv)
{
    log_i("Audio play over\n");
    g_audio_play_end = true;
    g_audio_play_ongoing_num--;
    log_i("Audio play ongoing num: %d\n", g_audio_play_ongoing_num);
}

static void switch_end_callback(int code, void *priv)
{
    log_i("Switch PortE OK\n");
    g_ble_porte_switch_ok = true;
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

            if (g_task_state == STATE_TASK_CREATE_SESSION) {
                if (g_session_create_state == STATE_CREATE_ROLE_SESSION_CREATING) {
                    log_i("get role[%d] session id: %d\n", g_session_create_idx, session_id);
                    g_edev_table[g_session_create_idx].session_id = session_id;
                }
            }
        } else {
            /* 请求后端切换session时，若session已存在，则new_session字段为False */
            if (g_task_state == STATE_TASK_SWITCH_SAY_HELLO) {
                if (g_switch_say_hello_state == STATE_SWITCH_SAY_HELLO_SWITCH) {
                    g_backend_switch_session_ok = true;
                }
            }
        }
        break;
    case LR_MESSAGE_ASR_DATA:
        log_i("LR_MESSAGE_ASR_DATA\n");
        break;
    case LR_MESSAGE_ASR_END:
        log_i("LR_MESSAGE_ASR_END\n");
        // if (g_communication_app_state == STATE_COMMUNICATION) {
        //     if (g_communication_state == STATE_COMMUNICATION_WAIT_RSP) {
        //         g_message_asr_end_ok = true;
        //     }
        // }
        break;
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

            if (g_task_state == STATE_TASK_SWITCH_SAY_HELLO) {
                if (g_switch_say_hello_state == STATE_SWITCH_SAY_HELLO_PLAY_AUDIO) {
                    /* 保存对话内容，用于之后和另一个角色对话 */
                    log_i("latest content :%s", vcontent);
                    log_i("latest url     :%s", vurl);

                    /* 执行任务播放音乐，并注册回调，播放完成时处理 */
                    my_task_play_one(vmessage_id, vurl, NULL, audio_play_end_callback);
                    g_audio_play_ongoing_num++;
                }
            } else if (g_task_state == STATE_TASK_RECORD) {
                if (g_record_state == STATE_RECORD_PLAY_AUDIO) {
                    /* 保存对话内容，用于之后和另一个角色对话 */
                    log_i("latest content :%s", vcontent);
                    log_i("latest url     :%s", vurl);

                    /* 执行任务播放音乐，并注册回调，播放完成时处理 */
                    my_task_play_one(vmessage_id, vurl, NULL, audio_play_end_callback);
                    g_audio_play_ongoing_num++;
                }
            } else if (g_task_state == STATE_TASK_CREATE_SESSION) {
                /* 不处理 */
                
                // /* 保存开场白 */
                // lwl_app_context_t *ctx = lwl_app_core_context_get_instance();
                // save_role_opening_task_param_t *save_opening_param = (save_role_opening_task_param_t *)malloc(sizeof(save_role_opening_task_param_t));
                // save_opening_param->lfs = ctx->lfs_hdl;
                // save_opening_param->url = (char *)malloc(strlen(vurl) + 1);
                // if (save_opening_param->url == NULL) {
                //     printf("Failed to allocate memory for vurl\n");
                //     free(save_opening_param);
                //     break;
                // }
                // strcpy(save_opening_param->url, vurl);
                // save_opening_param->role_name = g_edev_table[g_session_create_idx].ip_id;
                // sprintf(save_opening_param->opening_filename, "opening_%d.mp3", g_edev_table[g_session_create_idx].ip_id);
                // if (!threadpool_add_task(ctx->threadpool, save_role_opening_file_task, save_opening_param))
                // {
                //     printf("!!!!!!!!!!!!!!!!!Failed to add save_role_opening_file_task  !!!!!!!!!!!!!!!!!!!!!\r\n");
                //     free(save_opening_param->url);
                //     free(save_opening_param); 
                //     break;                  
                // }
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

        if (g_task_state == STATE_TASK_SWITCH_SAY_HELLO) {
            if (g_switch_say_hello_state == STATE_SWITCH_SAY_HELLO_PLAY_AUDIO) {
                g_backend_message_end_ok = true;
            }
        } else if (g_task_state == STATE_TASK_RECORD) {
            if (g_record_state == STATE_RECORD_PLAY_AUDIO) {
                g_backend_message_end_ok = true;
            }
        } else if (g_task_state == STATE_TASK_CREATE_SESSION) {
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
// Recorder
//=================================================================================================

static void recorder_data_cb(void* priv, void *data, u32 size) {
    wss_asr_data(data, size);
}

static void recorder_vad_cb(void* priv, int type)
{
    lwl_app_context_t *context = lwl_app_core_context_get_instance();
    
    uint8_t data_1[] = {0xA1, 0x01};
    uint8_t data_2[] = {0xA1, 0x00};

    if (g_task_state == STATE_TASK_IDLE || g_task_state == STATE_TASK_RECORD) {
        if (type == 1) {
            g_vad_state_curr = 1;
            dev_write(context->uartdev, data_1, 2);
        } else {
            g_vad_state_curr = 0;
            dev_write(context->uartdev, data_2, 2);
        }
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
        if (g_task_state == STATE_TASK_IDLE && g_recorder_trigger_mode == REC_TRIG_VAD) {
            app_record_task_start();
        }
    }
    if (g_vad_state == 1 && g_vad_state_curr == 0) {
        if (g_task_state == STATE_TASK_RECORD && g_recorder_trigger_mode == REC_TRIG_VAD) {
            g_recording = false;
        }
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

static void wss_asr_data_to_server(struct websocket_struct *websockets_info, AsrData *asr_data)
{
    /* 调试输出数据量 */
    static int cnt1 = 0;
    cnt1 += ASR_DATA_CHUNK_SIZE;
    log_d("cnt1: %d\n", cnt1);

    size_t write_len = 0;
    memset(base64_buf, 0, sizeof(base64_buf));
    int ret = mbedtls_base64_encode(base64_buf, BASE64_BUF_MAX_SIZE, &write_len, asr_data->data, ASR_DATA_CHUNK_SIZE);
    ASSERT(ret == 0);
    ASSERT(write_len == (BASE64_BUF_MAX_SIZE - 1));

    if (g_recording == true) {
        log_i(">>");
        if (communication_message_asr_data(websockets_info, base64_buf) != 0){
            wss_application_exception_exit();
            return;
        }
    }
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
// Message queue
//=================================================================================================
enum {
    WSS_TASK_MESSAGE_DEV_DISTANCE,

    WSS_TASK_MESSAGE_MAX = 0xFF,
};
typedef struct {
    int devIndex;
} WssMessageDevDistance;

typedef struct {
    int msg;
    union {
        WssMessageDevDistance dev_distance;
    };
} WssTaskMessage;

#define MY_TASK_MESSAGE_QUEUE_CAPACITY 10
static MyBlockingQueue g_message_queue; // MyBlockingQueue<WssTaskMessage>
static u8 g_message_queue_buffer[MY_TASK_MESSAGE_QUEUE_CAPACITY * sizeof(WssTaskMessage)];

//=================================================================================================
// Message notify functions
//=================================================================================================
void wss_dev_distance(int devIndex)
{
    WssTaskMessage message_temp;
    message_temp.msg = WSS_TASK_MESSAGE_DEV_DISTANCE;
    message_temp.dev_distance.devIndex = devIndex;

    if (my_blocking_queue_is_full(&g_message_queue)) {
        log_e("!!!queue is full please expend the capacity.");
    } else {
        my_blocking_queue_enqueue(&g_message_queue, &message_temp);
    }
}

//=================================================================================================
// Message handler
//=================================================================================================



static void app_create_session_task_start(void);
static void app_switch_say_hello_task_start(int devIndex);
static void wss_task_message_dev_distance_handler(WssTaskMessage *message)
{
    ASSERT(message->msg == WSS_TASK_MESSAGE_DEV_DISTANCE);
    WssMessageDevDistance *param = &message->dev_distance;

    int idx = param->devIndex;

    app_switch_say_hello_task_start(idx);
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

#define DISTANCE_KEEP_TIME 5
void self_function(disatance_info_t *distance_info, int distance_type)
{
    log_i("distance_type: %d %d\n", distance_type, distance_info->average_rssi);

    int idx = find_edev_idx(distance_info->p_eend_dev);
    if (idx == -1) {
        return;
    }

    /* 保存当前距离 */
    distance_put(&g_edev_table[idx].distance_history, distance_type);
    
    /* 如果在2米范围外保持了 DISTANCE_KEEP_TIME 次，并且当前在2米范围内，则判定为靠近 */
    if (g_edev_table[idx].distance_history.tmp_size >= (DISTANCE_KEEP_TIME + 1)) {
        bool is_approach = true;
        for (int i = 0; i < (DISTANCE_KEEP_TIME + 1); ++i) {
            int distance_type = get_distance_by_rank(&g_edev_table[idx].distance_history, i);

            log_d("distance_history[%d] = %d", i, distance_type);

            /* 当前不在2米范围内 */
            if (i == 0 && distance_type != DISTANCE_0) {
                is_approach = false;
                break;
            }
            /* 最近 DISTANCE_KEEP_TIME 次中，任意一次在2米范围内 */
            if (i != 0 && (distance_type == DISTANCE_0)) {
                is_approach = false;
                break;
            }
        }
        if (is_approach){//} && g_edev_table[idx].hello_flag == 0) {
            g_edev_table[idx].hello_flag = 1;
            wss_dev_distance(idx);
        }
    }
}

//=================================================================================================
// APP task
//=================================================================================================
static void app_create_session_task_start(void)
{
    ASSERT(g_task_state == STATE_TASK_IDLE);
    log_i("Start create session task.\n");

    /* 初始化 Create session 任务上下文 */
    g_session_create_state = STATE_CREATE_ROLE_SESSION_STARTUP;
    g_session_create_result = RESULT_SUCCESS;
    g_session_create_idx = 0;

    /* 创建超时定时器 */
    timeout_timer_init(20000);  // 20s

    log_i("IDLE -> CREATE_SESSION\n");
    g_task_state = STATE_TASK_CREATE_SESSION;
}
static bool app_create_session_task_loop(struct websocket_struct *websockets_info)
{
    switch (g_session_create_state) {
    case STATE_CREATE_ROLE_SESSION_STARTUP:
        /* wss 发起创建会话 */
        for (;g_session_create_idx < get_edev_count(); g_session_create_idx++) {
            if (g_edev_table[g_session_create_idx].session_create_ok == false && 
                g_edev_table[g_session_create_idx].ip_id != -1) {
                if (communication_init_role(websockets_info, g_edev_table[g_session_create_idx].ip_id) != 0){
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
        return true;
    }
    return false;
}

static void app_switch_say_hello_task_start(int devIndex)
{
    ASSERT(g_task_state == STATE_TASK_IDLE);
    log_i("Start switch say hello task.\n");

    /* 初始化 Switch say hello 任务上下文 */
    g_switch_say_hello_state = STATE_SWITCH_SAY_HELLO_STARTUP;
    g_switch_say_hello_result = RESULT_SUCCESS;
    g_next_edev_idx = devIndex;

    /* 创建超时定时器 */
    timeout_timer_init(20000);  // 20s
    
    log_i("IDLE -> SWITCH_SAY_HELLO\n");
    g_task_state = STATE_TASK_SWITCH_SAY_HELLO;
}
static bool app_switch_say_hello_task_loop(struct websocket_struct *websockets_info)
{
    switch (g_switch_say_hello_state) {
    case STATE_SWITCH_SAY_HELLO_STARTUP:
        /* wss 发起切换会话 */
        g_backend_switch_session_ok = false;
        if (wss_2ip_switch(websockets_info, g_edev_table[g_next_edev_idx].session_id, 
            g_edev_table[g_next_edev_idx].ip_id) != 0){
            wss_application_exception_exit();
            return true;
        }

        /* 切换 E端 */
        g_ble_porte_switch_ok = false;
        my_task_switch_only(g_edev_table[g_next_edev_idx].pdev->sn, NULL, switch_end_callback);

        log_i("[Switch and say hello] STARTUP -> SWITCH\n");
        g_switch_say_hello_state = STATE_SWITCH_SAY_HELLO_SWITCH;
        break;
    case STATE_SWITCH_SAY_HELLO_SWITCH:
        if (g_backend_switch_session_ok && g_ble_porte_switch_ok) {
            /* 切换完毕 */
            g_curr_edev_idx = g_next_edev_idx; 
        
            /* 发送 你好呀！请求音频回复 */
            g_audio_play_end = false;
            g_backend_message_end_ok = false;
            g_audio_play_ongoing_num = 0;
            if (communication_talk_message(websockets_info, g_hello_content) != 0){
                wss_application_exception_exit();
                return true;
            }

            log_i("[Switch and say hello] SWITCH -> PLAY_AUDIO\n");
            g_switch_say_hello_state = STATE_SWITCH_SAY_HELLO_PLAY_AUDIO;
        }
        break;
    case STATE_SWITCH_SAY_HELLO_PLAY_AUDIO:
        if (g_backend_message_end_ok && g_audio_play_ongoing_num == 0) {
            /* 音频播放完毕 */
            log_i("[Switch and say hello] PLAY_AUDIO -> OVER\n");
            g_switch_say_hello_state = STATE_SWITCH_SAY_HELLO_OVER;
        }
        break;
    case STATE_SWITCH_SAY_HELLO_OVER:
        return true;
    }
    return false;
}


static void app_record_task_start()
{
    ASSERT(((g_task_state == STATE_TASK_IDLE) | (g_task_state == STATE_TASK_RECORD)));
    log_i("Start record.\n");

    /* 初始化 Switch say hello 任务上下文 */
    g_record_state = STATE_RECORD_STARTUP;
    
    log_i("IDLE -> RECORD\n");
    g_task_state = STATE_TASK_RECORD;
}

static AsrData asr_data_frame;
static bool app_record_task_loop(struct websocket_struct *websockets_info)
{
    WssTaskMessage message;

    switch (g_record_state) {
    case STATE_RECORD_STARTUP:
        /* 发送asr start */
        if (communication_message_asr(websockets_info) != 0){
            wss_application_exception_exit();
            return true;
        }
        if (communication_message_asr_start(websockets_info) != 0){
            wss_application_exception_exit();
            return true;
        }
        /* 清空asr data queue */
        my_blocking_queue_clear(&g_asr_data_queue);

        /* 设置录音上传回调 */
        wss_app_recorder_data_upstream_en(ENABLE);
        g_recording = true;

        log_i("[Record] STARTUP -> RECORDING\n");
        g_record_state = STATE_RECORD_RECORDING;
        break;
    case STATE_RECORD_RECORDING:
        if (!my_blocking_queue_is_empty(&g_asr_data_queue)) {
            /* 从ASR缓冲取数据，不允许发生阻塞 */
            my_blocking_queue_dequeue(&g_asr_data_queue, &asr_data_frame);
            /* 发送给服务器 */
            wss_asr_data_to_server(websockets_info, &asr_data_frame);
        }

        if (g_recording == false) {
            /* 取消录音上传回调 */
            wss_app_recorder_data_upstream_en(DISABLE);
            /* 停止录音 */
            wss_app_recorder_en(DISABLE);
            /* 发送asr end */
            if (communication_message_asr_end(websockets_info) != 0){
                wss_application_exception_exit();
                return true;
            }

            /* 等待音频回复 */
            g_audio_play_end = false;
            g_backend_message_end_ok = false;
            g_audio_play_ongoing_num = 0;

            log_i("[Record] RECORDING -> PLAY_AUDIO\n");
            g_record_state = STATE_RECORD_PLAY_AUDIO;
        }
        break;
    case STATE_RECORD_PLAY_AUDIO:
        if (g_backend_message_end_ok && g_audio_play_ongoing_num == 0) {
            /* 音频播放完毕 */
            lwl_app_context_t *context = lwl_app_core_context_get_instance();
            /* 开启录音 */
            wss_app_recorder_en(ENABLE);

            log_i("[Record] PLAY_AUDIO -> OVER\n");
            g_record_state = STATE_RECORD_OVER;
        }
        break;
    case STATE_RECORD_OVER:
        return true;
    }
    return false;
}


static void app_task_break()
{
    g_break_start = true;
}
static bool app_is_break()
{
    int tmp = g_break_start;
    g_break_start = false;
    return tmp;
}
static bool app_break_loop(struct websocket_struct *websockets_info)
{
    switch (g_break_state) {
    case STATE_BREAK_STARTUP:
        /* 发送asr start */
        g_message_stop_ok = false;
        if (communication_message_stop(websockets_info) != 0){
            wss_application_exception_exit();
            return true;
        }

        /* 设置VAD回调 */
        wss_app_set_vad_callback(ENABLE);
        /* 默认不设置录音上传回调 */
        wss_app_recorder_data_upstream_en(DISABLE);
        /* 开启录音 */
        wss_app_recorder_en(ENABLE);

        /* 取消所有播放任务 */
        disable_all_audio_message();

        /* 停止正在进行的解码 */
        lwl_app_context_t *ctx = lwl_app_core_context_get_instance();
        app_music_play_stop(ctx->music_player);

        log_i("[Break] STARTUP -> MESSAGE_STOP\n");
        g_break_state = STATE_BREAK_MESSAGE_STOP;
        break;
    case STATE_BREAK_MESSAGE_STOP:
        if (g_message_stop_ok) {
            log_i("[Break] MESSAGE_STOP -> OVER\n");
            g_break_state = STATE_BREAK_OVER;
        }
        break;
    case STATE_BREAK_OVER:
        return true;
    }
    return false;
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
        /* 设置VAD回调 */
        wss_app_set_vad_callback(DISABLE);
        /* 默认不设置录音上传回调 */
        wss_app_recorder_data_upstream_en(ENABLE);
        /* 开启录音 */
        wss_app_recorder_en(ENABLE);
        /* 切换测试模式 */
        g_test_mode = true;
    } else {
        log_i("switch to normal mode")
        /* 设置VAD回调 */
        wss_app_set_vad_callback(ENABLE);
        /* 默认不设置录音上传回调 */
        wss_app_recorder_data_upstream_en(DISABLE);
        /* 开启录音 */
        wss_app_recorder_en(ENABLE);
        /* 切换正常模式 */
        g_test_mode = false;
    }
}

void switch_cb(int code, void *priv)
{
    log_i("[WSS] switch code: %d", code);
}
static void wss_app_lwl_event_handler_cb(lwl_event_t *e)
{
    static int en = 1;
    switch (e->event_type) {
    case LWL_EVENT_TYPE_UART_IO_KEY_SHORT_CLICK:
        log_i("SHORT_CLICK");

        if (g_task_state == STATE_TASK_SWITCH_SAY_HELLO ||
            g_task_state == STATE_TASK_RECORD) {
            /* 打断 */
            app_task_break();
        }
        
        // wss_application_switch_test_mode(); 

        // log_i("en = %d\n", en);

        // if (en) {
        //     my_task_switch_only("WCCN24H28FEEEF8", NULL, switch_cb);
        //     en = 0;
        // } else {
        //     my_task_switch_only("000000000000000", NULL, switch_cb);
        //     en = 1;
        // }

        /* 打断 */

        break;
    case LWL_EVENT_TYPE_UART_SYNC:
        log_i("SYNC");
        uart_response_synch_status(g_task_state != STATE_TASK_IDLE);
        break;
    case LWL_EVENT_TYPE_UART_TOUCH_KEY_DOWN:
        log_i("TOUCH KEY DOWN");
        break;
    case LWL_EVENT_TYPE_UART_SHUTDOWN:
        log_i("SHUTDOWN");
        break;
    }
}

void wss_application_init()
{
    log_i("Init WSS application\n");
    my_blocking_queue_init_static(&g_message_queue, g_message_queue_buffer, sizeof(WssTaskMessage), MY_TASK_MESSAGE_QUEUE_CAPACITY);
    my_blocking_queue_init_static(&g_asr_data_queue, g_asr_data_queue_buffer, sizeof(AsrData), MY_ASR_DATA_QUEUE_CAPACITY);

    lwl_app_context_t *context = lwl_app_core_context_get_instance();
    add_edev(&context->eend_dev_list[0], 45);
    add_edev(&context->eend_dev_list[1], 5);

    for (int i; i < get_edev_count(); ++i) {
        g_edev_table[i].session_create_ok = false;
    }

    /* 注册loswssp事件回调 */
    loswssp_register_rsp_cbk(loswssp_rsp_cbk);

    /* 设置VAD回调 */
    wss_app_set_vad_callback(ENABLE);
    /* 默认不设置录音上传回调 */
    wss_app_recorder_data_upstream_en(DISABLE);
    /* 开启录音 */
    wss_app_recorder_en(ENABLE);

    /* 注册lwl事件处理回调 */
    lwl_event_handler_add(wss_app_lwl_event_handler_cb);

    /* 默认执行创建会话任务 */
    app_create_session_task_start();

    /* 重置 */
    g_exception_exit = false;
}

void wss_application_uninit()
{
    log_i("Uninit WSS application\n");
    my_blocking_queue_destroy_static(&g_message_queue);
    my_blocking_queue_destroy_static(&g_asr_data_queue);
    
    /* 设置VAD回调 */
    wss_app_set_vad_callback(DISABLE);
    /* 取消录音上传回调 */
    wss_app_recorder_data_upstream_en(DISABLE);
    /* 开启录音 */
    wss_app_recorder_en(DISABLE);
    
    /* 取消注册lwl事件处理回调 */
    lwl_event_handler_remove(wss_app_lwl_event_handler_cb);
}

static void wss_application_test_loop(struct websocket_struct *websockets_info)
{
    WssTaskMessage message; 
    if (!my_blocking_queue_is_empty(&g_message_queue)) {
        /* 从消息队列取消息，不允许发生阻塞 */
        my_blocking_queue_dequeue(&g_message_queue, &message);
        log_d("wss task get message: %d\n", message.msg);
        /* 测试模式只消费消息，不做任何逻辑处理 */
    }
    
    if (!my_blocking_queue_is_empty(&g_asr_data_queue)) {
        /* 从ASR缓冲取数据，不允许发生阻塞 */
        my_blocking_queue_dequeue(&g_asr_data_queue, &asr_data_frame);
        /* 日志输出 */
        wss_asr_data_to_console(&asr_data_frame);
    }
}

static void wss_application_normal_loop(struct websocket_struct *websockets_info)
{
    WssTaskMessage message;
    if (g_task_state == STATE_TASK_IDLE && !my_blocking_queue_is_empty(&g_message_queue)) {
        /* 从消息队列取消息，不允许发生阻塞 */
        my_blocking_queue_dequeue(&g_message_queue, &message);

        log_d("wss task get message: %d\n", message.msg);

        switch (message.msg) {
        case WSS_TASK_MESSAGE_DEV_DISTANCE:
            wss_task_message_dev_distance_handler(&message);
            break;
        }
    }

    switch (g_task_state) {
    case STATE_TASK_IDLE:
        /* 检测VAD speak start */
        wss_app_vad_update_loop();
        break;
    case STATE_TASK_CREATE_SESSION:
        if (timeout_timer_is_timeout()) {
            /* 执行超时切回IDLE状态 */
            log_i("TIMEOUT CREATE_SESSION -> IDLE\n");
            g_task_state = STATE_TASK_IDLE;
        }
        if (app_create_session_task_loop(websockets_info)) {
            /* 执行完毕切回IDLE状态 */
            log_i("COMPLETED CREATE_SESSION -> IDLE\n");
            g_task_state = STATE_TASK_IDLE;
        }
        break;
    case STATE_TASK_SWITCH_SAY_HELLO:
        if (timeout_timer_is_timeout()) {
            /* 执行超时切回IDLE状态 */
            log_i("TIMEOUT SWITCH_SAY_HELLO -> IDLE\n");
            g_task_state = STATE_TASK_IDLE;
        }
        if (app_is_break()) {
            /* 发生打断了 */
            g_break_state = STATE_BREAK_STARTUP;
            log_i("BREAK SWITCH_SAY_HELLO -> BREAK\n");
            g_task_state = STATE_TASK_BREAK;
        }
        if (app_switch_say_hello_task_loop(websockets_info)) {
            /* 执行完毕切回IDLE状态 */
            log_i("COMPLETED SWITCH_SAY_HELLO -> IDLE\n");
            g_task_state = STATE_TASK_IDLE;
        }
        break;
    case STATE_TASK_RECORD:
        /* 检测VAD speak stop */
        wss_app_vad_update_loop();
        if (app_is_break()) {
            /* 发生打断了 */
            g_break_state = STATE_BREAK_STARTUP;
            log_i("BREAK RECORD -> BREAK\n");
            g_task_state = STATE_TASK_BREAK;
        }
        if (app_record_task_loop(websockets_info)) {
            /* 执行完毕切回IDLE状态 */
            log_i("COMPLETED RECORD -> IDLE\n");
            g_task_state = STATE_TASK_IDLE;
        }
        break;
    case STATE_TASK_BREAK:
        log_i("COMPLETED BREAK -> IDLE\n");
        if (app_break_loop(websockets_info)) {
            g_task_state = STATE_TASK_IDLE;
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
        if (wss_ping_send(websockets_info) != 0){
            wss_application_exception_exit();
            return true;
        }

        int pp = 2;
        lwl_app_context_t *context = lwl_app_core_context_get_instance();
        if (context && context->recorder) {
            pp = recorder_get_play_pause_state(context->recorder);
        }
        log_i("%d %d %d %d %d %d %d", g_test_mode, g_recorder_trigger_mode, 
            g_task_state, g_session_create_state, g_switch_say_hello_state, g_record_state, pp);

        g_heat_timer = g_curr_time;
    }

    if (wss_application_check_exit()) {
        return true;
    }
    return false;
}
