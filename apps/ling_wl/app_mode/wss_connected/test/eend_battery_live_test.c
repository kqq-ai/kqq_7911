#include "eend_battery_live_test.h"
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

static void wss_app_recorder_data_upstream(recorder_data_cb_t cb);
static void recorder_data_to_data_queue(void* priv, void *data, u32 size);
static void recorder_data_to_ringbuffer(void* priv, void *data, u32 size);
static void wss_application_switch_test_mode();
static void wss_asr_data(void *data, u32 size);
static void wss_asr_data_to_ringbuffer(void *data, u32 size);
static void app_record_task_start();
static void app_task_break();

#define ENABLE 1
#define DISABLE 0

#define DUMMY_SN "000000000000000"
#define AUTO_TEST_SN "WCCN24H28FEEEFA"

typedef enum {
    STATE_MODE_IDLE,
    STATE_MODE_AUTO_TEST_RUNNING,
    STATE_MODE_AUTO_TEST_PAUSE,
} AppModeState;

typedef enum {
    STATE_AUTO_TEST_WAIT_HANDSHAKE,
    STATE_AUTO_TEST_TURNON,
    STATE_AUTO_TEST_PLAY,
    STATE_AUTO_TEST_TURNOFF,
    STATE_AUTO_TEST_WAIT,
} AppTaskStepState;

static u32 g_curr_time = 0;
#define HEAT_TIMEOUT 5000
static u32 g_heat_timer = 0;

static int g_exception_exit = false;

static lwl_app_context_t *g_app_ctx = NULL;

/* Task state */
static int g_mode_state = STATE_MODE_IDLE;

/* Auto test running state */
static int g_auto_test_running_state = STATE_AUTO_TEST_WAIT_HANDSHAKE;
static int g_auto_test_open_count = 0;

/*=================================================================================================
 * E switch context
 *=================================================================================================*/
static int g_ble_edev_switch_ok = false;


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
static void audio_play_end_callback(int state, void *priv)
{
    switch (state) {
    case CODE_PLAY_AUDIO_STATE_BEFORE_DEC_START:
        log_i("Audio play begin\n");
        break;
    case CODE_PLAY_AUDIO_STATE_AFTER_DEC_END:
        log_i("Audio play over\n");
        break;
    case CODE_PLAY_AUDIO_STATE_CANCEL:
        log_i("Audio play cancel\n");
        break;
    }
}

static void play_audio_task(void *priv);
static void music_play_cb(void *priv)
{
    threadpool_add_task(g_app_ctx->threadpool, play_audio_task, NULL);
}

static void play_audio_task(void *priv)
{
    msleep(10000);
    app_music_play_net_music_with_callback(g_app_ctx->music_player, "https://music.163.com/song/media/outer/url?id=1902252436.mp3", music_play_cb, NULL);
}

static void net_active_task(void *priv)
{ 
    while (1) {
        msleep(800);
        char *ota_info_json = NULL;
        extern int request_ota_info(char **pota_info);
        int ret = request_ota_info(&ota_info_json);
    }
}


