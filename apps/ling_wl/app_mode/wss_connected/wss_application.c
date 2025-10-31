#include "wss_application.h"
#include "wss_audio.h"
#include "cpu.h"
#include "../lwl_app_framework.h"
#include "net_music_play_my.h"
#include "app_lingwl.h"

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

static OS_MUTEX g_wss_mutex;
int play_voice_prompt=0;
music_player *player;

#define log_i log_mqtt
static void set_led_on_touch(void)     { uart_send_set_led_color(COLOR_PURPLE); }    //触摸按键时灯光变紫色
static void set_led_tts_play(void)      { uart_send_set_led_color(COLOR_BLUE); }    //音频播放时灯光变蓝色
static void set_led_idle(void)      { uart_send_set_led_color(COLOR_WHITE); }   //空闲时灯光变白色

#define NO_REQ_TIMEOUT_MS      (10 * 60 * 1000) // 5分钟
#define NO_E_TIMEOUT_MS      (5 * 60 * 1000) // 5分钟
#define NO_MOVE_TIMEOUT_MS   (5 * 60 * 1000) // 5分钟
#define DEV_CALL_TIMEOUT_MS   (1 * 1000) // 1s

static u32 g_curr_time = 0;
#define HEAT_TIMEOUT 1000
static u32 g_heat_timer = 0;
static u32 last_req_time = 0;

static int g_audio_playing = false;
static bool g_mic_on = false;
static bool multi_play_mode = false;

/* Task state */
typedef enum {
    DEVICE_IDLE,
    DEVICE_AUDIO,
} AppTaskStepState;

AppTaskStepState g_device_state = DEVICE_IDLE;

void wss_application_vailed()
{
    last_req_time = g_curr_time;
}

void wss_disconnect_cbk(void)
{
    log_i("[wss_application]odmp disconnect!!!");
    wss_exception_exit();
}

static void wss_application_print_state(int mode_state, char* tag)
{
    switch (mode_state) {
    case DEVICE_IDLE:
        log_i("[wss_application][%s]DEVICE_IDLE", tag);
        break;
    case DEVICE_AUDIO:
        log_i("[wss_application][%s]DEVICE_AUDIO", tag);
        break;
    }
}

static int g_touch_key_exit = false;

// 设置退出标志
static void wss_application_touch_key()
{
    g_touch_key_exit = true;
    wss_application_vailed();
}

// 查询退出标志
bool wss_application_check_touch_key()
{
    bool rst = g_touch_key_exit;
    g_touch_key_exit = false;
    return rst;
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
        g_mic_on = !g_mic_on;
        log_i("[wss_application]APPLICATION SHORT_CLICK %d", g_mic_on);
        set_mic(g_mic_on);
        if (!g_mic_on) {
            multi_play_mode = true;
        } else {
            multi_play_mode = false;
        }
        break;
    case LWL_EVENT_TYPE_UART_TOUCH_KEY_DOWN:
        if (e->uart_cmd_touch.touch_key_event_type == 0) {
            log_i("[wss_application]Touch key pressed down");
            // wss_application_touch_key();
            // play_prompt_sound("t_key.mp3");
            // set_led_on_touch();
        } else {
            log_i("[wss_application]Touch key released");
            // set_led_idle();
        }
        // wss_application_touch_key();
        break;
    case LWL_EVENT_TYPE_UART_SYNC:
        uart_response_synch_status(timer_get_ms() - last_req_time < NO_REQ_TIMEOUT_MS, MAX_LE_AUDIO_VOLUME);/// 0是idle 1是busy，必须回复
        break;
    case LWL_EVENT_TYPE_UART_MOVE_MORE_THAN_ONE_METER:
        log_i("[wss_application]MOVE_MORE_THAN_ONE_METER");
        break;
    }
}

static void wss_app_mode_play_cb(void *priv)
{
    os_mutex_pend(&g_wss_mutex, 0);
    play_voice_prompt = 0;
#ifndef C_SPEAKLER
    resume_le_audio();
#endif
    resume_mic();
    os_mutex_post(&g_wss_mutex);
}

void wss_app_mode_play(const char *fname) 
{
    os_mutex_pend(&g_wss_mutex, 0);
    log_i("wss_app_mode_play: %s", fname);
    wss_mic_stop();
    wss_request_stop();
    gpio_direction_output(IO_PORTA_00, 1);  //Enable C speaker
    set_dec_volume(player, INIT_VOLUME_VALUE); // 恢复默认音量
    
    int err = app_music_play_local_music_with_callback(player, fname, wss_app_mode_play_cb, NULL);
    if (!err) {
        play_voice_prompt = 1;
    }
    os_mutex_post(&g_wss_mutex);
}

