/**
 * @author luotianyu
 * @brief 
 */
#include "devices_check.h"
#include "mode_list.h"
#include "gpio.h"
#include "log.h"
#include "bt_ble/client/ble_dev_task_scheduler.h"
#include "../../bt_ble/client/ble_simple_client.h"
#include "../../include/app_config.h"
#include "../../protocol/cp/c_uart_p.h"
#include "../../protocol/odm/odm_secure.h"

#include "../../bt_ble/client/ble_distance.h"
#include <sys_time.h>
#include "key_event.h"
#include "cJSON_common/cJSON.h"
#include "bt_ble/app_wifi_manage.h"
#include "http/http_cli.h"
#include <stdlib.h> // For rand() and srand()

#include "os/os_api.h"
static OS_MUTEX g_mode_mutex;

#define MODE_TIMEOUT_MS 1200000 // 20 minutes
static u32 init_time = 0;

static void set_led_green(void)      { uart_send_set_led_color(COLOR_GREEN); }

static void switch_to_mode(work_mode_e new_mode);

// 全局变量
work_mode_e g_current_mode = WORK_MODE_INIT;
work_mode_e g_new_mode = WORK_MODE_INIT;
u8 g_audio_playing = 0;
u8 g_mode_switching = 0;      // 模式切换锁定标志，防止重复切换导致问题


lwl_eend_dev_t* pdev_list[MAX_PDEV_LIST] = {0};
int pdev_list_count = 0;

work_mode_e get_current_mode()
{
    return g_current_mode;
}



// 判断pdev是否已在列表中
static int is_pdev_in_list(lwl_eend_dev_t* pdev) {
    for (int i = 0; i < pdev_list_count; ++i) {
        if (pdev_list[i] == pdev) return 1;
    }
    return 0;
}

// 添加pdev到列表
static void add_pdev_to_list(lwl_eend_dev_t* pdev) {
    if (!pdev || is_pdev_in_list(pdev) || pdev_list_count >= MAX_PDEV_LIST) return;
    pdev_list[pdev_list_count++] = pdev;
    char sn_string[SN_SIZE] = "(NULL)";
    pdev->get_string(pdev, sn_string, sizeof(sn_string));
    log_i("[PDEV_LIST] Added: %s, total: %d", sn_string, pdev_list_count);
    
    // uint32_t cmd_id;
    // int ret = devices_check_e_turn_on(pdev, NULL, pdev, &cmd_id);
    // log_i("turn on cmd_id: %d", ret);
}

// 从列表中移除pdev
static void remove_pdev_from_list(lwl_eend_dev_t* pdev) {
    for (int i = 0; i < pdev_list_count; ++i) {
        if (pdev_list[i] == pdev) {
            for (int j = i; j < pdev_list_count - 1; ++j) {
                pdev_list[j] = pdev_list[j + 1];
            }
            pdev_list[--pdev_list_count] = NULL;
            char sn_string[SN_SIZE] = "(NULL)";
            pdev->get_string(pdev, sn_string, sizeof(sn_string));
            log_i("[PDEV_LIST] Removed: %s, total: %d", sn_string, pdev_list_count);
            break;
        }
    }
}

void devices_check_play_music(const char *fname) 
{
    lwl_app_context_t *context = lwl_app_core_context_get_instance();
    
    // 检查context有效性
    if (!context || !context->music_player) {
        log_e("devices_check_play_music: Context or music_player is NULL");
        return;
    }
    
    // 检查文件名有效性
    if (!fname) {
        log_e("devices_check_play_music: Invalid filename");
        return;
    }
    
    // g_music_player.play_voice_prompt = 1;
    log_i("play: %s", fname);
    int err = app_music_play_local_music_with_callback(context->music_player, fname, NULL, NULL);
    if (err) {
        log_e("devices_check_play_music: Failed to play %s, error: %d", fname, err);
        // key_event_enable();
        // g_music_player.play_voice_prompt = 0;
    }
}

