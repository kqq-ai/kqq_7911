#include "wss_application.h"
#include "wss_request.h"
#include "wss_audio.h"
#include "wss_mic.h"
#include "wss_app_util.h"
#include "wss_device.h"
#include "../lwl_app_framework.h"
#include "net_music_play_my.h"
#include "app_cookie.h"

#include "bt_ble/client/ble_distance.h"
#include "app_mode/lwl_app_core.h"
#include "protocol/odm/odmp.h"
#include "log/log_mqtt.h"
#include "../../SC7A20/sc7a20.h"
#include <string.h>
#include <stdlib.h>

#include "lwip/stats.h" // 添加头文件
#include "system/timer.h" // 定时器API

static OS_MUTEX g_wss_app_mutex;
music_player *player;

#define NO_REQ_TIMEOUT_MS       (10 * 60 * 1000) // 连续10分钟无请求则超时
#define NO_E_TIMEOUT_MS         (5 * 60 * 1000)  // 触发超时，5分钟内无Pi则关机
#define NO_MOVE_TIMEOUT_MS      (5 * 60 * 1000)  // 触发超时，5分钟内无移动则关机
#define DEV_CALL_TIMEOUT_MS     (3 * 1000)       // 等待后端发送tts_bind，超过3s则回到靠近触发

#define INPUT_EVENT_QUEUE_SIZE  32

static u32 g_curr_time = 0;
static u32 last_req_time = 0;

static bool g_on_touch_key = false;
static bool g_power_key_retry = false;
static bool g_exit = false;

static u32 g_touch_key_press_time = 0;
static bool g_long_press_mic_started = false;  // 标记是否已经因为长按而开启了麦克风
static u16 g_long_press_timer_id = 0;  // 长按定时器ID
#define LONG_PRESS_THRESHOLD_MS 300  // 长按阈值300ms

// 按钮点击事件类型
#define BTN_TAP_TYPE_PRESS    1  // 按下
#define BTN_TAP_TYPE_RELEASE  0  // 释放

u32 last_E_found_time = 0;
u32 last_move_time = 0;
u32 last_mem_stats_time = 0;

/* 输入事件队列 */
typedef struct {
    LWL_EVENT_TYPE event_type;
    union {
        struct {
            bool press_down;
        } key_event;
        struct {
            bool is_press_down;
        } touch_key_event;
    } data;
} input_event_t;

static input_event_t g_event_queue[INPUT_EVENT_QUEUE_SIZE];
static int g_event_queue_head = 0;
static int g_event_queue_tail = 0;

//=================================================================================================
// 事件队列操作
//=================================================================================================
static bool input_event_queue_is_empty(void)
{
    return g_event_queue_head == g_event_queue_tail;
}

static bool input_event_queue_is_full(void)
{
    return (g_event_queue_tail + 1) % INPUT_EVENT_QUEUE_SIZE == g_event_queue_head;
}

static bool input_event_queue_push(const input_event_t *event)
{
    if (input_event_queue_is_full()) {
        log_w("[wss_application]Event queue is full, drop event %d", event->event_type);
        return false;
    }
    
    g_event_queue[g_event_queue_tail] = *event;
    g_event_queue_tail = (g_event_queue_tail + 1) % INPUT_EVENT_QUEUE_SIZE;
    return true;
}

static bool input_event_queue_pop(input_event_t *event)
{
    if (input_event_queue_is_empty()) {
        return false;
    }
    
    *event = g_event_queue[g_event_queue_head];
    g_event_queue_head = (g_event_queue_head + 1) % INPUT_EVENT_QUEUE_SIZE;
    return true;
}


//=================================================================================================
// 联网有效工作状态维护
//=================================================================================================
void wss_application_vailed()
{
    last_req_time = g_curr_time;
    last_move_time = g_curr_time;
    g_sensor_reset_alive_timer();
}

void wss_disconnect_cbk(void)
{
    log_i("[wss_application]odmp disconnect!!!");
    // os_time_dly(100);      // 修正：延时 1000ms (假设 1 tick = 10ms)
}

