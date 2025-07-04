#include "wss_application.h"
#include "wss_app_util.h"
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
#include "net_music_play_my.h"

#include "protocol/odm/odmp.h"
#include "protocol/odm/odm_util.h"

#include "protocol/odm/odm_secure.h"

#define ENABLE 1
#define DISABLE 0


static u32 g_curr_time = 0;
#define HEAT_TIMEOUT 5000
static u32 g_heat_timer = 0;
#define WAIT_AUDIO_RESP_TIMEOUT 5000
static u32 g_wait_audio_resp_timer = 0;

static int g_exception_exit = false;
static lwl_app_context_t *g_app_ctx = NULL;

static lwl_eend_dev_t *g_edev_closest = NULL; // C 本地维护
static lwl_eend_dev_t *g_edev_last = NULL; // C 本地维护
static lwl_eend_dev_t *g_edev_target = NULL; // 服务器维护
static lwl_eend_dev_t *g_edev_connected = NULL; // E 已连接


static int g_audio_playing = false;
// static int g_audio_ready = false;
// static int g_audio_play_ok = false;
static int g_has_tts_bind_req = false;

static void wss_application_exception_exit()
{
    g_exception_exit = true;
}

static bool wss_application_check_exit()
{
    return g_exception_exit;
}

//=================================================================================================
// MUSIC response callback
//=================================================================================================
void app_music_play_end_cb(void *priv)
{
    log_i("app_music_play_end_cb End");
    lwl_eend_dev_t *pdev = (lwl_eend_dev_t*)(priv);
    
    char sn_string[SN_SIZE] = "(NULL)";
    if (pdev) {
        pdev->get_string(pdev, sn_string, sizeof(sn_string));
        pdev->handshake_info.need_handshake = false;
    }
    log_i("bka_e_turn_off %s\n", sn_string);
    g_audio_playing = false;
    g_edev_target = NULL;
}

static void simple_play_prompt(const char *fname, void *priv)
{
    // g_music_player.play_voice_prompt = 1;
    g_audio_playing = true;
    log_i("play: %s", fname);
    int err = app_music_play_local_music_with_callback(g_app_ctx->music_player, fname, app_music_play_end_cb, priv);
    if (err) {
        // key_event_enable();
        // g_music_player.play_voice_prompt = 0;
    }
}
#define CONFIG_VOICE_PROMPT_FILE_PATH       "mnt/sdfile/res/audlogo/"

//=================================================================================================
// pDev response callback
//=================================================================================================
static void e_turn_on_cb(bka_cmd_code_e code, void *priv)
{
    lwl_eend_dev_t *pdev = (lwl_eend_dev_t*)(priv);
    
    char sn_string[SN_SIZE] = "(NULL)";
    if (pdev) {
        pdev->get_string(pdev, sn_string, sizeof(sn_string));
    }
    log_i("bka_e_turn_on code %d %s\n", code, sn_string);
    switch(code) {
    case BKA_CMD_CODE_COMPL:

        log_i("Switch PortE %s OK\n", sn_string);
        g_edev_connected = pdev;
        if (pdev == g_edev_target) simple_play_prompt(CONFIG_VOICE_PROMPT_FILE_PATH"BindSucc.mp3", pdev);
        g_has_tts_bind_req = false;
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
        g_edev_closest = p_eend_dev;
    }
    // break;
    case LWL_EVENT_TYPE_EEND_DEV_DISTANCE_CHANGE: {
        char sn_string[SN_SIZE * 2 + 1] = "(NULL)";
        p_eend_dev->get_string(p_eend_dev, sn_string, sizeof(sn_string));
        log_i("[MEASDIST] %s: %d %d\r\n", sn_string, p_eend_dev->distance_info.rssi, p_eend_dev->distance_info.distance_type);

        switch (p_eend_dev->distance_info.distance_type) {
        case DISTANCE_0:
            log_i("[MEASDIST] distance < 2m : do things1 here --- %s\r\n", sn_string);
            if (g_edev_closest != p_eend_dev || p_eend_dev == g_edev_target) break;
            if (g_audio_playing == false && g_has_tts_bind_req == false) {
                g_edev_target = p_eend_dev;
                uint cmd_id;
                gpio_direction_output(IO_PORTA_00, 0);  //disable    C speaker
                g_edev_target->handshake_info.need_handshake = true;
                bka_e_turn_on(g_edev_target, e_turn_on_cb, g_edev_target, &cmd_id);
                g_has_tts_bind_req = true;
            }
            break;
        case DISTANCE_1:
            log_d("[MEASDIST] distance > 2m  < 5m do things2 here ---%s\r\n", sn_string);
            break;
        case DISTANCE_2:
            log_d("[MEASDIST] distance > 5m do things3 here ---%s\r\n", sn_string);

            if (p_eend_dev == g_edev_last) {
                if (g_audio_playing) {
                    // TODO STOP talking
                }
                g_edev_last = NULL;
            } 
            break;
        case DISTANCE_3:
            log_d("[MEASDIST] not found ---%s\r\n", sn_string);
            
            if (p_eend_dev == g_edev_last) {
                g_edev_last = NULL;
            }
            break;
        }
    }   break;
    case LWL_EVENT_TYPE_EEND_DEV_NEW_IN:
        log_i("ORMET_NEW_DEVICE_IN");
        break;
    case LWL_EVENT_TYPE_UART_IO_KEY_SHORT_CLICK: {
        log_i("SHORT_CLICK");

    }   break;
    case LWL_EVENT_TYPE_UART_SYNC:
        log_i("SYNC");
        log_i("battery: %d", e->uart_cmd_sync.battery);
        if(e->uart_cmd_sync.battery < 30 )
            uart_send_set_led_color(6000, 10000, 10000); //red~ low power
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

    /* UART指令开启C端le audio */
    uart_send_open_le_audio(g_app_ctx->mac);

    /* 注册lwl事件处理回调 */
    lwl_event_handler_add(wss_app_lwl_event_handler_cb);

    /* 重置 */
    g_edev_last = NULL; 
    g_edev_target = NULL; 
}

void wss_application_uninit()
{
    log_i("Uninit WSS application\n");

    /* 取消注册lwl事件处理回调 */
    lwl_event_handler_remove(wss_app_lwl_event_handler_cb);
    /* 取消注册loswssp事件回调 */
    loswssp_register_rsp_cbk(NULL);
    /* 设置VAD回调 */
    recorder_set_vad_cbk(g_app_ctx->recorder, NULL, NULL);
}

bool wss_application_loop(struct websocket_struct *websockets_info)
{
    g_curr_time = timer_get_ms();
    ASSERT(websockets_info != NULL);

    msleep(10);

    if (g_curr_time - g_heat_timer >= HEAT_TIMEOUT) {
        dump_all_eend_info();
        g_heat_timer = g_curr_time;
    }

    if (wss_application_check_exit()) {
        return true;
    }
    return false;
}