#include "wss_application.h"
#include "wss_audio.h"
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

#include "protocol/odm/odmp.h"
#include "protocol/odm/odm_util.h"

#include "protocol/odm/odm_secure.h"

static void wss_application_event_to_req(lwl_eend_dev_t *p_dev, int msg_event);

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


/*=================================================================================================
 * Audio dec app context
 *=================================================================================================*/
static int g_audio_play_end = false;
int vad_timeout = 0;


/* Task state */
typedef enum {
    STATE_IDLE,
    STATE_ONLINE,

    STATE_PLAYINTRO_SWITCH,
    STATE_PLAYINTRO_PLAYING,
    STATE_PLAYINTRO_VAD,

    STATE_ONEONONE_PLAYING,
    STATE_ONEONONE_SWITCH,
    STATE_ONEONONE_VAD,

    STATE_COMMAND_BUTTON,
    STATE_COMMAND_WAITING,

} AppTaskStepState;

static int g_mode_state = STATE_IDLE;

#define STEP_TO_NEXT_STEP(cur,next) if (g_mode_state == cur) {g_mode_state = next;wss_application_print_state(g_mode_state, "STEP_TO");}

static int g_audio_playing = false;
static int g_audio_ready = false;
static int g_audio_play_ok = false;
static int g_has_tts_bind_req = false;

