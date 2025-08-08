#include "wss_request.h"
#include "wss_audio.h"
#include "wss_app_util.h"
#include "cpu.h"

#include "util/cqueue.h"
#include "util/ringbuffer.h"
#include "bt_ble/client/ble_distance.h"
#include "async_msg_loop.h"
#include "app_mode/lwl_app_core.h"
#include "device.h"
#include "util/threadpool_tasks.h"
#include "protocol/cp/c_uart_p.h"
#include "ota/http_ota.h"
#include "uart.h"

#include "protocol/odm/odmp.h"

#include "protocol/odm/odm_secure.h"
#include "protocol/cp/c_uart_p.h"

// 提示音资源路径与本地播放接口
#include "app_config.h"          // for CONFIG_VOICE_PROMPT_FILE_PATH
#include "net_music_play_my.h"   // for app_music_play_local_music_with_callback

#ifndef CONFIG_VOICE_PROMPT_FILE_PATH
#define CONFIG_VOICE_PROMPT_FILE_PATH       "mnt/sdfile/res/audlogo/"
#endif

// 调试：无论后端返回结果如何，强制播放“未注册”提示音
#ifndef DEBUG_FORCE_UNREGISTERED_PROMPT
#define DEBUG_FORCE_UNREGISTERED_PROMPT 1
#endif

static u32 g_curr_time = 0;
#define HEAT_TIMEOUT 5000
static u32 g_heat_timer = 0;

#define G_MAX_VOLUME 12

static int g_exception_exit = false;

static lwl_app_context_t *g_app_ctx = NULL;

static lwl_eend_dev_t *g_edev_target = NULL; // 服务器维护
static lwl_eend_dev_t *g_edev_connected = NULL; // E 已连接

static void set_led_red_low_power(void)        { uart_send_set_led_color(COLOR_RED); } //red~ low power

/*=================================================================================================
 * Audio dec app context
 *=================================================================================================*/
int wait_time = 0;
int last_message_received = 0;
int last_event = 0;

int send_message = 0;


/* Task state */
typedef enum {
    STATE_IDLE,
    STATE_TTS_BIND,

    STATE_DOWNLOAD,
    STATE_UPLOAD,
} AppRequestState;

static int g_mode_state = STATE_IDLE;

static void wss_request_print_state(int mode_state, char* tag)
{
    switch (mode_state) {
    case STATE_IDLE:
        log_i("[%s]STATE_IDLE", tag);
        break;
    case STATE_TTS_BIND:
        log_i("[%s]STATE_TTS_BIND", tag);
        break;
    case STATE_DOWNLOAD:
        log_i("[%s]STATE_DOWNLOAD", tag);
        break;
    case STATE_UPLOAD:
        log_i("[%s]STATE_UPLOAD", tag);
        break;
    }
}

static void wss_request_print_event(int msg_event, char* tag)
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

//=================================================================================================
// MUSIC response callback
//=================================================================================================
void app_music_play_end_cb(void *priv)
{
    log_i("app_music_play_end_cb last_messgae %d %d", last_message_received, last_event);
    if (last_message_received == OAST_TTS && last_event == OASE_END) {
        wss_ble_audio_stop();
        send_message = ORMET_PLAY_END;
    } else {
        send_message = ORMET_PLAY_NEAR_END;
    }
}