//=================================================================================================
// LWL event callback
//=================================================================================================
static void devices_check_lwl_event_handler_cb(lwl_event_t *e)
{
    static int en = 1;
    lwl_eend_dev_t *p_eend_dev = e->distance_change.pdev;
    switch (e->event_type) {
    case LWL_EVENT_TYPE_EEND_DEV_CLOSEST_CHANGE: {
        log_i("LWL_EVENT_TYPE_EEND_DEV_CLOSEST_CHANGE %d %d", p_eend_dev, p_eend_dev);
        if (p_eend_dev != NULL) {
            char sn_string[SN_SIZE] = "(NULL)";
            p_eend_dev->get_string(p_eend_dev, sn_string, sizeof(sn_string));
            log_i("LWL_EVENT_TYPE_EEND_DEV_CLOSEST_CHANGE %s", sn_string);
        }
    }   break;
    case LWL_EVENT_TYPE_EEND_DEV_DISTANCE_CHANGE: {
        char sn_string[SN_SIZE * 2 + 1] = "(NULL)";
        p_eend_dev->get_string(p_eend_dev, sn_string, sizeof(sn_string));
        log_i("[MEASDIST] %s: %d %d\r\n", sn_string, p_eend_dev->distance_info.rssi, p_eend_dev->distance_info.distance_type);
        if (p_eend_dev->distance_info.distance_type == DISTANCE_0) {
            add_pdev_to_list(p_eend_dev); // 新增：维护pdev列表
        } else if (p_eend_dev->distance_info.distance_type == DISTANCE_2 || p_eend_dev->distance_info.distance_type == DISTANCE_3) {
            remove_pdev_from_list(p_eend_dev); // 新增：从列表中移除pdev
        }
        if (p_eend_dev->distance_info.rssi_sample_count == 0) {
            remove_pdev_from_list(p_eend_dev); // 新增：从列表中移除pdev
        }
    }   break;
    case LWL_EVENT_TYPE_EEND_DEV_NEW_IN:
        log_i("ORMET_NEW_DEVICE_IN");
        break;
    case LWL_EVENT_TYPE_UART_IO_KEY_SHORT_CLICK: {
        log_i("=== UART IO KEY SHORT CLICK DETECTED ===");
        log_i("Button pressed via UART interface");
        log_i("Event timestamp: %d ms", timer_get_ms());
        log_i("Current mode: %s (%d)", mode_names[g_current_mode], g_current_mode);
        
        // 按键切换模式逻辑
        work_mode_e next_mode;
        switch (g_current_mode) {
        case WORK_MODE_INIT:
            next_mode = WORK_MODE_6;
            break;
        case WORK_MODE_1:
            next_mode = WORK_MODE_2;
            break;
        case WORK_MODE_2:
            next_mode = WORK_MODE_3;
            break;
        case WORK_MODE_3:
            next_mode = WORK_MODE_4;
            break;
        case WORK_MODE_4:
            next_mode = WORK_MODE_5;
            break;
        case WORK_MODE_5:
            next_mode = WORK_MODE_6;
            break;
        case WORK_MODE_6:
            next_mode = WORK_MODE_1;
            break;
        case WORK_MODE_7:
            if (mode7_is_wifi_connected()) {
                next_mode = WORK_MODE_8;
            }
            break;
        case WORK_MODE_8:
            next_mode = WORK_MODE_9;
            break;
        case WORK_MODE_9:
        default:
            next_mode = WORK_MODE_INIT; // 模式8后回到初始模式，形成循环
            break;
        }
        os_mutex_pend(&g_mode_mutex, 0);
        g_new_mode = next_mode;
        os_mutex_post(&g_mode_mutex);

    }   break;
    case LWL_EVENT_TYPE_UART_CLEAR_WIFI_SSID:
        // return to WORK_MODE_1
        os_mutex_pend(&g_mode_mutex, 0);
        g_new_mode = WORK_MODE_INIT;
        os_mutex_post(&g_mode_mutex);
        break;
    case LWL_EVENT_TYPE_UART_SYNC:
        log_i("battery: %d", e->uart_cmd_sync.battery);
        log_i("SYNC response E.. v=3; timer_get_ms() - init_time:%d ;  battery: %d", timer_get_ms() - init_time, e->uart_cmd_sync.battery);
        log_i("SYNC response by devices_check_lwl_event_handler_cb (%d, 12)", timer_get_ms() - init_time < MODE_TIMEOUT_MS);
        uart_response_synch_status(timer_get_ms() - init_time < MODE_TIMEOUT_MS, MAX_LE_AUDIO_VOLUME);/// 超时后，返回idle
        break;
    case LWL_EVENT_TYPE_UART_TOUCH_KEY_DOWN:
        log_i("=== UART TOUCH KEY DOWN DETECTED ===");
        break;
        if (e->uart_cmd_touch.touch_key_event_type == 0) {
            log_i("Touch key pressed down");
            log_i("Event timestamp: %d ms", timer_get_ms());
            log_i("Current mode: %s (%d)", mode_names[g_current_mode], g_current_mode);
            
            log_i("Touch key event type: %d",e->uart_cmd_touch.touch_key_event_type);
            
            // 特殊处理模式8的触摸按键检测
            if (g_current_mode == WORK_MODE_8) {
                process_mode_8_touch_key(e->uart_cmd_touch.touch_key_event_type);
            }
            // 特殊处理模式1的触摸按键检测
            else if (g_current_mode == WORK_MODE_1) {
                process_mode_1_touch_key(e->uart_cmd_touch.touch_key_event_type);
            }
        } else {
            log_i("Touch key released");
            log_i("Event timestamp: %d ms", timer_get_ms());
            log_i("Current mode: %s (%d)", mode_names[g_current_mode], g_current_mode);
            
            log_i("Touch key event type: %d", e->uart_cmd_touch.touch_key_event_type);
            
            // 可以在这里添加触摸按键抬起的特殊处理逻辑
            if (g_current_mode == WORK_MODE_1) {
                process_mode_1_touch_key(1);
            } else {
                log_i("Touch key released in mode %s", mode_names[g_current_mode]);
            }
        }
        break;
    case LWL_EVENT_TYPE_UART_MOVE_MORE_THAN_ONE_METER:
        log_i("MOVE_MORE_THAN_ONE_METER");
        break;
    }
}
static void devices_check_init(void)
{
    init_time = timer_get_ms();
    lwl_app_context_t *g_app_ctx = lwl_app_core_context_get_instance();
    
    if (!os_mutex_valid(&g_mode_mutex)) {
        os_mutex_create(&g_mode_mutex);
    }
    os_mutex_pend(&g_mode_mutex, 0);
    
    msleep(5000);
    log_i("=== DEFAULT MODE INITIALIZED ===");
    log_i("Multi-mode system ready");
    log_i("Key detection enabled");
    log_i("UART event monitoring active");
    ble_simple_client_module_enable(0);
    /* UART指令开启C端le audio */
    uart_send_open_le_audio(g_app_ctx->mac);

    pdev_list_count = 0; // 清空设备列表
    /* 注册lwl事件处理回调 */
    lwl_event_handler_add(devices_check_lwl_event_handler_cb);

    // 使用当前时间作为随机种子
    srand(timer_get_ms());


    // 初始化模式系统，进入初始模式（红灯常亮）
    switch_to_mode(WORK_MODE_INIT);

    wifi_and_network_off();
    os_mutex_post(&g_mode_mutex);
    
    log_i("=== MODE SYSTEM INITIALIZED ===");
    log_i("Press key to switch modes:");
    log_i("INIT -> MODE_1 (Touch Key Detection) -> MODE_2 (All Colors Cycling) -> MODE_3 (Audio Loop) -> MODE_4 (Microphone VAD) -> MODE_5 (G-Sensor Test) -> INIT (Loop)");
    log_i("Available colors (16 total): Red*, Green*, Blue*, Purple*, Cyan, Yellow, Magenta, White, Orange, Pink, Dark_Yellow, Dark_Blue, Maroon, Brown, Gray, Black");
    log_i("Color basis: Red/Green/Blue/Purple are accurate (* = accurate), others adjusted");
    log_i("MODE 1: Green LED = Ready, Red LED = Touch detected (0.8s duration)");
    log_i("MODE 4: Red LED = Voice detected, Green LED = Silent");
    log_i("MODE 5: Green LED = Static, Blue LED = Small motion, Red LED = Large motion, Gyroscope data every 0.5s");
    log_i("Touch Key: MODE 1 = Red LED (0.8s), Other modes = Yellow LED (temporary)");
}

