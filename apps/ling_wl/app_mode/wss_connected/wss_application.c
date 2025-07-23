#include "wss_application.h"
#include "wss_audio.h"
#include "wss_app_util.h"
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
#include "protocol/odm/odm_util.h"

#include "protocol/odm/odm_secure.h"
#include "protocol/cp/c_uart_p.h"

static u32 g_curr_time = 0;
#define HEAT_TIMEOUT 5000
static u32 g_heat_timer = 0;

static int g_exception_exit = false;

static lwl_app_context_t *g_app_ctx = NULL;
static int g_audio_playing = false;


static void set_led_red_low_power(void)        { uart_send_set_led_color(COLOR_RED); } //red~ low power


/* Task state */
typedef enum {
    DEVICE_IDLE,
    DEVICE_AUDIO,
} AppTaskStepState;

static AppTaskStepState g_device_state = DEVICE_IDLE;

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

static void wss_application_print_event(int msg_event, char* tag)
{
    switch (msg_event) {
    case ORMET_INITIAL_SETUP:
        log_i("[%s]ORMET_INITIAL_SETUP", tag);
        break;
    case ORMET_NEAR_TRIGGER:
        log_i("[%s]ORMET_NEAR_TRIGGER", tag);
        break;
    case ORMET_NEW_DEVICE_IN:
        log_i("[%s]ORMET_NEW_DEVICE_IN", tag);
        break;
    case ORMET_SWITCH_SUCCESS:
        log_i("[%s]ORMET_SWITCH_SUCCESS", tag);
        break;
    case ORMET_SWITCH_FAILED:
        log_i("[%s]ORMET_SWITCH_FAILED", tag);
        break;
    case ORMET_BUTTON_TRIGGER:
        log_i("[%s]ORMET_BUTTON_TRIGGER", tag);
        break;
    case ORMET_BUTTON_PRESS:
        log_i("[%s]ORMET_BUTTON_PRESS", tag);
        break;
    case ORMET_BUTTON_DOUBLE_PRESS:
        log_i("[%s]ORMET_BUTTON_DOUBLE_PRESS", tag);
        break;
    case ORMET_BUTTON_START:
        log_i("[%s]ORMET_BUTTON_START", tag);
        break;
    case ORMET_BUTTON_END:
        log_i("[%s]ORMET_BUTTON_END", tag);
        break;
    case ORMET_MOVE_PATROL:
        log_i("[%s]ORMET_MOVE_PATROL", tag);
        break;
    case ORMET_MOVE_STOP:
        log_i("[%s]ORMET_MOVE_STOP", tag);
        break;
    case ORMET_MOVE_SHAKE:
        log_i("[%s]ORMET_MOVE_SHAKE", tag);
        break;
    case ORMET_VAD_SPEAKER_START:
        log_i("[%s]ORMET_VAD_SPEAKER_START", tag);
        break;
    case ORMET_VAD_SPEAKER_END:
        log_i("[%s]ORMET_VAD_SPEAKER_END", tag);
        break;
    case ORMET_PLAY_START:
        log_i("[%s]ORMET_PLAY_START", tag);
        break;
    case ORMET_PLAY_NEAR_END:
        log_i("[%s]ORMET_PLAY_NEAR_END", tag);
        break;
    case ORMET_PLAY_END:
        log_i("[%s]ORMET_PLAY_END", tag);
        break;
    }
}

static bool is_edev_far_away(lwl_eend_dev_t *pdev)
{
    return (pdev->distance_info.distance_type == DISTANCE_2) || (pdev->distance_info.distance_type == DISTANCE_3);
}

static void wss_application_exception_exit()
{
    g_exception_exit = true;
}

