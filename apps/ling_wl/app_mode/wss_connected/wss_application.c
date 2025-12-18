#include "wss_application.h"
#include "wss_request.h"
#include "wss_audio.h"
#include "wss_mic.h"
#include "wss_app_util.h"
#include "../lwl_app_framework.h"
#include "net_music_play_my.h"
#include "app_cookie.h"

#include "bt_ble/client/ble_distance.h"
#include "app_mode/lwl_app_core.h"
#include "protocol/odm/odmp.h"
#include "log/log_mqtt.h"
#include "../../SC7A20/sc7a20.h"
#include <string.h>

#include "lwip/stats.h" // 添加头文件

static OS_MUTEX g_wss_app_mutex;
int play_voice_prompt = 0;
music_player *player;

#define NO_REQ_TIMEOUT_MS       (10 * 60 * 1000) // 连续10分钟无请求则超时
#define NO_E_TIMEOUT_MS         (5 * 60 * 1000)  // 触发超时，5分钟内无Pi则关机
#define NO_MOVE_TIMEOUT_MS      (5 * 60 * 1000)  // 触发超时，5分钟内无移动则关机
#define DEV_CALL_TIMEOUT_MS     (3 * 1000)       // 等待后端发送tts_bind，超过3s则回到靠近触发

static u32 g_curr_time = 0;
static u32 last_req_time = 0;

static bool g_mic_on = false;
static bool multi_play_mode = false;

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

//=================================================================================================
// 联网有效工作状态维护
//=================================================================================================
void wss_application_vailed()
{
    last_req_time = g_curr_time;
    g_sensor_reset_alive_timer();
}

void wss_disconnect_cbk(void)
{
    log_i("[wss_application]odmp disconnect!!!");
    // os_time_dly(100);      // 修正：延时 1000ms (假设 1 tick = 10ms)
    // wss_exception_exit();
}

// 查询退出标志
bool wss_application_check_power_key()
{
    bool rst = g_power_key_retry;
    g_power_key_retry = false;
    return rst;
}

//=================================================================================================
// 麦克风相关
//=================================================================================================
void wss_application_resume_mic()
{
    log_i("[wss_application]wss_application_resume_mic touch=%d mic_on=%d", g_on_touch_key, g_mic_on);
    log_mqtt("🔄🔄🔄 麦克风重启");
    os_mutex_pend(&g_wss_app_mutex, 0);
    if (g_on_touch_key) {
        wss_mic_start();
    } else if (g_mic_on && (g_device_state == APP_IDLE || g_device_state == APP_VAD)) {
        wss_mic_start();
    } else if (!g_mic_on || g_device_state == APP_AUDIO) {
        wss_mic_stop();
    }
    os_mutex_post(&g_wss_app_mutex);
}

// 打开麦克风并启动vad录音
void wss_application_start_mic()
{
    log_i("[wss_application]STATE: APP_AUDIO -> APP_IDLE");
    g_device_state = APP_VAD;
    g_mic_on = true;
    wss_application_resume_mic();
    wss_vad_start(NO_REQ_TIMEOUT_MS); // 开始录音
}

//=================================================================================================
// 按键相关
//=================================================================================================

// 设置退出标志
static void wss_application_touch_key(bool press_down)
{
#if TOUCH_KEY_AVAILABLE == 0
    return;
#endif
    os_mutex_pend(&g_wss_app_mutex, 0);
    if (press_down) {
        wss_request_pause();
        wss_mic_stop();
        wss_asr_start();
        play_prompt_sound("t_key.mp3");
    } else {
        bool is_short_press = !is_recording_uploading(); //未上传音频，是短按
        wss_request_resume();
        wss_mic_stop();
        wss_asr_stop();
        if (is_short_press) {
            // 短按触摸键，打断/切换对话状态
            if (g_device_state == APP_VAD) {// 按键前在说话，按键触发
                log_i("[wss_application]STATE: APP_VAD -> APP_WAIT");
                log_mqtt("🔄🔄🔄 短按触摸键，打断/切换对话状态");
                g_device_state = APP_WAIT;
                wss_request_send_event_target(ORMET_BUTTON_TRIGGER); 
                wss_application_vailed();
            } else {             // 按键前未在说话
                log_i("[wss_application]STATE: Touch key -> APP_VAD");
                wss_application_start_mic();
            }
        } else {
            // 长按默认进剧本和多角色模式
            log_i("[wss_application]STATE: long press -> APP_WAIT");
            g_device_state = APP_WAIT;
            multi_play_mode = true;
            g_mic_on = false;
            last_req_time = g_curr_time + 10000;
        }
    }
    os_mutex_post(&g_wss_app_mutex);
}

