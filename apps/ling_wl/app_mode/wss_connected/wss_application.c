#include "wss_application.h"
#include "wss_request.h"
#include "wss_audio.h"
#include "cpu.h"
#include "../lwl_app_framework.h"
#include "net_music_play_my.h"
#include "app_cookie.h"

#include "wss/wss_client.h"
#include "util/cqueue.h"
#include "util/ringbuffer.h"
#include "bt_ble/client/ble_distance.h"
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
int play_voice_prompt = 0;
music_player *player;

#define log_i log_mqtt
static void set_led_on_touch(void)      { uart_send_set_led_color(COLOR_ORANGE); }      //触摸按键时灯光变黄色
static void set_led_on_normal_key(void) { uart_send_set_led_color(COLOR_BLUE); }        //触摸按键时灯光变蓝色
static void set_led_idle(void)          { uart_send_set_led_color(COLOR_WHITE); }       //空闲时灯光变白色

#define NO_REQ_TIMEOUT_MS       (10 * 60 * 1000) // 10分钟
#define NO_E_TIMEOUT_MS         (5 * 60 * 1000)  // 5分钟
#define NO_MOVE_TIMEOUT_MS      (5 * 60 * 1000)  // 5分钟
#define DEV_CALL_TIMEOUT_MS     (3 * 1000)       // 3s

static u32 g_curr_time = 0;
static u32 last_req_time = 0;

static int g_audio_playing = false;
static bool g_mic_on = false;
static bool multi_play_mode = false;
static bool is_user_speaking = false;

static bool g_on_touch_key = false;
static bool g_power_key_retry = false;

/* Task state */
typedef enum {
    APP_IDLE,
    APP_AUDIO,
    APP_VAD,
    APP_WAIT,
} AppTaskStepState;

AppTaskStepState g_device_state = APP_IDLE;

void wss_application_vailed()
{
    last_req_time = g_curr_time;
}

void wss_disconnect_cbk(void)
{
    log_i("[wss_application]odmp disconnect!!!");
    wss_exception_exit();
}

void wss_request_resume_mic()
{
    os_mutex_pend(&g_wss_mutex, 0);
    if (g_on_touch_key) {
        wss_mic_start();
    }else if (g_mic_on && (g_device_state == APP_IDLE || g_device_state == APP_VAD)) {
        wss_mic_start();
    } else if (!g_mic_on || g_device_state == APP_AUDIO) {
        wss_mic_stop();
    }
    os_mutex_post(&g_wss_mutex);
}

// 查询退出标志
bool wss_application_check_power_key()
{
    os_mutex_pend(&g_wss_mutex, 0);
    bool rst = g_power_key_retry;
    g_power_key_retry = false;
    os_mutex_post(&g_wss_mutex);
    return rst;
}

// 设置退出标志
static void wss_application_touch_key(bool press_down)
{
#if TOUCH_KEY_AVAILABLE == 0
    return;
#endif
    os_mutex_pend(&g_wss_mutex, 0);
    if (press_down) {
        is_user_speaking = (g_device_state == APP_VAD);
        wss_request_touch_key_down();
        log_i("[wss_application]STATE: Touch key -> APP_IDLE");
        g_device_state = APP_IDLE;
        play_prompt_sound("t_key.mp3");
        set_led_on_touch();
    } else {
        wss_mic_stop();
        if (wss_request_touch_key_up()) {   // 是短按
            if (!is_user_speaking) {        // 按键前未在说话
                g_mic_on = true;
            } else {                        // 按键前在说话，按键触发
                log_i("[wss_application]STATE: Touch key -> APP_WAIT");
                wss_request_send_event_target(ORMET_BUTTON_TRIGGER); 
                g_device_state = APP_WAIT;
                wss_application_vailed();
            }
        } else {                            // 是长按
            multi_play_mode = true;         // 长按默认进剧本和多角色模式
            g_mic_on = !multi_play_mode;    // 自由模式关闭麦克风
            wss_request_resume_mic();
            last_req_time = g_curr_time + 10000;
        }
        set_led_idle();
    }
    os_mutex_post(&g_wss_mutex);
}

//=================================================================================================
// LWL event callback
//=================================================================================================
static void input_event_handler_cb(lwl_event_t *e)
{
    os_mutex_pend(&g_wss_mutex, 0);
    static int en = 1;
    lwl_eend_dev_t *p_eend_dev = e->distance_change.pdev;

    switch (e->event_type) {
    case LWL_EVENT_TYPE_UART_IO_KEY_SHORT_CLICK:
        log_i("[wss_application]APPLICATION SHORT_CLICK %d", multi_play_mode);
        if (wss_check_exit()) {
            g_power_key_retry = true;
        } else {
            multi_play_mode = !multi_play_mode;
            g_mic_on = !multi_play_mode; // 自由模式关闭麦克风
            wss_request_resume_mic();
        }
        break;
    case LWL_EVENT_TYPE_UART_TOUCH_KEY_DOWN:
        if (e->uart_cmd_touch.touch_key_event_type == 0) {
            log_i("[wss_application]Touch key pressed down");
            g_on_touch_key = true;
        } else {
            log_i("[wss_application]Touch key released");
            g_on_touch_key = false;
        }
        wss_application_vailed();
        wss_application_touch_key(g_on_touch_key);
        break;
    case LWL_EVENT_TYPE_UART_SYNC:
        uart_response_synch_status(timer_get_ms() - last_req_time < NO_REQ_TIMEOUT_MS, MAX_LE_AUDIO_VOLUME);/// 0是idle 1是busy，必须回复
        break;
    case LWL_EVENT_TYPE_UART_MOVE_MORE_THAN_ONE_METER:
        log_i("[wss_application]MOVE_MORE_THAN_ONE_METER");
        break;
    }
    os_mutex_post(&g_wss_mutex);
}

