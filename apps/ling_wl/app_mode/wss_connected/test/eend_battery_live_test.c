#include "eend_battery_live_test.h"
#include "../wss_app_util.h"
#include "cpu.h"

#include "util/cqueue.h"
#include "protocol/lingos/loswssp.h"
#include "bt_ble/client/ble_distance.h"
#include "async_msg_loop.h"
#include "app_mode/lwl_app_core.h"
#include "gpio.h"
#include "device.h"

static void wss_asr_data(void *data, u32 size);
static void app_record_task_start();

#define ENABLE 1
#define DISABLE 0

#define DEV_A_IDX 0
#define DEV_B_IDX 1

typedef enum {
    STATE_TASK_IDLE,
    STATE_TASK_TEST_RUNNING,
    STATE_TASK_TEST_PAUSE,
} AppTaskState;

typedef enum {
    STATE_ROUND_STARTUP,
    STATE_ROUND_SWITCH_A,
    STATE_ROUND_AUDIO_PLAY_A,
    STATE_ROUND_SWITCH_B,
    STATE_ROUND_AUDIO_PLAY_B,
    STATE_ROUND_OVER,
} AppTaskSubState;

#define ADV_MAX_LEN 16
#define SN_MAX_SIZE 16

typedef struct {
    lwl_eend_dev_t *pdev;
    int ip_id;
    int session_id;
} communication_edev_t;

static u32 g_curr_time = 0;
#define HEAT_TIMEOUT 5000
static u32 g_heat_timer = 0;

static bool g_test_mode = false;

/* Task state */
static int g_task_state = STATE_TASK_IDLE;
static int g_task_sub_state = STATE_ROUND_STARTUP;
static int g_task_sub_round_cnt = 0;

static int g_session_create_idx = 0;

#define TIMER_TASK_TIMEOUT (50 * 60 * 1000)
static u32 g_task_timer = 0;
#define TIMER_ROUND_TIMEOUT (30 * 1000)
static u32 g_task_round_timer = 0;

static int g_curr_edev_idx = DEV_A_IDX;
static int g_next_edev_idx = DEV_B_IDX;
static int g_backend_switch_session_ok = false;
static int g_ble_porte_switch_ok = false;
static int g_audio_play_end = false;
static int g_audio_play_ongoing_num = 0;
static int g_backend_message_end_ok = false;

/* 设备列表 */
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
    switch (code) {
    case SWITCH_ONLY_CODE_PREV_STOP_TIMEOUT:
        log_i("Switch e-end timeout: %d\n", code);
        break;
    case SWITCH_ONLY_CODE_NEXT_START_TIMEOUT:
        log_i("Switch e-end timeout: %d\n", code);
        break;
    case SWITCH_ONLY_CODE_SUCCESS:
        log_i("Switch e-end OK\n");
        break;
    }
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
        break;
    case LR_MESSAGE_ASR_DATA:
        log_i("LR_MESSAGE_ASR_DATA\n");
        break;
    case LR_MESSAGE_ASR_END:
        log_i("LR_MESSAGE_ASR_END\n");
        break;
    case LR_MESSAGE_DELTA:
        log_i("LR_MESSAGE_DELTA\n");
        break;
    case LR_MESSAGE_AUDIO: {
        log_i("LR_MESSAGE_AUDIO\n");
    }   break;
    case LR_MESSAGE_FILE:
        log_i("LR_MESSAGE_FILE\n");
        break;
    case LR_MESSAGE_END:
        log_i("LR_MESSAGE_END\n");
        break;
    case LR_MESSAGE_STOP:
        log_i("LR_MESSAGE_STOP\n");
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
// LOOP
//=================================================================================================

void test_application_init()
{
    log_i("Init WSS application\n");

    lwl_app_context_t *context = lwl_app_core_context_get_instance();
    add_edev(&context->eend_dev_list[0], 45);
    add_edev(&context->eend_dev_list[1], 5);

    /* 注册loswssp事件回调 */
    loswssp_register_rsp_cbk(loswssp_rsp_cbk);
}

void test_application_uninit()
{
    log_i("Uninit WSS application\n");
}