static void switch_end_callback(int code, void *priv)
{
    switch (code) {
    case CODE_EDEV_CMD_SUCCESS:
    case CODE_EDEV_CMD_TIMEOUT:
    case CODE_EDEV_CMD_UNEXPECTED_ERROR:
        log_i("Switch PortE OK %d\n", code);
        g_ble_edev_switch_ok = true;
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
        //uart_response_synch_status(g_mode_state != STATE_MODE_IDLE, 8);
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

static bool wss_app_msg_short_click_handler() 
{
    struct websocket_struct *websockets_info = g_app_ctx->websockets_info;
    return false;
}

//=================================================================================================
// LOOP
//=================================================================================================
void auto_test_application_init()
{
    ble_simple_server_module_enable(0);
    ble_simple_client_module_enable(1);

    log_i("Init auto test application\n");
    my_blocking_queue_init_static(&g_msg_queue, g_msg_queue_buffer, sizeof(AppMessage), MY_MESSAGE_QUEUE_CAPACITY);

    g_app_ctx = lwl_app_core_context_get_instance();

    /* 注册lwl事件处理回调 */
    lwl_event_handler_add(wss_app_lwl_event_handler_cb);

    
    // gpio_direction_output(IO_PORTA_00, 0);   //

    /* 重置 */
    g_exception_exit = false;
    
    ASSERT(g_mode_state == STATE_MODE_IDLE);

    /* 后台一直播放 */
    threadpool_add_task(g_app_ctx->threadpool, play_audio_task, NULL);
    // threadpool_add_task(g_app_ctx->threadpool, net_active_task, NULL);

    

    /* 初始化 Auto test running 任务上下文 */
    timeout_timer_init(1 * 60 * 1000);
    g_auto_test_running_state = STATE_AUTO_TEST_WAIT_HANDSHAKE;

    log_i("AUTO_TEST_PAUSE -> AUTO_TEST_RUNNING(WAIT_HANDSHAKE)");
    g_mode_state = STATE_MODE_AUTO_TEST_RUNNING;
}

void auto_test_application_uninit()
{
    log_i("Uninit auto test application\n");
    my_blocking_queue_destroy_static(&g_msg_queue);
    
    /* 取消注册lwl事件处理回调 */
    lwl_event_handler_remove(wss_app_lwl_event_handler_cb);
}

static void wss_application_normal_loop()
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
    struct websocket_struct *websockets_info = g_app_ctx->websockets_info;

    switch (g_mode_state) {
    case STATE_MODE_IDLE:
        break;
    case STATE_MODE_AUTO_TEST_RUNNING:
        switch (g_auto_test_running_state) {
        case STATE_AUTO_TEST_WAIT_HANDSHAKE:
            if (timeout_timer_is_timeout()) {

                g_ble_edev_switch_ok = false;
                my_task_turn_on_edev(AUTO_TEST_SN, 0, switch_end_callback, NULL);

                log_i("[AUTO_TEST_RUNNING] HANDSHAKE -> TUENON");
                g_auto_test_running_state = STATE_AUTO_TEST_TURNON;
            }
            break;
        case STATE_AUTO_TEST_TURNON:
            if (g_ble_edev_switch_ok == true) {

                // g_ble_edev_switch_ok = false;
                // my_task_turn_on_edev(AUTO_TEST_SN, 0, switch_end_callback, NULL);

                // log_i("[AUTO_TEST_RUNNING] HANDSHAKE -> TUENON");
                // g_auto_test_running_state = STATE_AUTO_TEST_TURNON;

                timeout_timer_init(30000);

                log_i("[AUTO_TEST_RUNNING] TURNON -> PLAY");
                g_auto_test_running_state = STATE_AUTO_TEST_PLAY;
            }
            break;
        case STATE_AUTO_TEST_PLAY:
            if (timeout_timer_is_timeout()) {

                g_ble_edev_switch_ok = false;
                my_task_turn_off_edev(AUTO_TEST_SN, 0, switch_end_callback, NULL);

                log_i("[AUTO_TEST_RUNNING] PLAY -> TURNOFF");
                g_auto_test_running_state = STATE_AUTO_TEST_TURNOFF;
            }
            break;
        case STATE_AUTO_TEST_TURNOFF:
            if (g_ble_edev_switch_ok == true) {
                if (g_auto_test_open_count++ < 10) {
                    timeout_timer_init(30000);
                    
                    log_i("[AUTO_TEST_RUNNING] TURNOFF -> WAIT");
                    g_auto_test_running_state = STATE_AUTO_TEST_WAIT;
                } else {
                    timeout_timer_init(49 * 60 * 1000);
                    ble_simple_client_module_enable(0);

                    log_i("AUTO_TEST_RUNNING(TURNOFF) -> AUTO_TEST_PAUSE");
                    g_mode_state = STATE_MODE_AUTO_TEST_PAUSE;
                }
            }
            break;
        case STATE_AUTO_TEST_WAIT:
            if (timeout_timer_is_timeout()) {

                g_ble_edev_switch_ok = false;
                my_task_turn_on_edev(AUTO_TEST_SN, 0, switch_end_callback, NULL);
                
                log_i("[AUTO_TEST_RUNNING] WAIT -> TUENON");
                g_auto_test_running_state = STATE_AUTO_TEST_TURNON;
            }
            break;
        }
        break;
    case STATE_MODE_AUTO_TEST_PAUSE:
        if (timeout_timer_is_timeout()) {
            ble_simple_client_module_enable(1);
            g_auto_test_open_count = 0;

            timeout_timer_init(1 * 60 * 1000);
            g_auto_test_running_state = STATE_AUTO_TEST_WAIT_HANDSHAKE;
            
            log_i("AUTO_TEST_PAUSE -> AUTO_TEST_RUNNING(WAIT_HANDSHAKE)");
            g_mode_state = STATE_MODE_AUTO_TEST_RUNNING;
        }
        break;
    }
}

bool auto_test_application_loop()
{
    g_curr_time = timer_get_ms();

    /* 超时定时器状态更新 */
    timeout_timer_update();

    wss_application_normal_loop();

    if (g_curr_time - g_heat_timer >= HEAT_TIMEOUT) {

        int pp = 2;
        lwl_app_context_t *context = lwl_app_core_context_get_instance();
        if (context && context->recorder) {
            pp = recorder_get_play_pause_state(context->recorder);
        }
        log_i("%d %d %d %d %d", g_mode_state, g_auto_test_running_state, pp, wifi_get_rssi(), wifi_get_cqi());

        g_heat_timer = g_curr_time;
    }

    if (wss_application_check_exit()) {
        return true;
    }
    return false;
}