//=================================================================================================
// WSS response callback
//=================================================================================================
static void wss_rsp_cbk(int type, odmp_package_t *pkg)
{
    log_i("wss_rsp_cbk level-%d(>=%d): %d STATE = %d\n", ORX_EVENT_CLASS[type], ORX_EVENT_CLASS[last_message_received], type, g_mode_state);
    if (ORX_EVENT_CLASS[type] < ORX_EVENT_CLASS[last_message_received]) return;
    switch (type) {
    case OR_INITIAL_SETUP_RESPONSE:
        log_i("INITIAL_SETUP_RESPONSE\n");
        log_i("setup_complete: %d\n", pkg->initial_setup_response.setup_complete);
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
        log_i("volume: %d\n", pkg->tts_bind.volume);
        int mapped_volume = pkg->tts_bind.volume * G_MAX_VOLUME / 100;
        if (mapped_volume > G_MAX_VOLUME) mapped_volume = G_MAX_VOLUME;
        if (mapped_volume < 0) mapped_volume = 0;

        uart_response_synch_status(1, mapped_volume);/// 音量 E
        
        // lwl_app_context_t *context = lwl_app_core_context_get_instance();
        // set_dec_volume(context->music_player, 80);

        last_message_received = OR_TTS_BIND;
        wait_time = pkg->tts_bind.wait_time;
        g_edev_target = find_eend_dev_by_sn(pkg->tts_bind.send_id);
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
                if (last_message_received != OR_TTS_BIND) {
                    last_message_received = OR_AUDIO_STATUS;
                    last_event = OASE_ABNORMAL;
                }
            }
        } else if (pkg->audio_status.type == OAST_TTS) {
            if (pkg->audio_status.event == OASE_START) {
                log_i("AUDIO_STATUS: OASE_START\n");
                app_music_play_stream_music_with_callback(g_app_ctx->music_player, app_music_play_end_cb, NULL);
                last_message_received = OR_AUDIO_STATUS;
                last_event = OASE_START;
            } else if (pkg->audio_status.event == OASE_END) {
                log_i("AUDIO_STATUS: OASE_END\n");
                last_message_received = OR_AUDIO_STATUS;
                last_event = OASE_END;
            } else if (pkg->audio_status.event == OASE_ABNORMAL) {
                log_i("AUDIO_STATUS: OASE_ABNORMAL\n");
                last_message_received = OR_AUDIO_STATUS;
                last_event = OASE_ABNORMAL;
            } else if (pkg->audio_status.event == OASE_SENTENCE_START) {
                log_i("AUDIO_STATUS: OASE_SENTENCE_START\n");
            } else if (pkg->audio_status.event == OASE_SENTENCE_END) {
                log_i("AUDIO_STATUS: OASE_SENTENCE_END\n");
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
        if (pdev == g_edev_target) {
            send_message = ORMET_SWITCH_SUCCESS;
        }
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
        send_message = ORMET_SWITCH_FAILED;
    }
}

void wss_request_stop()
{
    log_i("Stop WSS request\n");

    // 在STATE_DOWNLOAD时发送TTS异常
    if (g_mode_state == STATE_DOWNLOAD) {
        uint32_t cmd_id = 0;
        bka_e_turn_off(g_edev_target, NULL, g_edev_target, &cmd_id);
        send_audio_status(OAST_TTS, OASE_ABNORMAL); // 通知TTS异常
    }
    g_edev_target = NULL; // 清除目标设备

    wss_ble_audio_stop(); // 停止音频播放
    wss_mic_stop(); // 停止录音

    // 重置状态到IDLE
    g_mode_state = STATE_IDLE;
    last_message_received = 0;
    last_event = 0;
}

void wss_request_init()
{
    log_i("Init WSS request\n");
    g_app_ctx = lwl_app_core_context_get_instance();
    /* 注册odmp事件回调 */
    odmp_register_rsp_cbk(wss_rsp_cbk);

    g_mode_state = STATE_IDLE;
    wss_request_send_event(NULL, ORMET_INITIAL_SETUP);
}

void wss_request_uninit()
{
    log_i("Uninit WSS request\n");

    /* 取消注册loswssp事件回调 */
    odmp_register_rsp_cbk(NULL);
    wss_request_stop();
}

