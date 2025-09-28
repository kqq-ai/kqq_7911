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
#include "bt_ble/client/ble_dev_task_scheduler.h"

// 提示音资源路径与本地播放接口
#include "net_music_play_my.h"   // for app_music_play_local_music_with_callback

#include "log/log_mqtt.h"

static u32 g_curr_time = 0;
#define HEAT_TIMEOUT 5000
static u32 g_heat_timer = 0;

static lwl_app_context_t *g_app_ctx = NULL;

static char g_edev_target_sn[SN_SIZE]; // 服务器维护
static char g_edev_connected_sn[SN_SIZE]; // E 已连接

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
    
    STATE_PLAYING, // 播放状态
} AppRequestState;

AppRequestState g_mode_state = STATE_IDLE;

bool is_request_busy(){
    return (g_mode_state != STATE_IDLE);
}

void turn_off_Pi() {
    if (g_edev_connected_sn[0] != 0) {
        
        lwl_eend_dev_t *pdev = find_eend_dev_by_sn(g_edev_connected_sn);
        bdo_turn_off(pdev, 500, NULL, NULL);
        g_edev_connected_sn[0] = 0;
    }
}

static void wss_request_print_state(AppRequestState mode_state, char* tag)
{
    switch (mode_state) {
    case STATE_IDLE:
        log_i("[%s]STEP_TO STATE_IDLE", tag);
        break;
    case STATE_TTS_BIND:
        log_i("[%s]STEP_TO STATE_TTS_BIND", tag);
        break;
    case STATE_DOWNLOAD:
        log_i("[%s]STEP_TO STATE_DOWNLOAD", tag);
        break;
    case STATE_UPLOAD:
        log_i("[%s]STEP_TO STATE_UPLOAD", tag);
        break;
    }
}

