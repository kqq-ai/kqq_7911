#include "wss_application.h"
#include "wss_audio.h"
#include "cpu.h"

#include "../lwl_app_framework.h"
#include "util/cqueue.h"
#include "util/ringbuffer.h"
#include "bt_ble/client/ble_distance.h"
#include "async_msg_loop.h"
#include "app_mode/lwl_app_core.h"
#include "device.h"
#include "util/threadpool_tasks.h"

#include "ota/http_ota.h"
#include "uart.h"
#include "protocol/odm/odmp.h"

#include "protocol/odm/odm_secure.h"
#include "protocol/cp/c_uart_p.h"
#include "log/log_mqtt.h"

#include "../../SC7A20/sc7a20.h"

#define log_i log_mqtt

static u32 g_curr_time = 0;
#define HEAT_TIMEOUT 1000
static u32 g_heat_timer = 0;
static u32 last_req_time = 0;
static u8 last_req_volume = 12;

static int g_audio_playing = false;

/* Task state */
typedef enum {
    DEVICE_IDLE,
    DEVICE_AUDIO,
} AppTaskStepState;

AppTaskStepState g_device_state = DEVICE_IDLE;

void wss_application_vailed(int Volume)
{
    last_req_volume = Volume;
    last_req_time = g_curr_time;
}

static int odmp_disconnect_cbk(void)
{
    log_i("odmp disconnect!!!");
    wss_exception_exit();
}

static void wss_application_print_state(int mode_state, char* tag)
{
    switch (mode_state) {
    case DEVICE_IDLE:
        log_i("[%s]DEVICE_IDLE", tag);
        break;
    case DEVICE_AUDIO:
        log_i("[%s]DEVICE_AUDIO", tag);
        break;
    }
}

static int g_touch_key_exit = false;

// 设置退出标志
static void wss_application_touch_key_exit()
{
    g_touch_key_exit = true;
}

// 查询退出标志
static bool wss_application_check_touch_key_exit()
{
    return g_touch_key_exit;
}

//=================================================================================================
// LWL event callback
//=================================================================================================
static void input_event_handler_cb(lwl_event_t *e)
{
    static int en = 1;
    lwl_eend_dev_t *p_eend_dev = e->distance_change.pdev;

    switch (e->event_type) {
    case LWL_EVENT_TYPE_UART_IO_KEY_SHORT_CLICK:
        log_i("APPLICATION STOP");
        // lwl_app_mode_switch_to(APP_DEFAULT_MODE);
        wss_application_touch_key_exit();
        break;
    case LWL_EVENT_TYPE_UART_TOUCH_KEY_DOWN:
        log_i("TOUCH KEY DOWN");
        // wss_application_touch_key_exit();
        break;
    case LWL_EVENT_TYPE_UART_TOUCH_KEY_UP:
        log_i("TOUCH KEY UP");
        // 处理触摸按键抬起事件
        break;
    case LWL_EVENT_TYPE_UART_CLEAR_WIFI_SSID:
        // Return to Default Mode
        lwl_app_mode_switch_to(APP_DEFAULT_MODE);
        break;
    case LWL_EVENT_TYPE_UART_SYNC:
    //    log_i("SYNC response by input_event_handler_cb (%d, %d)", is_request_busy(), last_req_volume);
        uart_response_synch_status(is_request_busy(), last_req_volume);/// 0是idle 1是busy，必须回复
        break;
    case LWL_EVENT_TYPE_UART_MOVE_MORE_THAN_ONE_METER:
        log_i("MOVE_MORE_THAN_ONE_METER");
        break;
    }
}



static pthread_t monitor_thread = NULL;
static bool monitor_thread_running = false;
#define NO_E_TIMEOUT_MS      (50 * 60 * 1000) // 5分钟
#define NO_MOVE_TIMEOUT_MS   (50 * 60 * 1000) // 5分钟
// 模式5 加速度传感器相关变量

