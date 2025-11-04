#include "wss_request.h"
#include "wss_audio.h"
#include "wss_app_util.h"
#include "cpu.h"
#include "app_cookie.h"
#include "wss_application.h"

#include "util/cqueue.h"
#include "util/ringbuffer.h"
#include "bt_ble/client/ble_distance.h"
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
static u32 g_turnon_timer = 0;
#define WSS_TIMEOUT 10000

static lwl_app_context_t *g_app_ctx = NULL;

static char g_edev_target_sn[SN_SIZE]; // 服务器维护
static char g_edev_connected_sn[SN_SIZE]; // E 已连接

static void set_led_red_error(void)        { uart_send_set_led_color(COLOR_RED); } //red~ low power
// static void set_led_tts_bind(void)      { uart_send_set_led_color(COLOR_ORANGE); }   //TTS_BIND时灯光变黄色

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

bool wss_request_is_busy(){
    return (g_mode_state != STATE_IDLE);
}

bool is_player_busy(){
    return (g_mode_state == STATE_DOWNLOAD || is_audio_playing());
}

void edev_working(){
    g_turnon_timer = g_curr_time;
}

void turn_off_Pi() {
    wss_ble_audio_stop();
#ifdef C_SPEAKLER
    return;
#endif
    if (g_edev_connected_sn[0] != 0) {
        
        lwl_eend_dev_t *pdev = find_eend_dev_by_sn(g_edev_connected_sn);
        if (pdev) {
            bdo_turn_off(pdev, 500, NULL, NULL);
        } else {
            log_i("turn_off_Pi find_eend_dev_by_sn %s NULL\n", g_edev_connected_sn);
        }
        g_edev_connected_sn[0] = 0;
    }
}
static void e_turn_on_cb(bdo_code_e code, void *priv);

void turn_on_Pi(const char *pdev_sn) {
    wss_mic_stop(); // 停止录音
    // set_led_tts_bind();
#ifdef C_SPEAKLER
    log_i("bdo_turn_on %s\n", pdev_sn);
    memcpy(g_edev_connected_sn, pdev_sn, SN_SIZE);
    send_message = ORMET_SWITCH_SUCCESS;
    edev_working();
    return;
#endif
// 如果sn和g_edev_connected_sn 不相同则关闭之前的连接
    if (strncmp(g_edev_connected_sn, pdev_sn, SN_SIZE) != 0) {
        turn_off_Pi();
    }
    lwl_eend_dev_t *pdev = find_eend_dev_by_sn(pdev_sn);
    if (pdev == NULL) {
        log_i("turn_on_Pi find_eend_dev_by_sn %s NULL\n", pdev_sn);
        return;
    }
    //上传E的电量
    log_mqtt_set_battery_level(pdev->sn, (pdev->bat_level)*10);
    bdo_turn_on(pdev, 9000, 9000, e_turn_on_cb, pdev);
    edev_working();
}

static bool g_mic_on = true;
void set_mic(bool on)
{
    g_mic_on = on;
    if (!on) {
        wss_mic_stop();
    }
}

void wss_request_resume_mic()
{
    if (g_mic_on && !wss_request_is_busy()) {
        wss_mic_start();
    }
}

#define G_MAX_VOLUME 70
int g_volume = G_MAX_VOLUME;
// 设置播放器音量(1~70)
void set_tts_volume(int volume) {
    if (volume < 1) volume = 1;
    if (volume > 100) volume = 100;
    g_volume = volume * G_MAX_VOLUME / 100; // 保护E的电池，需要设置最高默认音量为 G_MAX_VOLUME
}

void wss_request_resume_le_audio()
{
    set_Pi_volume(g_volume);
}