static bool wss_application_check_exit()
{
    return g_exception_exit;
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
// MUSIC response callback
//=================================================================================================
void app_music_play_end_cb(void *priv)
{
    g_audio_playing = false;
}

//=================================================================================================
// pDev response callback
//=================================================================================================
static void e_turn_on_cb(bka_cmd_code_e code, void *priv)
{
    lwl_eend_dev_t *pdev = (lwl_eend_dev_t*)(priv);
    
    char sn_string[SN_SIZE * 2 + 1] = "(NULL)";
    if (pdev) {
        pdev->get_string(pdev, sn_string, sizeof(sn_string));
    }
    switch(code) {
    case BKA_CMD_CODE_COMPL:
        log_i("Switch PortE %s OK\n", sn_string);
        break;
    case BKA_CMD_CODE_DEVLOST:
        log_i("Switch PortE Dev Lost\n");
        break;
    case BKA_CMD_CODE_BUFFER_FULL:
        log_i("Switch PortE BLE TX Buffer Full\n");
        break;
    case BKA_CMD_CODE_SEARCH_PROFILE_TIMEOUT:
        log_i("Switch PoertE search GATT timeout\n");
        break;
    case BKA_CMD_CODE_E_TURN_ON_TIMEOUT:
        log_i("Switch PortE Timeout\n");
        break;
    case BKA_CMD_CODE_E_TURN_ON_ALREADY_BOUND:
        log_i("Switch PortE Failed: E is already bound\n");
        break;
    case BKA_CMD_CODE_UNEXPECTED:
        log_i("Switch PortE Error\n");
        break;
    }
    if (code != BKA_CMD_CODE_COMPL) {
    }
}

//=================================================================================================
// LWL event callback
//=================================================================================================
static void wss_app_lwl_event_handler_cb(lwl_event_t *e)
{
    static int en = 1;
    lwl_eend_dev_t *p_eend_dev = e->distance_change.pdev;

    switch (e->event_type) {
    case LWL_EVENT_TYPE_EEND_DEV_CLOSEST_CHANGE: {
    }
    // break;
    case LWL_EVENT_TYPE_EEND_DEV_DISTANCE_CHANGE: {
        char sn_string[SN_SIZE * 2 + 1] = "(NULL)";
        p_eend_dev->get_string(p_eend_dev, sn_string, sizeof(sn_string));
        log_i("[MEASDIST] %s: %d %d\r\n", sn_string, p_eend_dev->distance_info.rssi, p_eend_dev->distance_info.distance_type);

        switch (p_eend_dev->distance_info.distance_type) {
        case DISTANCE_0:
            log_d("[MEASDIST] distance < 2m : do things1 here --- %s\r\n", sn_string);
            break;
        case DISTANCE_1:
            log_d("[MEASDIST] distance > 2m  < 5m do things2 here ---%s\r\n", sn_string);
            break;
        case DISTANCE_2:
            log_d("[MEASDIST] distance > 5m do things3 here ---%s\r\n", sn_string);
            break;
        case DISTANCE_3:
            log_d("[MEASDIST] not found ---%s\r\n", sn_string);
            break;
        }
    }   break;
    case LWL_EVENT_TYPE_EEND_DEV_NEW_IN:
        log_i("ORMET_NEW_DEVICE_IN");
        break;
    case LWL_EVENT_TYPE_UART_IO_KEY_SHORT_CLICK: {
        log_i("SHORT_CLICK");
        lwl_app_mode_switch_to(APP_DEFAULT_MODE);
    }   break;
    case LWL_EVENT_TYPE_UART_SYNC:
        log_i("SYNC");
        log_i("battery: %d", e->uart_cmd_sync.battery);
        if(e->uart_cmd_sync.battery < 30 )
            set_led_red_low_power();
        uart_response_synch_status(1, 12);/// 音量 E only
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


void wss_application_init()
{
    log_i("Init WSS application\n");
    g_app_ctx = lwl_app_core_context_get_instance();
    
    wss_audio_init();

    ble_simple_server_module_enable(0);
    ble_simple_client_module_enable(1);

    /* 注册lwl事件处理回调 */
    lwl_event_handler_add(wss_app_lwl_event_handler_cb);

    /* 重置 */
    g_exception_exit = false;
    g_audio_playing = false;
}

void wss_application_uninit()
{
    log_i("Uninit WSS application\n");

    wss_audio_uninit();

    /* 取消注册lwl事件处理回调 */
    lwl_event_handler_remove(wss_app_lwl_event_handler_cb);
}

bool wss_application_loop()
{
    g_curr_time = timer_get_ms();
    if (wss_application_check_exit()) {
        g_exception_exit = false; // 清除退出标志
        return true;
    }

    // 状态机处理
    switch (g_device_state) {
    case DEVICE_IDLE:
        // 在IDLE状态下，检测是否有进入AUDIO的条件（可根据实际业务补充）
        // 这里只做示例，假设有某事件触发进入AUDIO
        break;

    case DEVICE_AUDIO:
        // AUDIO状态下，如果收到触摸按键事件则打断，回到IDLE
        if (wss_application_check_touch_key_exit()) {
            g_device_state = DEVICE_IDLE;
            log_i("STATE: DEVICE_AUDIO -> DEVICE_IDLE (touch key down)");
            g_touch_key_exit = false; // 清除标志
        }
        // 其它AUDIO状态处理...
        break;
    }

    msleep(10);

    if (g_curr_time - g_heat_timer >= HEAT_TIMEOUT) {
        dump_all_eend_info();
        g_heat_timer = g_curr_time;
    }

    return false;
}