// 查询退出标志
bool wss_application_check_power_key()
{
    bool rst = g_power_key_retry;
    g_power_key_retry = false;
    return rst;
}

//=================================================================================================
// 按键相关
//=================================================================================================

//=================================================================================================
// 触摸按键事件处理函数（长按/短按）
//=================================================================================================

// 长按超时回调函数：当达到长按阈值时触发
static void long_press_timeout_callback(void *priv)
{
    os_mutex_pend(&g_wss_app_mutex, 0);
    
    // 检查按键是否仍然按下且未开启麦克风
    if (g_on_touch_key && !g_long_press_mic_started) {
        // 达到长按阈值，开启麦克风
        log_mqtt("【butt】🔄🔄🔄 检测到长按，开启麦克风");
        log_i("【butt】🔄🔄🔄 检测到长按，开启麦克风");
        const char *stream_id = wss_request_get_stream_id();
        if (stream_id != NULL && stream_id[0] != '\0') {
            wss_mic_start(stream_id);
            g_long_press_mic_started = true;  // 标记已经开启，避免重复开启
        } else {
            log_e("[wss_application]stream_id is empty, cannot start mic");
        }
    }
    
    // 定时器是一次性的，会自动删除，这里清空ID
    g_long_press_timer_id = 0;
    
    os_mutex_post(&g_wss_app_mutex);
}

static void handle_long_press_end(void)
{
    log_i("【butt】🔄🔄🔄 长按触摸键释放 - 关闭麦克风");
    // 1. 停止麦克风录音和音频上传
    wss_mic_stop();
    send_audio_stream_end(wss_request_get_stream_id(), "user_interrupted");
}

static void handle_short_press(void)
{
    log_i("【butt】🔄🔄🔄 短按触摸键释放 - 中断音频播放");
    //  停止麦克风（确保状态清理）
    wss_mic_stop();
    
    // 状态由全局标志决定，不需要手动设置
}

//=================================================================================================
// 按键事件处理函数
//=================================================================================================
void wss_handle_key_event(void)
{
    log_mqtt("【butt】👆👆👆 电源键短按");
    if (g_exit) {
        g_power_key_retry = true;
    }
    // 移除多人模式切换逻辑
}

// 处理触摸按键事件（按下/释放）
static void wss_handle_touch_key_event(bool press_down)
{
#if TOUCH_KEY_AVAILABLE == 0
    return;
#endif
    os_mutex_pend(&g_wss_app_mutex, 0);
    if (press_down) {
        // ========== 触摸键按下事件 ==========
        log_i("【butt】👆👆 触摸键按下");
        send_btn_tap(BTN_TAP_TYPE_PRESS, 0);
        g_on_touch_key = true;
        g_touch_key_press_time = timer_get_ms();
        g_long_press_mic_started = false;  // 重置长按麦克风开启标志
        
        // 停止之前的定时器（如果存在）
        if (g_long_press_timer_id != 0) {
            sys_timeout_del(g_long_press_timer_id);
            g_long_press_timer_id = 0;
        }
        
        // 启动长按定时器，300ms后触发
        g_long_press_timer_id = sys_timeout_add_to_task("app_core", NULL, long_press_timeout_callback, LONG_PRESS_THRESHOLD_MS);
        
        // 1. 暂停音频播放
        wss_request_stop_audio();
        // 2. 停止麦克风
        wss_mic_stop();
        
    } else {
        // ========== 触摸键释放事件 ==========
        log_i("【butt】👇👇 触摸键释放");
        u32 duration = timer_get_ms() - g_touch_key_press_time;
        send_btn_tap(BTN_TAP_TYPE_RELEASE, duration);
        
        // 停止长按定时器（如果还在运行）
        if (g_long_press_timer_id != 0) {
            sys_timeout_del(g_long_press_timer_id);
            g_long_press_timer_id = 0;
        }
        
        g_on_touch_key = false;
        
        // 判断长按/短按（基于是否已经开启了麦克风）
        bool is_long_press = g_long_press_mic_started;
        g_long_press_mic_started = false;  // 重置长按麦克风开启标志
        
        if (is_long_press) {
            // ========== 长按结束事件 ==========
            handle_long_press_end();
        } else {
            // ========== 短按事件 ==========
            handle_short_press();
        }
    }
    os_mutex_post(&g_wss_app_mutex);
}