static void devices_check_unint(void)
{
    os_mutex_pend(&g_mode_mutex, 0);
    lwl_event_handler_remove(devices_check_lwl_event_handler_cb);
    ble_simple_client_module_enable(0);
    os_mutex_post(&g_mode_mutex);
    os_mutex_del(&g_mode_mutex, 1);
}

#define HEAT_TIMEOUT 1000
void app_devices_check_mode(void)
{
    devices_check_init();
    static u32 g_heat_timer;
    g_heat_timer = timer_get_ms();

    log_i("=== ENTERING MAIN LOOP ===");
    log_i("Multi-mode system active");

    while (1) {
        // 主循环延时
        mdelay(100);

        os_mutex_pend(&g_mode_mutex, 0);
        if (g_new_mode != g_current_mode) {
            switch_to_mode(g_new_mode);
        }
        
        // 执行当前模式的处理函数
        if (g_current_mode < WORK_MODE_MAX && mode_process_functions[g_current_mode]) {
            mode_process_functions[g_current_mode]();
        }
        
        // 检测按键状态（保留原有的GPIO按键检测功能）
        check_key_status();
        u32 g_curr_time = timer_get_ms();

        // 定期输出设备信息（减少频率以避免日志过多）
        if (g_curr_time - g_heat_timer >= HEAT_TIMEOUT * 3) { // 每3秒输出一次
            log_i("=== SYSTEM STATUS ===");
            log_i("Current mode: %s (%d)", mode_names[g_current_mode], g_current_mode);
            log_i("System uptime: %d ms", g_curr_time);
            log_i("System uptime: %d ms", lwl_rtc_timer_get_sec());
            dump_all_eend_info();
            g_heat_timer = g_curr_time;
        }
        os_mutex_post(&g_mode_mutex);

        if (lwl_app_mode_exitting()) {
            log_i("=== EXITING MULTI-MODE SYSTEM ===");
            devices_check_unint();
            return;
        }
    }
}