static void input_event_handler_cb(lwl_event_t *e)
{
    static int en = 1;
    lwl_eend_dev_t *p_eend_dev = e->distance_change.pdev;

    switch (e->event_type) {
    case LWL_EVENT_TYPE_KEY:
        log_mqtt("👆👆👆 短按 此时multi_play_mode=%d", multi_play_mode);
        if (wss_check_exit()) {
            g_power_key_retry = true;
        } else {
            multi_play_mode = !multi_play_mode;
            g_mic_on = !multi_play_mode; // 自由模式关闭麦克风
            if (!wss_request_is_playing()) {
                if (multi_play_mode) {
                    play_prompt_sound("1to2.mp3");
                    wss_request_send_event_target(ORMET_PLAY_NEXT);
                    log_i("[wss_application]STATE: MODE SWITCH -> APP_WAIT");
                    g_device_state = APP_WAIT;
                } else {
                    play_prompt_sound("1to1.mp3");
                    if (wss_device_call()) {
                        log_i("[wss_application]STATE: MODE SWITCH -> APP_WAIT");
                        g_device_state = APP_WAIT;
                    } else {
                        log_i("[wss_application]STATE: MODE SWITCH no device call -> APP_IDLE");
                        g_device_state = APP_IDLE;
                    }
                }
            } else {
                wss_application_resume_mic();
            }
        }
        break;
    case LWL_EVENT_TYPE_TOUCH_KEY:
        if (e->touch_key_cmd.touch_key_event_type == 0) {
            log_mqtt("👆👆 触摸键按下");
            g_on_touch_key = true;
            wss_application_touch_key(true);
        } else {
            log_mqtt("👇👇 触摸键释放");
            wss_application_touch_key(false);
            g_on_touch_key = false;
        }
        wss_application_vailed();
        break;
    case LWL_EVENT_TYPE_GSENSOR_IDLE:
        // g_sensor_set_alive_timeout(NO_REQ_TIMEOUT_MS);
        break;
    case LWL_EVENT_TYPE_LOW_BATTERY:
        lwl_app_mode_switch_to(APP_DEFAULT_MODE);
        break;
    }
}

//=================================================================================================
// 播放提示音相关
//=================================================================================================
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
    play_voice_prompt = 0;
    // wss_application_resume_mic();
    bdo_enable();
}

void wss_app_mode_play() 
{
    if (music_file[0] == 0) {
        return;
    }
    lwl_app_mode_stop_playing();
    if (play_voice_prompt == 1) {
        play_voice_prompt = 0;
    } else if (g_device_state == APP_AUDIO) {
        wss_ble_audio_stop();
    }
    // bdo_disable();
    log_i("[wss_application]wss_app_mode_play: %s", music_file);
    wss_mic_stop(); // 停止录音
    gpio_direction_output(IO_PORTA_00, 1);  //Enable C speaker
    set_dec_volume(player, INIT_VOLUME_VALUE); // 恢复默认音量
    int err = app_music_play_local_music_with_callback(player, music_file, wss_app_mode_play_cb, NULL);
    // int err = app_music_play_mix_file(music_file);
    if (!err) {
        play_voice_prompt = 1;
    }
    music_file[0] = 0;
}

void play_prompt(const char *fname) 
{
    log_i("[wss_application]prepare to play_prompt: %s", fname);
    memcpy(music_file, fname, strlen(fname)+1);
}

//=================================================================================================
// 休眠退出相关
//=================================================================================================
static pthread_t monitor_thread = NULL;
static bool monitor_thread_running = false;
// 模式5 加速度传感器相关变量