static void wss_handle_touch_key_press(void)
{
    wss_handle_touch_key_event(true);
    wss_application_vailed();
}

static void wss_handle_touch_key_release(void)
{
    wss_handle_touch_key_event(false);
    wss_application_vailed();
}

// 事件回调：仅记录事件到队列，避免在回调中调用加锁函数
static void input_event_handler_cb(lwl_event_t *e)
{
    input_event_t event = {0};
    
    switch (e->event_type) {
    case LWL_EVENT_TYPE_KEY:
        event.event_type = LWL_EVENT_TYPE_KEY;
        input_event_queue_push(&event);
        break;
        
    case LWL_EVENT_TYPE_TOUCH_KEY:
        event.event_type = LWL_EVENT_TYPE_TOUCH_KEY;
        event.data.touch_key_event.is_press_down = (e->touch_key_cmd.touch_key_event_type == 0);
        input_event_queue_push(&event);
        break;
        
    case LWL_EVENT_TYPE_LOW_BATTERY:
        int voltage = get_vbat_level();
        if (voltage < LOW_BATT_VERY_SHUTDOWN) {
            lwl_app_mode_switch_to(APP_DEFAULT_MODE);
        }
        break;
        
    default:
        break;
    }
}

//=================================================================================================
// 播放提示音相关
//=================================================================================================
static void wss_app_mode_play_cb(void *priv)
{
    log_i("[wss_application]wss_app_mode_play_cb turnoff cookie speaker");
}

void play_prompt(const char *fname) 
{
    lwl_app_mode_play(fname, wss_app_mode_play_cb);
    /* 停止蓝牙音频播放 */
    const char *stream_id = wss_request_get_stream_id();
    if (stream_id != NULL && stream_id[0] != '\0') {
        wss_ble_audio_stop(stream_id, false);
    } else {
        log_e("[wss_application]stream_id is empty, cannot stop audio");
    }

}

//=================================================================================================
// 休眠退出相关
//=================================================================================================
static pthread_t monitor_thread = NULL;
static bool monitor_thread_running = false;
// 模式5 加速度传感器相关变量