static void wss_request_print_event(int msg_event, char* tag)
{
    switch (msg_event) {
    case ORMET_INITIAL_SETUP:
        log_i("[%s]send ORMET_INITIAL_SETUP", tag);
        break;
    case ORMET_NEAR_TRIGGER:
        log_i("[%s]send ORMET_NEAR_TRIGGER", tag);
        break;
    case ORMET_NEW_DEVICE_IN:
        log_i("[%s]send ORMET_NEW_DEVICE_IN", tag);
        break;
    case ORMET_SWITCH_SUCCESS:
        log_i("[%s]send ORMET_SWITCH_SUCCESS", tag);
        break;
    case ORMET_SWITCH_FAILED:
        log_i("[%s]send ORMET_SWITCH_FAILED", tag);
        break;
    case ORMET_BUTTON_TRIGGER:
        log_i("[%s]send ORMET_BUTTON_TRIGGER", tag);
        break;
    case ORMET_BUTTON_PRESS:
        log_i("[%s]send ORMET_BUTTON_PRESS", tag);
        break;
    case ORMET_BUTTON_DOUBLE_PRESS:
        log_i("[%s]send ORMET_BUTTON_DOUBLE_PRESS", tag);
        break;
    case ORMET_BUTTON_START:
        log_i("[%s]send ORMET_BUTTON_START", tag);
        break;
    case ORMET_BUTTON_END:
        log_i("[%s]send ORMET_BUTTON_END", tag);
        break;
    case ORMET_MOVE_PATROL:
        log_i("[%s]send ORMET_MOVE_PATROL", tag);
        break;
    case ORMET_MOVE_STOP:
        log_i("[%s]send ORMET_MOVE_STOP", tag);
        break;
    case ORMET_MOVE_SHAKE:
        log_i("[%s]send ORMET_MOVE_SHAKE", tag);
        break;
    case ORMET_VAD_SPEAKER_START:
        log_i("[%s]send ORMET_VAD_SPEAKER_START", tag);
        break;
    case ORMET_VAD_SPEAKER_END:
        log_i("[%s]send ORMET_VAD_SPEAKER_END", tag);
        break;
    case ORMET_PLAY_START:
        log_i("[%s]send ORMET_PLAY_START", tag);
        break;
    case ORMET_PLAY_NEAR_END:
        log_i("[%s]send ORMET_PLAY_NEAR_END", tag);
        break;
    case ORMET_PLAY_END:
        log_i("[%s]send ORMET_PLAY_END", tag);
        break;
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
        if (strlen(pkg->tts_bind.send_id)!= SN_SIZE-1) break;
        set_Pi_volume(pkg->tts_bind.volume);

        last_message_received = OR_TTS_BIND;
        wait_time = pkg->tts_bind.wait_time;
        memcpy(g_edev_target_sn, pkg->tts_bind.send_id, SN_SIZE);
    }   break;
    case OR_AUDIO_STATUS:
        log_i("AUDIO_STATUS\n");
        log_i("c_id: %s\n", pkg->audio_status.c_id);
        log_i("type: %s\n", odmp_ast_to_text(pkg->audio_status.type));
        log_i("event: %s\n", odmp_ase_to_text(pkg->audio_status.event));
        log_i("format: %s\n", pkg->audio_status.audio_param.format);
        log_i("sample_rate: %d\n", pkg->audio_status.audio_param.sample_rate);
        
        if (pkg->audio_status.type == OAST_ASR) {
            if (pkg->audio_status.event == OASE_ABNORMAL) {
                log_i("AUDIO_STATUS: ASR OASE_ABNORMAL\n");
                if (last_message_received != OR_TTS_BIND) {
                    last_message_received = OR_AUDIO_STATUS;
                    last_event = OASE_ABNORMAL;
                }
            }
            if (pkg->audio_status.event == OASE_START) {
                last_message_received = OR_AUDIO_STATUS;
                last_event = 5 + OASE_START;
            }
        } else if (pkg->audio_status.type == OAST_TTS) {
            if (pkg->audio_status.event == OASE_START) {
                log_i("AUDIO_STATUS: OASE_START\n");
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
                // STOP TTS !!!
            } else if (pkg->audio_status.event == OASE_SENTENCE_START) {
                log_i("AUDIO_STATUS: OASE_SENTENCE_START\n");
            } else if (pkg->audio_status.event == OASE_SENTENCE_END) {
                log_i("AUDIO_STATUS: OASE_SENTENCE_END\n");
            }
        }
        break;
    case OR_MESSAGE_TEXT:
        // if (pkg->message_text.type == 1)
        //     log_i("ASR_TEXT %d %s\n", pkg->message_text.text, pkg->message_text.text);
        // if (pkg->message_text.type == 2)
        //     log_i("TTS_TEXT %d %s\n", pkg->message_text.c_id, pkg->message_text.text);
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
static void e_turn_on_cb(bdo_code_e code, void *priv)
{
    lwl_eend_dev_t *pdev = (lwl_eend_dev_t*)(priv);
    
    char sn_string[SN_SIZE * 2 + 1] = "(NULL)";
    if (pdev) {
        pdev->get_string(pdev, sn_string, sizeof(sn_string));
    }
    switch(code) {
    case BDOCode_Success:

        log_i("Switch PortE %s OK\n", sn_string);
        memcpy(g_edev_connected_sn, g_edev_target_sn, SN_SIZE);
        if (strcmp(pdev->sn,g_edev_target_sn)==0) {
            send_message = ORMET_SWITCH_SUCCESS;
        }
        break;
    case BDOCode_Connection_Timeout:
        log_i("Switch PortE BDOCode_Connection_Timeout\n");
        break;
    case BDOCode_Disconnect_Timeout:
        log_i("Switch PortE BDOCode_Disconnect_Timeout\n");
        break;
    case BDOCode_Scan_Adv_Stat_Timeout:
        log_i("Switch PortE BDOCode_Scan_Adv_Stat_Timeout\n");
        break;
    case BDOCode_Unexpected_Error:
        log_i("Switch PortE Error\n");
        break;
    }
    if (code != BDOCode_Success) {
        send_message = ORMET_SWITCH_FAILED;
    }
}

void wss_request_stop()
{
    log_i("Stop WSS request\n");

    // 在STATE_DOWNLOAD时发送TTS异常
    if (g_mode_state == STATE_DOWNLOAD || g_mode_state == STATE_PLAYING) {
        // turn_off_Pi();
        g_mode_state = STATE_IDLE;
        wss_request_print_state(g_mode_state, "Send AUDIO ABNORMAL(TTS)");
        wss_ble_audio_stop(); // 停止音频播放
    } else if (g_mode_state == STATE_TTS_BIND) {
        send_message = ORMET_SWITCH_FAILED;
    }
    g_edev_target_sn[0] = 0; // 清除目标设备

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

    // turn_off_Pi();
    /* 取消注册loswssp事件回调 */
    odmp_register_rsp_cbk(NULL);
    wss_request_stop();
    wss_mic_stop(0);
    // msleep(500);
}

bool wss_request_loop()
{
    // 使用 last_message_received 和 last_event 替代 last_audio_status_type、last_audio_status_event、last_tts_bind_received

    lwl_eend_dev_t *pdev = find_eend_dev_by_sn(g_edev_target_sn);
    switch (g_mode_state) {
    case STATE_IDLE:
        // 等待 OR_TTS_BIND 事件
        if (last_message_received == OR_TTS_BIND) {
            g_mode_state = STATE_TTS_BIND;
            wss_request_print_state(g_mode_state, "receive TTS_BIND");
            last_message_received = 0;
            wss_mic_stop(0); // 停止录音
            wss_request_send_event(g_edev_target_sn, ORMET_SWITCH_SUCCESS);
        } else if (last_message_received == OR_AUDIO_STATUS && last_event == 5 + OASE_START) {
            wss_request_print_state(g_mode_state, "receive AUDIO START ASR");
            wss_mic_start(get_time_stamp() + 600000); // 开始录音
            last_message_received = 0;
            last_event = 0;
        }
        // VAD信号响应，进入上传
        if (is_voice_availble()) {
            g_mode_state = STATE_UPLOAD;
            wss_request_print_state(g_mode_state, "Vioce On");
            wss_request_send_event(g_edev_target_sn, ORMET_VAD_SPEAKER_START);
        }
        break;

    case STATE_TTS_BIND:
        // TTS下载流程
        if (send_message == ORMET_SWITCH_SUCCESS) {
            log_i("Switch success, starting audio playback");
            wss_request_send_event(g_edev_target_sn, ORMET_SWITCH_SUCCESS);
            last_message_received = 0;
            last_event = 0;
            send_message = 0; // 重置发送消息标志
        } else if (send_message == ORMET_SWITCH_FAILED || !pdev) {
            g_mode_state = STATE_IDLE;
            wss_request_print_state(g_mode_state, "SWITCH_FAILED");
            wss_request_send_event(g_edev_target_sn, ORMET_SWITCH_FAILED);
            last_message_received = 0;
            last_event = 0;
            send_message = 0; // 重置发送消息标志
        } else if (last_message_received == OR_AUDIO_STATUS && last_event == OASE_START) {
            g_mode_state = STATE_DOWNLOAD;
            wss_request_print_state(g_mode_state, "receive AUDIO START");
            wss_ble_audio_start();
            last_message_received = 0;
            last_event = 0;
        } else if (last_message_received == OR_TTS_BIND) {
            g_mode_state = STATE_TTS_BIND;
            wss_request_print_state(g_mode_state, "receive TTS_BIND Again");
            last_message_received = 0;
            log_i("bdo_turn_on %s\n", g_edev_target_sn);
            wss_request_send_event(g_edev_target_sn, ORMET_SWITCH_SUCCESS);
        }
        break;

    case STATE_DOWNLOAD:
        // 下载结束，继续播放
        if (last_message_received == OR_AUDIO_STATUS && last_event == OASE_END){
            g_mode_state = STATE_PLAYING;
            wss_request_print_state(g_mode_state, "receive AUDIO END");
            last_message_received = 0;
            last_event = 0;
        }
        // 音频异常，终止播放
        if (last_message_received == OR_AUDIO_STATUS && last_event == OASE_ABNORMAL) {
            g_mode_state = STATE_IDLE;
            wss_request_print_state(g_mode_state, "receive AUDIO ABNORMAL");
            wss_ble_audio_stop();
            last_message_received = 0;
            last_event = 0;
        }
        wss_application_vailed();
        break;

    case STATE_UPLOAD:
        // VAD信号结束，回到TTS_BIND
        if (!is_voice_availble()) {
            g_mode_state = STATE_IDLE;
            wss_request_print_state(g_mode_state, "VAD state is not voice");
            wss_request_send_event(g_edev_target_sn, ORMET_VAD_SPEAKER_END);
        }
        wss_application_vailed();
        break;

    case STATE_PLAYING:
        // 音频异常，终止播放
        if (last_message_received == OR_AUDIO_STATUS && last_event == OASE_ABNORMAL) {
            g_mode_state = STATE_IDLE;
            wss_request_print_state(g_mode_state, "receive AUDIO ABNORMAL");
            wss_ble_audio_stop();
            last_message_received = 0;
            last_event = 0;
        }
        // 播放结束，打开麦克风
        if (!is_audio_playing()) {
            g_mode_state = STATE_IDLE;
            wss_request_print_state(g_mode_state, "[Decoder Play end cb]STEP_TO");
            wss_ble_audio_stop();
            // turn_off_Pi();
            wss_request_send_event(g_edev_target_sn, ORMET_PLAY_END);

            wss_mic_start(get_time_stamp() + 600000); // 开始录音
            last_message_received = 0;
            last_event = 0;
        } else {
            wss_application_vailed();
        }
        break;
    }
    

    return (g_mode_state == STATE_IDLE);
}

void wss_request_send_event(char *sn, int msg_event)
{
    wss_request_print_event(msg_event, "wss_request_send_event");
    if (sn != NULL && strlen(sn)!= SN_SIZE-1) {
        sn = NULL;
    }
    if (send_req_message(sn, is_audio_playing(), msg_event)) {
        return;
    }
}