void *wss_condition_monitor_thread(void *arg)
{
    u32 current_time = timer_get_ms();
    u32 last_E_found_time = current_time;
    u32 last_move_time = current_time;
    u32 last_mem_stats_time = current_time; // 新增变量

    log_i("[wss_application]Starting WSS condition monitor thread...");
    while (monitor_thread_running && lwl_app_mode_get_current_mode() == APP_WSS_CONNECTED_MODE) {
        current_time = timer_get_ms();
        
        // 检查E设备
        if (get_5m_edev_count() > 0) {  // 检测到E设备
            last_E_found_time = current_time;
        }

        // 检查设备移动状态
        if (is_device_moving()) {  // 设备在移动
            last_move_time = current_time;
        }

        // 每60秒打印一次LwIP内存统计
        if (current_time - last_mem_stats_time > 60000) {
            last_mem_stats_time = current_time;
            
            log_i("=== LwIP Memory Stats Debug ===");
            
            // 1. 尝试打印 Heap (System Heap / LwIP Heap)
            #if MEM_STATS
            log_i("[HEAP] avail: %d, used: %d, max: %d, err: %d", 
                  lwip_stats.mem.avail, lwip_stats.mem.used, 
                  lwip_stats.mem.max, lwip_stats.mem.err);
            #else
            log_i("[HEAP] MEM_STATS not enabled");
            #endif

            // 2. 打印 PBUF_POOL (关键!)
            #if MEMP_STATS
            log_i("[PBUF] avail: %d, used: %d, max: %d, err: %d",
                  lwip_stats.memp[MEMP_PBUF_POOL]->avail,
                  lwip_stats.memp[MEMP_PBUF_POOL]->used,
                  lwip_stats.memp[MEMP_PBUF_POOL]->max,
                  lwip_stats.memp[MEMP_PBUF_POOL]->err);
                  
            // 3. 打印 TCP_SEG (防止发送积压)
            log_i("[SEG ] avail: %d, used: %d, max: %d, err: %d",
                  lwip_stats.memp[MEMP_TCP_SEG]->avail,
                  lwip_stats.memp[MEMP_TCP_SEG]->used,
                  lwip_stats.memp[MEMP_TCP_SEG]->max,
                  lwip_stats.memp[MEMP_TCP_SEG]->err);
            #endif
            
            log_i("===============================");
        }

        // 打印NO_E_TIMEOUT_MS
        // log_i("[wss_application]NO_E_TIMEOUT_MS: %d", NO_E_TIMEOUT_MS);

        if (current_time - last_req_time > NO_REQ_TIMEOUT_MS) {

            // 检查超时条件
            bool e_timeout = ((current_time - last_E_found_time) > NO_E_TIMEOUT_MS);
                            
            bool move_timeout = ((current_time - last_move_time) > NO_MOVE_TIMEOUT_MS);

            // 任一超时条件满足就切换模式
            if (e_timeout || move_timeout) {
                log_i("[wss_application]No E or movement timeout (e_to=%u, move_to=%u, last_req_delta=%u), switching to default mode.",
                      current_time - last_E_found_time,
                      current_time - last_move_time,
                      current_time - last_req_time);
                lwl_app_mode_switch_to(APP_POWER_OFF_MODE);
                monitor_thread_running = false;
                return NULL;
            }
        }
        msleep(100);  // 100ms 延迟
    }
    monitor_thread_running = false;
    return NULL;
}

//=================================================================================================
// 应用初始化和循环
//=================================================================================================
void wss_application_init()
{
    log_i("[wss_application]Init WSS application\n");
    log_mqtt("🤖🤖🤖 初始化WSS应用");
    g_sensor_reset_alive_timer();
    g_sensor_set_alive_timeout(NO_REQ_TIMEOUT_MS);
    play_voice_prompt = 0;
    player = lwl_app_core_context_get_instance()->music_player;

    odmp_register_disconnect_cbk(wss_disconnect_cbk);
    
    monitor_start();
    wss_audio_init();
    wss_mic_init();
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

    /* 重置 */
    g_curr_time = timer_get_ms();
    last_req_time = g_curr_time - DEV_CALL_TIMEOUT_MS; // 初始化为3秒前，避免刚启动就超时

    if (!os_mutex_valid(&g_wss_app_mutex)) {
        os_mutex_create(&g_wss_app_mutex);
    }

    /* 注册lwl事件处理回调 */
    lwl_event_handler_add(input_event_handler_cb);
}