char music_file[128];

void stop_prompt()
{
    log_i("stop_prompt %d", play_voice_prompt);
    music_file[0] = 0;
    play_voice_prompt = 0;
}

static void wss_app_mode_play_cb(void *priv)
{
    log_i("[wss_application]wss_app_mode_play_cb turnoff cookie speaker");
    os_mutex_pend(&g_wss_mutex, 0);
    play_voice_prompt = 0;
    wss_request_resume_mic();
    os_mutex_post(&g_wss_mutex);
}

void wss_app_mode_play() 
{
    os_mutex_pend(&g_wss_mutex, 0);
    if (music_file[0] == 0) {
        return;
    }
    if (play_voice_prompt == 1 || g_device_state == APP_AUDIO) {
        play_voice_prompt = 0;
        music_play_stop();
    }
    log_i("[wss_application]wss_app_mode_play: %s", music_file);
    gpio_direction_output(IO_PORTA_00, 1);  //Enable C speaker
    set_dec_volume(player, INIT_VOLUME_VALUE); // 恢复默认音量
    
    int err = app_music_play_local_music_with_callback(player, music_file, wss_app_mode_play_cb, NULL);
    // int err = app_music_play_mix_file(music_file);
    if (!err) {
        play_voice_prompt = 1;
    }
    music_file[0] = 0;
    os_mutex_post(&g_wss_mutex);
}

void play_prompt(const char *fname) 
{
    os_mutex_pend(&g_wss_mutex, 0);
    log_i("play_prompt: %s", fname);
    memcpy(music_file, fname, strlen(fname)+1);
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
    wss_mic_stop(); // 停止录音

    g_mic_on = true;
    multi_play_mode = false;
    music_file[0] = 0;

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
        if (wss_request_is_busy()) wss_request_stop();
        return true;
    }
    if (music_file[0] != 0 && play_voice_prompt == 0) {
        wss_app_mode_play();
    }

    // if (play_voice_prompt || g_on_touch_key) {
    //     return false;
    // }
    if (wss_request_loop() && !g_on_touch_key && play_voice_prompt == 0) {
        if ((!wss_device_is_calling() || multi_play_mode) && g_curr_time - last_req_time >= DEV_CALL_TIMEOUT_MS) {
            wss_device_loop();
        }
    }

    // 状态机处理
    switch (g_device_state) {
    case APP_IDLE:
        if (wss_request_is_busy()) {
            log_i("[wss_application]STATE: APP_IDLE -> APP_AUDIO");
            g_device_state = APP_AUDIO;
            wss_mic_stop(); // 停止录音
        } else if (g_mic_on && wss_device_is_calling()) {
            log_i("[wss_application]STATE: APP_IDLE -> APP_VAD");
            g_device_state = APP_VAD;
            wss_vad_start(600000); // 开始录音
        // } else if (!wss_device_is_calling() || multi_play_mode) {
        //     wss_device_loop();
        }
        break;

    case APP_AUDIO:
        // AUDIO状态下，如果收到触摸按键事件则打断，回到IDLE
        if (!wss_request_is_busy()) {
            if (multi_play_mode) {
                log_i("[wss_application]STATE: APP_AUDIO -> APP_WAIT");
                wss_request_send_event_target(ORMET_PLAY_NEXT);
                g_device_state = APP_WAIT;
            } else {
                log_i("[wss_application]STATE: APP_AUDIO -> APP_VAD");
                wss_request_send_event_target(ORMET_PLAY_END);
                g_device_state = APP_VAD;
                wss_request_resume_mic();
                wss_vad_start(600000); // 开始录音
            }
        }
        wss_application_vailed();
        break;

    case APP_VAD: // until audio upload end
        // 音频上传结束，回到IDLE状态
        if (wss_request_is_busy()) {
            log_i("[wss_application]STATE: APP_VAD -> APP_AUDIO");
            g_device_state = APP_AUDIO;
            g_mic_on = !multi_play_mode; // 自由模式关闭麦克风
            wss_mic_stop(); // 停止录音
        }
        if (!wss_device_is_calling()) {
            log_i("[wss_application]STATE: APP_VAD -> APP_IDLE");
            g_device_state = APP_IDLE;
        }
        break;

    case APP_WAIT: // until TTS_bind
        // 等待下个角色说话
        if (wss_request_is_busy()) {
            log_i("[wss_application]STATE: APP_WAIT -> APP_AUDIO");
            g_device_state = APP_AUDIO;
            wss_mic_stop(); // 停止录音
        }
        if (g_curr_time - last_req_time >= DEV_CALL_TIMEOUT_MS) {
            log_i("[wss_application]STATE: APP_WAIT -> APP_IDLE");
            g_device_state = APP_IDLE;
        }
        break;
    }

    return false;
}