bool wss_request_loop()
{
    // 使用 last_message_received 和 last_event 替代 last_audio_status_type、last_audio_status_event、last_tts_bind_received

    switch (g_mode_state) {
    case STATE_IDLE:
        // 等待 OR_TTS_BIND 事件
        if (last_message_received == OR_TTS_BIND) {
            g_mode_state = STATE_TTS_BIND;
            wss_request_print_state(g_mode_state, "STEP_TO");
            last_message_received = 0;
            wss_mic_stop(); // 停止录音
            int cmd_id;
            log_i("bka_e_turn_on %s\n", g_edev_target->sn);
            bka_e_turn_on(g_edev_target, e_turn_on_cb, g_edev_target, &cmd_id);
        }
        // VAD信号响应，进入上传
        if (is_voice_availble()) {
            g_mode_state = STATE_UPLOAD;
            wss_request_print_state(g_mode_state, "STEP_TO");
            wss_request_send_event(g_edev_target, ORMET_VAD_SPEAKER_START);
        }
        break;

    case STATE_TTS_BIND:
        // TTS下载流程
        if (send_message == ORMET_SWITCH_SUCCESS) {
            log_i("Switch success, starting audio playback");
            wss_ble_audio_start();
            wss_request_send_event(g_edev_target, ORMET_SWITCH_SUCCESS);
            last_message_received = 0;
            last_event = 0;
            send_message = 0; // 重置发送消息标志
        } else if (send_message == ORMET_SWITCH_FAILED) {
            log_i("Switch failed, returning to STATE_IDLE");
            g_mode_state = STATE_IDLE;
            wss_request_print_state(g_mode_state, "STEP_TO");
            wss_request_send_event(g_edev_target, ORMET_SWITCH_FAILED);
            last_message_received = 0;
            last_event = 0;
            send_message = 0; // 重置发送消息标志
        } else if (last_message_received == OR_AUDIO_STATUS && last_event == OASE_START) {
            g_mode_state = STATE_DOWNLOAD;
            wss_request_print_state(g_mode_state, "STEP_TO");
            last_message_received = 0;
            last_event = 0;
        } else if (last_message_received == OR_TTS_BIND) {
            g_mode_state = STATE_TTS_BIND;
            wss_request_print_state(g_mode_state, "STEP_TO Again");
            last_message_received = 0;
            int cmd_id;
            log_i("bka_e_turn_on %s\n", g_edev_target->sn);
            if (g_edev_connected) {
                bka_e_turn_off(g_edev_connected, NULL, g_edev_connected, &cmd_id);
            }
            bka_e_turn_on(g_edev_target, e_turn_on_cb, g_edev_target, &cmd_id);
        }
        break;

    case STATE_DOWNLOAD:
        // 下载结束，回到TTS_BIND
        if (last_message_received == OR_AUDIO_STATUS && last_event == OASE_END){
            // log_i("Download finished, returning to TTS_BIND %d", send_message);
        }
        if ((last_message_received != OR_AUDIO_STATUS || last_event != OASE_END) && send_message == ORMET_PLAY_NEAR_END) {
            log_i("Download continue  reset music callback");
            app_music_play_stream_music_with_callback(g_app_ctx->music_player, app_music_play_end_cb, NULL);
            send_message = 0; // 重置发送消息标志
        }
        if (last_message_received == OR_AUDIO_STATUS && last_event == OASE_END && send_message == ORMET_PLAY_END) {
            g_mode_state = STATE_IDLE;
            wss_request_print_state(g_mode_state, "STEP_TO");
            wss_request_send_event(g_edev_target, ORMET_PLAY_END);
            wss_mic_start(get_time_stamp() + 600000); // 开始录音
            
            uint32_t cmd_id=0;
            bka_e_turn_off(g_edev_target, NULL, g_edev_target, &cmd_id);

            last_message_received = 0;
            last_event = 0;
            send_message = 0; // 重置发送消息标志
        }
        wss_application_vailed();
        break;

    case STATE_UPLOAD:
        // VAD信号结束，回到TTS_BIND
        if (!is_voice_availble()) {
            g_mode_state = STATE_IDLE;
            wss_request_print_state(g_mode_state, "STEP_TO");
            wss_request_send_event(g_edev_target, ORMET_VAD_SPEAKER_END);
        }
        wss_application_vailed();
        break;
    }

    return (g_mode_state == STATE_IDLE);
}

void wss_request_send_event(lwl_eend_dev_t *p_dev, int msg_event)
{
    wss_request_print_event(msg_event, "wss_request_send_event");
    
    if (send_req_message(p_dev ? p_dev->sn : "", is_audio_playing(), msg_event)) {
        return;
    }
}