void wss_application_uninit()
{
    log_i("[wss_application]Uninit WSS application\n");

    /* 取消注册lwl事件处理回调 */
    lwl_event_handler_remove(input_event_handler_cb);

    os_mutex_pend(&g_wss_app_mutex, 0);
    g_mic_on = false;
    multi_play_mode = false;

    wss_request_uninit();
    wss_audio_uninit();
    wss_mic_uninit();
    wss_device_uninit();
    monitor_stop();//free all pdev
    bdo_disable();

    os_mutex_post(&g_wss_app_mutex);
}

void wss_application_reset()
{
    log_i("[wss_application]Reset WSS application\n");
    log_mqtt("🤖🤖🤖 重置WSS应用");
    g_sensor_reset_alive_timer();
    g_sensor_set_alive_timeout(NO_REQ_TIMEOUT_MS);
    play_voice_prompt = 0;
    wss_request_init();//发送1
    wss_device_init();//设备初始化，包含设备队列等
    wss_mic_stop(); // 停止录音

    g_mic_on = true;
    multi_play_mode = false;
    music_file[0] = 0;

    /* 重置 */
    g_curr_time = timer_get_ms();
    last_req_time = g_curr_time - DEV_CALL_TIMEOUT_MS; // 初始化为3秒前，避免刚启动就超时

    /* 注册lwl事件处理回调 */
    lwl_event_handler_add(input_event_handler_cb);
}

bool wss_application_loop()
{
    g_curr_time = timer_get_ms();
    if (wss_check_exit()) {
        if (wss_request_is_busy()) {
            log_i("[wss_application]Exception exit detected (reason=%s), stopping busy request",
            wss_get_last_disconnect_reason());
            wss_request_pause();
            wss_mic_stop();
        }
        if (g_device_state != APP_IDLE) {
            log_i("[wss_application]STATE: wss_check_exit -> APP_IDLE");
            g_device_state = APP_IDLE;
        }
        return true;
    }
    if (music_file[0] != 0 && play_voice_prompt == 0) {
        wss_app_mode_play();
    }

    wss_request_loop();
    if (play_voice_prompt || g_on_touch_key) {
        return false;
    }

    // 状态机处理
    switch (g_device_state) {
    case APP_IDLE:
        if (wss_request_is_playing()) {
            log_i("[wss_application]STATE: APP_IDLE -> APP_AUDIO");
            g_device_state = APP_AUDIO;
            wss_mic_stop(); // 停止录音
        } else if (g_mic_on && wss_device_is_calling()) {
            log_i("[wss_application]STATE: APP_IDLE -> APP_VAD");
            wss_application_start_mic();
        } else if (!wss_device_is_calling() || multi_play_mode) {
            if (wss_device_loop()) {
                log_i("[wss_application]STATE: APP_IDLE -> APP_WAIT");
                g_device_state = APP_WAIT;
                wss_application_vailed();
            }
        }
        break;

    case APP_AUDIO:
        // AUDIO状态下，如果收到触摸按键事件则打断，回到IDLE
        if (!wss_request_is_playing()) { //也可能是超时退出
            if (multi_play_mode) {
                log_i("[wss_application]STATE: APP_AUDIO -> APP_WAIT");
                wss_request_send_event_target(ORMET_PLAY_NEXT);
                g_device_state = APP_WAIT;
            } else if (wss_device_is_calling() && g_mic_on) {
                wss_application_start_mic();
            } else {
                log_i("[wss_application]STATE: APP_AUDIO -> APP_IDLE");
                wss_request_send_event_target(ORMET_PLAY_NEXT);
                g_device_state = APP_WAIT;
            }
        }
        wss_application_vailed();
        break;

    case APP_VAD: // until audio upload end
        // 音频上传结束，回到IDLE状态
        if (wss_request_is_playing()) {
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
        if (wss_request_is_playing()) {
            log_i("[wss_application]STATE: APP_WAIT -> APP_AUDIO");
            g_device_state = APP_AUDIO;
            wss_mic_stop(); // 停止录音
        } else if (wss_request_is_busy()) {
            // 继续等待
        } else if (g_curr_time - last_req_time >= DEV_CALL_TIMEOUT_MS) {
            log_i("[wss_application]STATE: APP_WAIT -> APP_IDLE");
            log_mqtt("🧬🧬🧬 等待超时");
            g_device_state = APP_IDLE;
        }
        break;
    }

    return false;
}