static void wss_request_update_state(AppRequestState mode_state, char* tag)
{
    switch (mode_state) {
    case STATE_IDLE:
        log_i("%d -> [%s]STEP_TO STATE_IDLE", g_mode_state, tag);
        break;
    case STATE_TTS_BIND:
        log_i("%d -> [%s]STEP_TO STATE_TTS_BIND", g_mode_state, tag);
        break;
    case STATE_DOWNLOAD:
        log_i("%d -> [%s]STEP_TO STATE_DOWNLOAD", g_mode_state, tag);
        break;
    case STATE_UPLOAD:
        log_i("%d -> [%s]STEP_TO STATE_UPLOAD", g_mode_state, tag);
        break;
    }
    g_mode_state = mode_state;
    last_message_received = 0;
    last_event = 0;
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
        break;
    case OR_DEVICE_SWITCH_RESPONSE:
        log_i("DEVICE_SWITCH_RESPONSE\n");
        break;
    case OR_TTS_BIND: {
        log_i("TTS_BIND\n");
        log_i("c_id: %s\n", pkg->tts_bind.c_id);
        log_i("send_id: %s\n", pkg->tts_bind.send_id);
        log_i("wait_time: %d\n", pkg->tts_bind.wait_time);
        log_i("volume: %d\n", pkg->tts_bind.volume);
        if (strlen(pkg->tts_bind.send_id)!= SN_SIZE-1) break;
        play_prompt_sound("TTSBind.mp3");
        set_tts_volume(pkg->tts_bind.volume);

        last_message_received = OR_TTS_BIND;
        send_message = 0; // 重置发送消息标志
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
            if (pkg->audio_status.event == OASE_END) {
                last_message_received = OR_AUDIO_STATUS;
                last_event = 5 + OASE_END;
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
        // log_i("EXECUTION_TIME %s %d\n", pkg->execution_time.event, pkg->execution_time.execution_time);
        break;
    case OR_PONG:
        // log_i("LR_PONG\n");
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
        memcpy(g_edev_connected_sn, sn_string, SN_SIZE);
        if (strcmp(sn_string,g_edev_target_sn) == 0) {
            send_message = ORMET_SWITCH_SUCCESS;
        } else {
            turn_off_Pi();
        }
        edev_working();
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
    if (code != BDOCode_Success && strcmp(sn_string,g_edev_target_sn) == 0) {
        send_message = ORMET_SWITCH_FAILED;
    }
}

void wss_request_stop()
{
    log_i("Stop WSS request\n");

    turn_off_Pi();
    // 在STATE_DOWNLOAD时发送TTS异常
    if (g_mode_state == STATE_DOWNLOAD || g_mode_state == STATE_PLAYING) {
        wss_request_update_state(STATE_IDLE, "wss_request_stop");
    } else if (g_mode_state == STATE_TTS_BIND) {
        wss_request_update_state(STATE_IDLE, "wss_request_stop");
        wss_request_send_event(g_edev_target_sn, ORMET_SWITCH_FAILED);
    } else {
        wss_vad_abort();
    }

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
    wss_audio_init();
    g_mic_on = true;

    g_mode_state = STATE_IDLE;
    wss_request_send_event(NULL, ORMET_INITIAL_SETUP);
    send_audio_format(OAST_TTS);
    // wss_mic_stop(); // 停止录音
}

void wss_request_uninit()
{
    log_i("Uninit WSS request\n");

    /* 取消注册loswssp事件回调 */
    odmp_register_rsp_cbk(NULL);
    wss_request_stop();
    wss_audio_uninit();
    // msleep(500);
}

bool wss_request_loop()
{
    // 使用 last_message_received 和 last_event 替代 last_audio_status_type、last_audio_status_event、last_tts_bind_received

    g_curr_time = timer_get_ms();
    switch (g_mode_state) {
    case STATE_IDLE:
        // 等待 OR_TTS_BIND 事件
        if (last_message_received == OR_TTS_BIND) {
            wss_request_update_state(STATE_TTS_BIND, "receive TTS_BIND");
            turn_on_Pi(g_edev_target_sn);
        } else if (last_message_received == OR_AUDIO_STATUS && last_event == 5 + OASE_START) {
            wss_request_update_state(STATE_IDLE, "receive AUDIO START ASR");
            wss_request_resume_mic();
            wss_vad_start(600000); // 开始录音
        } else if (last_message_received == OR_AUDIO_STATUS && (last_event == 5+OASE_END || last_event == 5+OASE_ABNORMAL)) {
            wss_request_update_state(STATE_IDLE, "receive AUDIO OASE_END ASR");
            wss_vad_abort(); // 停止录音
        }
        // 音频上传开始
        if (is_recording_uploading()) {
            if (send_message == 0) {
                send_message = ORMET_VAD_SPEAKER_START;
                wss_request_send_event(g_edev_target_sn, send_message);
            }
            wss_request_update_state(STATE_UPLOAD, "Vioce On");
        }

        break;

    case STATE_TTS_BIND:
        // TTS下载流程
        if (send_message == ORMET_SWITCH_SUCCESS) {
            log_i("Switch success, starting audio playback");
            wss_request_send_event(g_edev_target_sn, send_message);
            send_message = 0; // 重置发送消息标志
        } else if (last_message_received == OR_TTS_BIND) {
            wss_request_update_state(STATE_TTS_BIND, "receive TTS_BIND Again");
            turn_on_Pi(g_edev_target_sn);
        } else if (last_message_received == OR_AUDIO_STATUS && last_event == OASE_START) {
            wss_request_update_state(STATE_DOWNLOAD, "receive AUDIO START");
            wss_ble_audio_start();
            last_event = 0;
        } else if (last_message_received == OR_AUDIO_STATUS && (last_event == OASE_END || last_event == OASE_ABNORMAL)) {
            wss_request_update_state(STATE_IDLE, "OR_AUDIO_STATUS ABNORMAL/END");
            turn_off_Pi();
            set_led_red_error();
        } else if (send_message == ORMET_SWITCH_FAILED) {
            wss_request_update_state(STATE_IDLE, "SWITCH_FAILED");
            wss_request_send_event(g_edev_target_sn, send_message);
            send_message = 0; // 重置发送消息标志
            turn_off_Pi();
            set_led_red_error();
        } else if (g_curr_time - g_turnon_timer > WSS_TIMEOUT) {
            wss_request_update_state(STATE_IDLE, "WSS AUDIO TIMEOUT");
            send_audio_status(OAST_TTS, OASE_ABNORMAL); // 通知TTS异常
            turn_off_Pi();
            set_led_red_error();
        }
        break;

    case STATE_DOWNLOAD:
        // 下载结束，继续播放
        if (last_message_received == OR_AUDIO_STATUS && last_event == OASE_SENTENCE_START){
            edev_working();
        }
        if (last_message_received == OR_AUDIO_STATUS && last_event == OASE_END){
            wss_request_update_state(STATE_PLAYING, "receive AUDIO END");
        }
        // 音频异常，终止播放
        if (last_message_received == OR_AUDIO_STATUS && last_event == OASE_ABNORMAL) {
            wss_request_stop();
            set_led_red_error();
        } else if (g_curr_time - g_turnon_timer > WSS_TIMEOUT) {
            wss_request_stop();
            set_led_red_error();
        }
        wss_application_vailed();
        break;

    case STATE_UPLOAD:
        // 音频上传结束，回到TTS_BIND
        if (!is_recording_uploading()) {
            g_mode_state = STATE_IDLE;
            wss_request_update_state(g_mode_state, "VAD state is not voice");
            wss_request_send_event(g_edev_target_sn, ORMET_VAD_SPEAKER_END);
        } else if (last_message_received == OR_TTS_BIND) {
            wss_request_update_state(STATE_TTS_BIND, "receive TTS_BIND");
            turn_on_Pi(g_edev_target_sn);
        }
        wss_application_vailed();
        break;

    case STATE_PLAYING:
        // 音频异常，终止播放
        if (last_message_received == OR_AUDIO_STATUS && last_event == OASE_ABNORMAL) {
            wss_request_stop();
            set_led_red_error();
        } else if (g_curr_time - g_turnon_timer > 1000) {
        // 播放结束，打开麦克风
            if (!is_audio_playing()) {
                wss_request_update_state(STATE_IDLE, "[Decoder Play end cb]STEP_TO");
                turn_off_Pi();
                wss_request_send_event(g_edev_target_sn, ORMET_PLAY_END);
                wss_request_resume_mic();
                wss_vad_start(600000); // 开始录音
            }
        }
        wss_application_vailed();
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
    if (sn != NULL) {
        update_edev_time(sn);
    }
    send_req_message(sn, is_audio_playing(), msg_event);
}