static pthread_t monitor_thread = NULL;
static bool monitor_thread_running = false;
// 模式5 加速度传感器相关变量

void *wss_condition_monitor_thread(void *arg)
{
    u32 current_time = timer_get_ms();
    u32 last_E_found_time = current_time;
    u32 last_move_time = current_time;
    log_i("[wss_application]Starting WSS condition monitor thread...");
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

        if (current_time - last_req_time > NO_REQ_TIMEOUT_MS) {

            // 检查超时条件
            bool e_timeout = ((current_time - last_E_found_time) > NO_E_TIMEOUT_MS);
                            
            bool move_timeout = ((current_time - last_move_time) > NO_MOVE_TIMEOUT_MS);

            // 任一超时条件满足就切换模式
            if (e_timeout || move_timeout) {
                log_i("[wss_application]No E found or device not moving for a long time, switching to default mode.");
                lwl_app_mode_switch_to(APP_DEFAULT_MODE);
                monitor_thread_running = false;
                return NULL;
            }
        }
        os_time_dly(100);  // 100ms 延迟
    }
    monitor_thread_running = false;
    return NULL;
}

void wss_application_init()
{
    log_i("[wss_application]Init WSS application\n");
    if (!os_mutex_valid(&g_wss_mutex)) {
        os_mutex_create(&g_wss_mutex);
    }
    play_voice_prompt = 0;
    player = lwl_app_core_context_get_instance()->music_player;

    lwl_event_handler_add(input_event_handler_cb);
    odmp_register_disconnect_cbk(wss_disconnect_cbk);
    
    monitor_start();
    wss_request_init();//发送1
    wss_device_init();//设备初始化，包含设备队列等
    // wss_mic_stop(); // 停止录音

    g_mic_on = true;
    multi_play_mode = false;

   // 检查线程是否已经在运行
    if (!monitor_thread_running) {
        pthread_create(&monitor_thread, NULL, wss_condition_monitor_thread, NULL);
        monitor_thread_running = true; 
    }

    /* 注册lwl事件处理回调 */

    /* 重置 */
    g_audio_playing = false;
    g_curr_time = timer_get_ms();
    last_req_time = g_curr_time - DEV_CALL_TIMEOUT_MS; // 初始化为3秒前，避免刚启动就超时
}

void wss_application_uninit()
{
    log_i("[wss_application]Uninit WSS application\n");

    monitor_thread_running = false;
    if (!monitor_thread) {
        pthread_join(monitor_thread, NULL);  // 等待线程退出
        monitor_thread = NULL;
    }

    wss_request_uninit();
    wss_device_uninit();
    monitor_stop();//free all pdev
    bdo_disable();

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
        if (wss_device_is_calling() && !multi_play_mode) {
            // 多次唤醒模式下，持续检测设备呼叫状态
        } else {
            if (g_curr_time - last_req_time >= DEV_CALL_TIMEOUT_MS) {
                wss_device_loop();
            }
        }
    }

    // 状态机处理
    switch (g_device_state) {
    case DEVICE_IDLE:
        // 在IDLE状态下，检测是否有进入AUDIO的条件（可根据实际业务补充）
        // 这里只做示例，假设有某事件触发进入AUDIO
        if (is_player_busy()) {
            g_device_state = DEVICE_AUDIO;
            log_i("[wss_application]STATE: DEVICE_IDLE -> DEVICE_AUDIO");
            wss_application_print_state(g_device_state, "STEP_TO");
        } else if (wss_application_check_touch_key()) {
            // if (is_request_busy()) {
            //     log_i("[wss_application]is_request_busy, wss_request_stop.");
            //     wss_request_stop();
            // } else {
            //     log_i("[wss_application]wss_device_call.");
            wss_device_call();
            // }
            // multi_player_game();
        }
        break;

    case DEVICE_AUDIO:
        // AUDIO状态下，如果收到触摸按键事件则打断，回到IDLE
        if (wss_application_check_touch_key()) {
            g_device_state = DEVICE_IDLE;
            wss_application_print_state(g_device_state, "touch key down");
            set_led_idle();
            // wss_request_stop(); // 按键打断对话
            // wss_vad_start(60000); // 开始录音
            last_req_time = g_curr_time;
        } else if (!is_player_busy()) {
            log_i("[wss_application]STATE: DEVICE_AUDIO -> DEVICE_IDLE");
            g_device_state = DEVICE_IDLE;
            wss_application_print_state(g_device_state, "STEP_TO");
            set_led_idle();
            // play_prompt_sound("VadStart.mp3");
        } else {
            set_led_tts_play();
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