static void wss_application_print_state(int mode_state, char* tag)
{
    switch (mode_state) {
    case STATE_IDLE:
        log_i("[%s]STATE_IDLE", tag);
        break;
    case STATE_ONLINE:
        log_i("[%s]STATE_ONLINE", tag);
        break;
    case STATE_PLAYINTRO_SWITCH:
        log_i("[%s]STATE_PLAYINTRO_SWITCH", tag);
        break;
    case STATE_PLAYINTRO_PLAYING:
        log_i("[%s]STATE_PLAYINTRO_PLAYING", tag);
        break;
    case STATE_PLAYINTRO_VAD:
        log_i("[%s]STATE_PLAYINTRO_VAD", tag);
        break;
    case STATE_ONEONONE_SWITCH:
        log_i("[%s]STATE_ONEONONE_SWITCH", tag);
        break;
    case STATE_ONEONONE_PLAYING:
        log_i("[%s]STATE_ONEONONE_PLAYING", tag);
        break;
    case STATE_ONEONONE_VAD:
        log_i("[%s]STATE_ONEONONE_VAD", tag);
        break;
    case STATE_COMMAND_BUTTON:
        log_i("[%s]STATE_COMMAND_BUTTON", tag);
        break;
    case STATE_COMMAND_WAITING:
        log_i("[%s]STATE_COMMAND_WAITING", tag);
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

/*=================================================================================================
 * E switch context
 *=================================================================================================*/
static int g_backend_switch_session_ok = false;
static lwl_eend_dev_t *g_curr_edev = NULL;
static lwl_eend_dev_t *g_next_edev = NULL;


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



static void reply_tts_bind(lwl_eend_dev_t *p_dev)
{
    if (p_dev == NULL) {
        wss_application_event_to_req(p_dev, ORMET_SWITCH_FAILED);
        return;
    } else {
        if (g_has_tts_bind_req) {
            wss_application_event_to_req(p_dev, ORMET_SWITCH_SUCCESS);
        }
    }
}


//=================================================================================================
// MUSIC response callback
//=================================================================================================
void app_music_play_end_cb(void *priv)
{
    g_audio_playing = false;
    if (g_audio_play_end) {
        log_i("g_audio_playing End");
        // wss_application_event_to_req(NULL, ORMET_PLAY_END);
    } else {
        log_i("g_audio_play_end not end, continue to play");
    }
    wss_application_event_to_req(NULL, ORMET_PLAY_END);
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
        g_edev_connected = g_edev_target;
        if (pdev == g_edev_target) reply_tts_bind(pdev);
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
        wss_application_event_to_req(pdev, ORMET_SWITCH_FAILED);
    }
}

//=================================================================================================
// WSS response callback
//=================================================================================================
static int odmp_rsp_cbk(int type, odmp_package_t *pkg)
{
    switch (type) {
    case OR_INITIAL_SETUP_RESPONSE:
        log_i("INITIAL_SETUP_RESPONSE\n");
        log_i("setup_complete: %d\n", pkg->initial_setup_response.setup_complete);
        // STEP_TO_NEXT_STEP(STATE_IDLE,                       STATE_ONLINE);
        break;
    case OR_DEVICE_SWITCH_RESPONSE:
        log_i("DEVICE_SWITCH_RESPONSE\n");
        log_i("device_id: %s\n", pkg->device_switch_response.device_id);
        log_i("status: %s\n", pkg->device_switch_response.status);
        break;
    case OR_TTS_BIND: {
        log_i("TTS_BIND\n");
        log_i("c_id: %s\n", pkg->tts_bind.c_id);
        log_i("send_id: %s\n", pkg->tts_bind.send_id);
        log_i("wait_time: %d\n", pkg->tts_bind.wait_time);

        // send_id 和E端序列号的关系?
        // 1. 把 send_id 默认为E端的序列号
        // 2. 把 send_id 映射为E端的序列号

        char *sn = pkg->tts_bind.send_id;
        // if (!sn) break;

        lwl_eend_dev_t *pdev = find_eend_dev_by_sn(sn);
        pdev->handshake_info.need_handshake = true;
        int state = g_mode_state;

        if (pdev == NULL) {
            log_e("dev not exists: %s", sn);
        }else {
            uint32_t cmd_id;
            g_edev_target = pdev;
            g_has_tts_bind_req = true;
            // STEP_TO_NEXT_STEP(STATE_ONLINE,                 STATE_ONEONONE_SWITCH);// Turn to One on one mode when use C
            STEP_TO_NEXT_STEP(STATE_ONLINE,                 STATE_PLAYINTRO_SWITCH);
            STEP_TO_NEXT_STEP(STATE_PLAYINTRO_VAD,          STATE_ONEONONE_SWITCH);
            STEP_TO_NEXT_STEP(STATE_ONEONONE_VAD,           STATE_ONEONONE_SWITCH);
            bka_e_turn_on(pdev, e_turn_on_cb, pdev, &cmd_id);
            // log_d("turn on cmd_id: %d", cmd_id);
        }

        if (state == STATE_ONLINE) {
            // reply_tts_bind(pdev);
        } else
        if (pkg->tts_bind.wait_time > 0) {
            vad_timeout = pkg->tts_bind.wait_time;
            g_has_tts_bind_req = true;
            update_time();
        } else {
            g_edev_last = pdev;
            vad_timeout = 0;
            // reply_tts_bind(pdev);
        }
    }   break;
    case OR_AUDIO_STATUS:
        log_i("AUDIO_STATUS\n");
        log_i("c_id: %s\n", pkg->audio_status.c_id);
        log_i("type: %s\n", odmp_ast_to_text(pkg->audio_status.type));
        log_i("event: %s\n", odmp_ase_to_text(pkg->audio_status.event));
        log_i("format: %s\n", pkg->audio_status.audio_param.format);
        log_i("sample_rate: %d\n", pkg->audio_status.audio_param.sample_rate);
        log_i("channels: %d\n", pkg->audio_status.audio_param.channels);
        log_i("frame_duration: %d\n", pkg->audio_status.audio_param.frame_duration);
        
        if (pkg->audio_status.type == OAST_ASR) {
            if (pkg->audio_status.event == OASE_ABNORMAL) {
                log_i("AUDIO_STATUS: ASR OASE_ABNORMAL\n");
            }
        } else if (pkg->audio_status.type == OAST_TTS) {
            if (pkg->audio_status.event == OASE_START && !g_audio_playing) {
                log_i("AUDIO_STATUS: OASE_START\n");
                g_audio_play_end = false;
                STEP_TO_NEXT_STEP(STATE_PLAYINTRO_SWITCH,   STATE_PLAYINTRO_PLAYING);
                STEP_TO_NEXT_STEP(STATE_ONEONONE_SWITCH,    STATE_ONEONONE_PLAYING);
                g_audio_playing = true;
                app_music_play_stream_music_with_callback(g_app_ctx->music_player, app_music_play_end_cb, NULL);
            } else if (pkg->audio_status.event == OASE_END) {
                log_i("AUDIO_STATUS: OASE_END %d\n", g_audio_playing);
                g_audio_play_end = true;
                if (!g_audio_playing) {
                    wss_application_event_to_req(NULL, ORMET_PLAY_END);
                }
            } else if (pkg->audio_status.event == OASE_ABNORMAL) {
                if (g_audio_playing) {
                    g_audio_play_end = true;
                    wss_application_event_to_req(NULL, ORMET_PLAY_END);
                    // TODO STOP talking
                }
            } else if (pkg->audio_status.event == OASE_SENTENCE_START) {
                g_audio_play_end = false;
                // if (!g_audio_playing) {
                //     log_i("AUDIO_STATUS: OASE_SENTENCE_START replay\n");
                //     g_audio_playing = true;
                //     app_music_play_stream_music_with_callback(g_app_ctx->music_player, app_music_play_end_cb, NULL);
                // }
            } else if (pkg->audio_status.event == OASE_SENTENCE_END) {
                g_audio_play_end = true;
                // if (!g_audio_playing) {
                //     log_i("AUDIO_STATUS: OASE_SENTENCE_END replay\n");
                //     g_audio_playing = true;
                //     app_music_play_stream_music_with_callback(g_app_ctx->music_player, app_music_play_end_cb, NULL);
                // }
            }
        }
        break;
    case OR_MESSAGE_TEXT:
        if (pkg->message_text.type == 1)
            log_i("ASR_TEXT %d %s\n", pkg->message_text.text, pkg->message_text.text);
        if (pkg->message_text.type == 2)
            log_i("TTS_TEXT %d %s\n", pkg->message_text.c_id, pkg->message_text.text);
        break;
    case OR_EXECUTION_TIME:
        log_i("EXECUTION_TIME %s %d\n", pkg->execution_time.event, pkg->execution_time.execution_time);
        break;
    case OR_PONG:
        log_i("LR_PONG\n");
        break;
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
        if (vad_timeout == 0) {
            g_edev_closest = p_eend_dev;
        }
    }
    // break;
    case LWL_EVENT_TYPE_EEND_DEV_DISTANCE_CHANGE: {
        char sn_string[SN_SIZE * 2 + 1] = "(NULL)";
        p_eend_dev->get_string(p_eend_dev, sn_string, sizeof(sn_string));
        log_i("[MEASDIST] %s: %d %d\r\n", sn_string, p_eend_dev->distance_info.rssi, p_eend_dev->distance_info.distance_type);

        switch (p_eend_dev->distance_info.distance_type) {
        case DISTANCE_0:
            log_d("[MEASDIST] distance < 2m : do things1 here --- %s\r\n", sn_string);

            p_eend_dev->handshake_info.need_handshake = true;
            wss_application_event_to_req(p_eend_dev, ORMET_NEAR_TRIGGER);
            break;
        case DISTANCE_1:
            log_d("[MEASDIST] distance > 2m  < 5m do things2 here ---%s\r\n", sn_string);
            break;
        case DISTANCE_2:
            log_d("[MEASDIST] distance > 5m do things3 here ---%s\r\n", sn_string);

            p_eend_dev->handshake_info.need_handshake = false;
            if (p_eend_dev == g_edev_last) {
                if (g_audio_playing) {
                    // TODO STOP talking
                }
                wss_application_event_to_req(p_eend_dev, ORMET_DEVICE_LEAVE);
                g_edev_last = NULL;
            } 
            break;
        case DISTANCE_3:
            log_d("[MEASDIST] not found ---%s\r\n", sn_string);
            
            p_eend_dev->handshake_info.need_handshake = false;
            if (p_eend_dev == g_edev_last) {
                wss_application_event_to_req(p_eend_dev, ORMET_DEVICE_LEAVE);
                g_edev_last = NULL;
            }
            break;
        }
    }   break;
    case LWL_EVENT_TYPE_EEND_DEV_NEW_IN:
        log_i("ORMET_NEW_DEVICE_IN");
        wss_application_event_to_req(p_eend_dev, ORMET_NEW_DEVICE_IN);
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
    
    wss_audio_init();

    /* UART指令开启C端le audio */
    uart_send_open_le_audio(g_app_ctx->mac);

    /* 注册lwl事件处理回调 */
    lwl_event_handler_add(wss_app_lwl_event_handler_cb);
    /* 注册odmp事件回调 */
    odmp_register_rsp_cbk(odmp_rsp_cbk);

    /* 重置 */
    g_exception_exit = false;
    g_edev_last = NULL; 
    g_has_tts_bind_req = false;
    g_audio_playing = false;

    g_mode_state = STATE_IDLE;
    wss_application_event_to_req(NULL, ORMET_INITIAL_SETUP);
}

void wss_application_uninit()
{
    log_i("Uninit WSS application\n");

    wss_audio_uninit();

    /* 取消注册lwl事件处理回调 */
    lwl_event_handler_remove(NULL);
    /* 取消注册loswssp事件回调 */
    loswssp_register_rsp_cbk(NULL);
}

bool wss_application_loop(struct websocket_struct *websockets_info)
{
    g_curr_time = timer_get_ms();
    ///ASSERT(websockets_info != NULL);

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

static void wss_application_event_to_req(lwl_eend_dev_t *p_dev, int msg_event)
{
    wss_application_print_event(msg_event, "wss_application_event_to_req");
    switch (g_mode_state) {
    case STATE_IDLE:
        if (msg_event != ORMET_INITIAL_SETUP &&
            msg_event != ORMET_NEAR_TRIGGER) return;
            STEP_TO_NEXT_STEP(STATE_IDLE,                   STATE_ONLINE);// TODO
            // gpio_direction_output(IO_PORTA_00, 1);  //enable C speaker
            // gpio_direction_output(IO_PORTB_02, 0);  // disable E speaker
        break;
    case STATE_ONLINE:
        if (msg_event != ORMET_BUTTON_TRIGGER &&
            msg_event != ORMET_NEAR_TRIGGER) return;
        if (g_edev_closest == NULL) return;
        if (p_dev == g_edev_closest && g_edev_last != p_dev) {
            g_edev_last = p_dev;
        } else return;
        break;
    case STATE_PLAYINTRO_SWITCH:
        if (msg_event != ORMET_BUTTON_TRIGGER &&
            msg_event != ORMET_SWITCH_SUCCESS &&
            msg_event != ORMET_SWITCH_FAILED) return;
        if (msg_event == ORMET_SWITCH_FAILED) {
            if (p_dev == g_edev_target) g_edev_target = NULL;
            if (p_dev == g_edev_last  ) g_edev_last   = NULL;
            STEP_TO_NEXT_STEP(STATE_PLAYINTRO_SWITCH,       STATE_ONLINE);
        } 
        if (msg_event == ORMET_SWITCH_SUCCESS) {
            g_edev_last = p_dev;
            log_i("Play intro switch success, E sn: %s", p_dev->sn);
            gpio_direction_output(IO_PORTA_00, 0);  //Disable C speaker
            gpio_direction_output(IO_PORTB_02, 1);
        }
            
        // if (!g_edev_target == p_dev) return;
        break;
    case STATE_PLAYINTRO_PLAYING:
        if (msg_event != ORMET_BUTTON_TRIGGER &&
            msg_event != ORMET_PLAY_NEAR_END &&
            msg_event != ORMET_PLAY_END) return;
        if (msg_event == ORMET_PLAY_END) {
            wss_app_recorder_en(ENABLE);
            
            // 等待 VAD 超时
            log_i("ENABLE VAD");
            update_time();
            vad_timeout = 5000;
            threadpool_add_task(g_app_ctx->threadpool, wait_vad_task, &vad_timeout);
            STEP_TO_NEXT_STEP(STATE_PLAYINTRO_PLAYING,      STATE_PLAYINTRO_VAD);
        }
        break;
    case STATE_PLAYINTRO_VAD:
        if (msg_event != ORMET_BUTTON_TRIGGER &&
            msg_event != ORMET_VAD_SPEAKER_END &&
            msg_event != ORMET_NEW_DEVICE_IN &&
            msg_event != ORMET_DEVICE_LEAVE) return;

        if (msg_event == ORMET_VAD_SPEAKER_END) {// TODO
            STEP_TO_NEXT_STEP(STATE_PLAYINTRO_VAD,          STATE_ONLINE);
            return;
        }
        break;
    case STATE_ONEONONE_SWITCH:
        if (msg_event != ORMET_BUTTON_TRIGGER &&
            msg_event != ORMET_SWITCH_SUCCESS &&
            msg_event != ORMET_SWITCH_FAILED) return;
        if (msg_event == ORMET_SWITCH_FAILED || g_edev_last != p_dev) {
            if (p_dev == g_edev_target) g_edev_target = NULL;
            if (p_dev == g_edev_last  ) g_edev_last   = NULL;
            STEP_TO_NEXT_STEP(STATE_ONEONONE_SWITCH,        STATE_ONLINE);
        } 
        break;
    case STATE_ONEONONE_PLAYING:
        if (msg_event != ORMET_BUTTON_TRIGGER &&
            msg_event != ORMET_PLAY_NEAR_END &&
            msg_event != ORMET_PLAY_END) return;
        if (msg_event == ORMET_PLAY_END) {
            wss_app_recorder_en(ENABLE);
            
            // 等待 VAD 超时
            log_i("ENABLE LONG VAD");
            update_time();
            vad_timeout = 600000;
            threadpool_add_task(g_app_ctx->threadpool, wait_vad_task, &vad_timeout);
            STEP_TO_NEXT_STEP(STATE_ONEONONE_PLAYING,       STATE_ONEONONE_VAD);
        }
        break;
    case STATE_ONEONONE_VAD:
        if (msg_event != ORMET_BUTTON_TRIGGER &&
            msg_event != ORMET_DEVICE_LEAVE) return;
        break;
    case STATE_COMMAND_BUTTON:
        if (msg_event != ORMET_BUTTON_PRESS &&
            msg_event != ORMET_BUTTON_DOUBLE_PRESS &&
            msg_event != ORMET_BUTTON_START &&
            msg_event != ORMET_BUTTON_END) return;
        break;
    case STATE_COMMAND_WAITING:
        break;
    }
    if (msg_event == ORMET_SWITCH_SUCCESS || msg_event == ORMET_SWITCH_FAILED) {
        g_has_tts_bind_req = false;
    }
    
    if (send_req_message(g_edev_last ? g_edev_last->sn : "", g_audio_playing, msg_event)) {
        // wss_server_reconnect();
        wss_application_exception_exit();
        return;
    }
}