static void test_application_normal_loop(struct websocket_struct *websockets_info)
{
    switch (g_task_state) {
    case STATE_TASK_IDLE:
        log_i("[TASK]IDLE -> TEST_RUNNING\n");
        g_task_state = STATE_TASK_TEST_RUNNING;
        g_task_sub_state = STATE_ROUND_STARTUP;
        break;
    case STATE_TASK_TEST_RUNNING:
        switch (g_task_sub_state) {
        case STATE_ROUND_STARTUP:
            /* 切换 E端 */
            g_ble_porte_switch_ok = false;
            my_task_switch_only(g_edev_table[g_next_edev_idx].pdev->sn, NULL, switch_end_callback);

            log_i("[ROUND] STARTUP -> SWITCH_A\n");
            g_task_sub_state = STATE_ROUND_SWITCH_A;
            break;
        case STATE_ROUND_SWITCH_A:
            if (g_ble_porte_switch_ok) {
                /* 切换完毕 */
                g_curr_edev_idx = g_next_edev_idx; 
                g_next_edev_idx = (g_curr_edev_idx == DEV_A_IDX) ? DEV_B_IDX : DEV_A_IDX;

                /* 30s计时开始 */
                g_task_round_timer = g_curr_time;
            
                log_i("[ROUND] SWITCH_A -> PLAY_A\n");
                g_task_sub_state = STATE_ROUND_AUDIO_PLAY_A;
            }
            break;
        case STATE_ROUND_AUDIO_PLAY_A:
            if (g_curr_time - g_task_round_timer >= TIMER_ROUND_TIMEOUT) {
                /* 30s计时结束 */
                
                /* 切换 E端 */
                g_ble_porte_switch_ok = false;
                my_task_switch_only(g_edev_table[g_next_edev_idx].pdev->sn, NULL, switch_end_callback);

                log_i("[ROUND] PLAY_A -> SWITCH_B\n");
                g_task_sub_state = STATE_ROUND_SWITCH_B;
            }
            break;
        case STATE_ROUND_SWITCH_B:
            if (g_ble_porte_switch_ok) {
                /* 切换完毕 */
                g_curr_edev_idx = g_next_edev_idx; 
                g_next_edev_idx = (g_curr_edev_idx == DEV_A_IDX) ? DEV_B_IDX : DEV_A_IDX;

                /* 30s计时开始 */
                g_task_round_timer = g_curr_time;
            
                log_i("[ROUND] SWITCH_B -> PLAY_B\n");
                g_task_sub_state = STATE_ROUND_AUDIO_PLAY_B;
            }
            break;
        case STATE_ROUND_AUDIO_PLAY_B:
            if (g_curr_time - g_task_round_timer >= TIMER_ROUND_TIMEOUT) {
                /* 30s计时结束 */
                
                log_i("[ROUND] PLAY_B -> OVER\n");
                g_task_sub_state = STATE_ROUND_OVER;
            }
            break;
        case STATE_ROUND_OVER:
            if (g_task_sub_round_cnt++ < 10) {
                /* 重复一轮 */
                log_i("[ROUND] OVER -> STARTUP\n");
                g_task_sub_state = STATE_ROUND_STARTUP;
            } else {
                /* 10轮结束  */
                /* 50分钟计时开始 */
                g_task_timer = g_curr_time;
                g_task_state = STATE_TASK_TEST_PAUSE;
                log_i("[TASK] RUNNING -> PAUSE\n");
            }
            break;
        }
        break;
    case STATE_TASK_TEST_PAUSE:
        if (g_curr_time - g_task_timer >= TIMER_TASK_TIMEOUT) {
            /* 50分钟计时结束 */
            log_i("[ROUND] PAUSE -> RUNNING\n");
            g_task_state = STATE_TASK_TEST_RUNNING;
            g_task_sub_state = STATE_ROUND_STARTUP;
        }
        break;
    }
}

void test_application_loop(struct websocket_struct *websockets_info)
{
    g_curr_time = timer_get_ms();
    ASSERT(websockets_info != NULL);

    /* 超时定时器状态更新 */
    timeout_timer_update();

    test_application_normal_loop(websockets_info);

    if (g_curr_time - g_heat_timer >= HEAT_TIMEOUT) {
        wss_ping_send(websockets_info);

        g_heat_timer = g_curr_time;
    }
}