void *wss_condition_monitor_thread(void *arg)
{
    last_E_found_time = g_curr_time;
    last_move_time = g_curr_time;
    last_mem_stats_time = g_curr_time; // 新增变量

    log_i("[wss_application]Starting WSS condition monitor thread...");
    while (monitor_thread_running && lwl_app_mode_get_current_mode() == APP_WSS_CONNECTED_MODE) {
        g_curr_time = timer_get_ms();
        
        // 检查E设备
        if (get_5m_edev_count() > 0) {  // 检测到E设备
            last_E_found_time = g_curr_time;
        }

        // 检查设备移动状态
        if (is_device_moving()) {  // 设备在移动
            last_move_time = g_curr_time;
        }

        // 每60秒打印一次LwIP内存统计
        if (g_curr_time - last_mem_stats_time > 60000) {
            last_mem_stats_time = g_curr_time;
            
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

        g_curr_time = timer_get_ms();
        if (g_curr_time - last_req_time > NO_REQ_TIMEOUT_MS) {

            // 检查超时条件
            bool e_timeout = ((g_curr_time - last_E_found_time) > NO_E_TIMEOUT_MS);
                            
            bool move_timeout = ((g_curr_time - last_move_time) > NO_MOVE_TIMEOUT_MS);

            // 任一超时条件满足就切换模式
            if (e_timeout || move_timeout) {
                log_i("[wss_application]No E or movement timeout (e_to=%u, move_to=%u, last_req_delta=%u), switching to default mode.",
                      g_curr_time - last_E_found_time,
                      g_curr_time - last_move_time,
                      g_curr_time - last_req_time);
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
    player = lwl_app_core_context_get_instance()->music_player;

    odmp_register_disconnect_cbk(wss_disconnect_cbk);
    
    // monitor_start(); // 已在app_cookie.c中调用，无需重复调用
    wss_audio_init();
    wss_mic_init();
    wss_request_init();//发送1
    wss_device_init();//设备初始化，包含设备队列等
    nearby_update_handler_init(); // 初始化nearby_update事件处理器
    wss_mic_stop(); // 停止录音

    g_exit = false;
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
    
    /* 初始化完成后发送空闲状态 */
    send_device_status(DEVICE_STATUS_IDLE); // 在线
}

void wss_application_uninit()
{
    log_i("[wss_application]Uninit WSS application\n");

    /* 取消注册lwl事件处理回调 */
    lwl_event_handler_remove(input_event_handler_cb);

    os_mutex_pend(&g_wss_app_mutex, 0);
    
    // 停止长按定时器（如果还在运行）
    if (g_long_press_timer_id != 0) {
        sys_timeout_del(g_long_press_timer_id);
        g_long_press_timer_id = 0;
    }
    
    g_exit = false;
    
    /* 清空事件队列 */
    g_event_queue_head = 0;
    g_event_queue_tail = 0;

    wss_request_uninit();
    wss_audio_uninit();
    wss_mic_uninit();
    nearby_update_handler_uninit(); // 反初始化nearby_update事件处理器
    wss_device_uninit();
    monitor_stop();//free all pdev

    os_mutex_post(&g_wss_app_mutex);
}

void wss_application_reset()
{
    log_i("[wss_application]Reset WSS application\n");
    log_mqtt("🤖🤖🤖 重置WSS应用");
    os_mutex_pend(&g_wss_app_mutex, 0);
    g_sensor_reset_alive_timer();
    g_sensor_set_alive_timeout(NO_REQ_TIMEOUT_MS);

    
    wss_audio_init();
    wss_request_init();//发送1
    wss_device_init();//设备初始化，包含设备队列等
    nearby_update_handler_init(); // 初始化nearby_update事件处理器
    wss_mic_init();
    wss_mic_stop(); // 停止录音

    g_exit = false;
    
    /* 清空事件队列 */
    g_event_queue_head = 0;
    g_event_queue_tail = 0;

    /* 重置 */
    g_curr_time = timer_get_ms();
    last_req_time = g_curr_time - DEV_CALL_TIMEOUT_MS; // 初始化为3秒前，避免刚启动就超时
    os_mutex_post(&g_wss_app_mutex);
}

// 应用循环
bool wss_application_loop()
{
    g_curr_time = timer_get_ms();
    // 处理队列中的输入事件
    input_event_t event;
    while (input_event_queue_pop(&event)) {
        os_mutex_pend(&g_wss_app_mutex, 0);
        
        switch (event.event_type) {
        case LWL_EVENT_TYPE_KEY:
            wss_handle_key_event();
            break;
            
        case LWL_EVENT_TYPE_TOUCH_KEY:
            if (event.data.touch_key_event.is_press_down) {
                // 触摸键按下，
                wss_handle_touch_key_event(true);
                wss_application_vailed();
            } else {
                // 触摸键释放
                wss_handle_touch_key_event(false);
                wss_application_vailed();
            }
            break;
            
        default:
            break;
        }
        
        os_mutex_post(&g_wss_app_mutex);
    }

    if (wss_check_exit()) {
        if (g_exit == false) {
            wss_request_uninit();
            wss_audio_uninit();
            wss_mic_uninit();
            nearby_update_handler_uninit(); // 反初始化nearby_update事件处理器
            wss_device_uninit();
            g_exit = true;
        }
        return true;
    }
    
    if (lwl_app_mode_playing()) {
        return false;
    }

    return false;
}