// 添加按键状态检测函数
static void check_key_status(void)
{
    static u32 last_key_check_time = 0;
    static u8 last_key_state = 0;
    
    u32 current_time = timer_get_ms();
    
    // 每100ms检查一次按键状态
    if (current_time - last_key_check_time >= 100) {
        last_key_check_time = current_time;
    }
}

// 初始模式处理函数
void process_mode_init(void)
{
}

// 初始模式处理函数
static void mode_0_init(void)
{   
    set_led_green();
    log_i("INIT MODE: Green LED ON, C speaker enabled");
    // 播放进入模式0的音频
    devices_check_play_music(CONFIG_CP_VOICE_FILE_PATH"test_start.mp3");
    log_i("Playing test_start.mp3 for INIT mode");
}

// 模式切换函数
static void switch_to_mode(work_mode_e new_mode)
{
    // 防止模式切换时的重复调用
    if (g_mode_switching) {
        log_i("Mode switching in progress, ignoring new request");
        return;
    }
    // 设置切换锁定
    g_mode_switching = 1;
    os_mutex_pend(&g_mode_mutex, 0);
    
    
    if (new_mode >= WORK_MODE_MAX) {
        log_i("Invalid mode: %d, staying in current mode", new_mode);
        os_mutex_post(&g_mode_mutex);
        return;
    }
    
    log_i("=== MODE SWITCH ===");
    log_i("From: %s (%d) -> To: %s (%d)", 
          mode_names[g_current_mode], g_current_mode,
          mode_names[new_mode], new_mode);
    

    // 模式切换时的初始化操作
    switch (g_current_mode) {
    case WORK_MODE_INIT:
        break;
    case WORK_MODE_1:
        mode1_uninit();
        break;
    case WORK_MODE_2:
        break;
    case WORK_MODE_3:
        mode3_uninit();
        break;
    case WORK_MODE_4:
        mode4_uninit();
        break;
    case WORK_MODE_5:
        mode5_uninit();
        break;
    case WORK_MODE_6:
        mode6_uninit();
        break;
    case WORK_MODE_7:
        mode7_uninit();
        break;
    case WORK_MODE_8:
        mode8_uninit();
        break;
    case WORK_MODE_9:
        mode9_uninit();
        break;
    default:
        break;
    }
    
    g_current_mode = new_mode;
    
    set_led_green();
    gpio_direction_output(IO_PORTA_00, 1);  // 启用C端扬声器
    // 模式切换时的初始化操作
    switch (new_mode) {
    case WORK_MODE_INIT:
        mode_0_init();
        break;
    case WORK_MODE_1:
        mode1_init();
        break;
    case WORK_MODE_2:
        mode2_init();
        break;
    case WORK_MODE_3:
        mode3_init();
        break;
    case WORK_MODE_4:
        mode4_init();
        break;
    case WORK_MODE_5:
        mode5_init();
        break;
    case WORK_MODE_6:
        mode6_init();
        break;
    case WORK_MODE_7:
        mode7_init();
        break;
    case WORK_MODE_8:
        mode8_init();
        break;
    case WORK_MODE_9:
        mode9_init();
        break;
    default:
        break;
    }
    
    // 解除切换锁定
    g_mode_switching = 0;
    os_mutex_post(&g_mode_mutex);
    log_i("=== MODE SWITCH COMPLETED ===");
}