void *wss_condition_monitor_thread(void *arg)
{
    u32 current_time = timer_get_ms();
    u32 last_E_found_time = current_time;
    u32 last_move_time = current_time;
    log_i("Starting WSS condition monitor thread...");
    while (monitor_thread_running) {
        current_time = timer_get_ms();
        
        // 检查E设备
        if (get_5m_edev_count() > 0) {  // 检测到E设备
            last_E_found_time = current_time;
        }

        // 检查设备移动状态
        if (is_device_moving()) {  // 设备在移动
            last_move_time = current_time;
        }

        // 检查超时条件
        bool e_timeout = ((current_time - last_E_found_time) > NO_E_TIMEOUT_MS);
                         
        bool move_timeout = ((current_time - last_move_time) > NO_MOVE_TIMEOUT_MS);

        // 任一超时条件满足就切换模式
        if (e_timeout || move_timeout) {
            log_i("No E found or device not moving for a long time, switching to default mode.");
            lwl_app_mode_switch_to(APP_DEFAULT_MODE);
            monitor_thread_running = false;
            return NULL;
        }
        os_time_dly(100);  // 100ms 延迟
    }
    monitor_thread_running = false;
    return NULL;
}

void wss_application_init()
{
    log_i("Init WSS application\n");
    lwl_event_handler_add(input_event_handler_cb);
    odmp_register_disconnect_cbk(odmp_disconnect_cbk);
    
    monitor_start();
    wss_audio_init();
    wss_request_init();//发送1
    wss_device_init();//设备初始化，包含设备队列等

   // 检查线程是否已经在运行
    if (!monitor_thread_running) {
        pthread_create(&monitor_thread, NULL, wss_condition_monitor_thread, NULL);
        monitor_thread_running = true; 
    }

    /* 注册lwl事件处理回调 */

    /* 重置 */
    g_audio_playing = false;
    last_req_time = 0;
}

void wss_application_uninit()
{
    log_i("Uninit WSS application\n");

    monitor_thread_running = false;
    if (!monitor_thread) {
        pthread_join(monitor_thread, NULL);  // 等待线程退出
        monitor_thread = NULL;
    }

    wss_audio_uninit();
    wss_request_uninit();
    wss_device_uninit();
    monitor_stop();//free all pdev
    /* 取消注册lwl事件处理回调 */
    lwl_event_handler_remove(input_event_handler_cb);
}

bool wss_application_loop()
{
    g_curr_time = timer_get_ms();
    if (wss_check_exit()) {
        return true;
    }

    if (wss_request_loop()) {
        if (g_curr_time - last_req_time >= 3000) {
            wss_device_loop();
        }
    }

    // 状态机处理
    switch (g_device_state) {
    case DEVICE_IDLE:
        // 在IDLE状态下，检测是否有进入AUDIO的条件（可根据实际业务补充）
        // 这里只做示例，假设有某事件触发进入AUDIO
        if (is_audio_playing()) {
            g_device_state = DEVICE_AUDIO;
            log_i("STATE: DEVICE_IDLE -> DEVICE_AUDIO");
            wss_application_print_state(g_device_state, "STEP_TO");
        } else if (wss_application_check_touch_key_exit()) {
            // multi_player_game();
            wss_mic_stop(); // 停止录音
            wss_request_stop(); // 按键打断对话
            wss_application_vailed(12);
            wss_request_send_event(NULL, ORMET_BUTTON_PRESS);
            g_touch_key_exit = false; // 清除标志
        }
        break;

    case DEVICE_AUDIO:
        // AUDIO状态下，如果收到触摸按键事件则打断，回到IDLE
        if (wss_application_check_touch_key_exit()) {
            g_device_state = DEVICE_IDLE;
            wss_request_stop(); // 按键打断对话
            last_req_time = 0;
            log_i("STATE: DEVICE_AUDIO -> DEVICE_IDLE (touch key down)");
            g_touch_key_exit = false; // 清除标志
        }
        if (!is_audio_playing()) {
            g_device_state = DEVICE_IDLE;
            log_i("STATE: DEVICE_AUDIO -> DEVICE_IDLE");
            wss_application_print_state(g_device_state, "STEP_TO");
        }
        break;
    }

    msleep(10);

    if (g_curr_time - g_heat_timer >= HEAT_TIMEOUT) {
        dump_all_eend_info();
        g_heat_timer = g_curr_time;
    }

    return false;
}