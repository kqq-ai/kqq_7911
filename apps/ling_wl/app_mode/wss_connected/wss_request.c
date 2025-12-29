#include "wss_request.h"
#include "wss_audio.h"
#include "wss_app_util.h"
#include "cpu.h"
#include "app_cookie.h"
#include "wss_application.h"

#include "app_mode/lwl_app_core.h"
#include "protocol/odm/odmp.h"
#include "bt_ble/client/ble_dev_task_scheduler.h"

#include "log/log_mqtt.h"
#include "LED/LED.h"

#define WSS_TIMEOUT 10000
#define WSS_MUSIC_TIMEOUT 870

static u32 g_curr_time = 0;
static u32 g_wss_last_time = 0;
static u32 g_mode_last_time = 0;
static OS_MUTEX g_request_mutex;

static lwl_app_context_t *g_app_ctx = NULL;
/*=================================================================================================
 * 请求状态
 *=================================================================================================*/
static int wait_time = 0;
static int last_message_type = 0;
static int last_event = 0;

static int send_message = 0;

static bool g_mode_script = 0; // 如果true则不播放切换提示音

static char g_edev_target_sn[SN_SIZE+1]; // 服务器维护
static char g_edev_connected_sn[SN_SIZE+1]; // E 已连接

//=================================================================================================
// 当前状态维护
//=================================================================================================
typedef enum {
    STATE_IDLE,
    STATE_TTS_BIND,

    STATE_DOWNLOAD,
    STATE_PLAYING, // 播放状态
    STATE_WAITING,
} AppRequestState;

AppRequestState g_mode_state = STATE_IDLE;

bool wss_request_is_busy(){
    return (g_mode_state != STATE_IDLE);
}

bool wss_request_is_playing(){
    return (g_mode_state != STATE_IDLE && g_mode_state != STATE_WAITING);
}

static void wss_request_update_state(AppRequestState mode_state, char* tag)
{
    switch (mode_state) {
    case STATE_IDLE:
        log_i("[wss_request]%d -> [%s]STEP_TO STATE_IDLE", g_mode_state, tag);
        break;
    case STATE_TTS_BIND:
        log_i("[wss_request]%d -> [%s]STEP_TO STATE_TTS_BIND", g_mode_state, tag);
        break;
    case STATE_DOWNLOAD:
        log_i("[wss_request]%d -> [%s]STEP_TO STATE_DOWNLOAD", g_mode_state, tag);
        break;
    case STATE_PLAYING:
        log_i("[wss_request]%d -> [%s]STEP_TO STATE_PLAYING", g_mode_state, tag);
        break;
    case STATE_WAITING:
        log_i("[wss_request]%d -> [%s]STEP_TO STATE_WAITING", g_mode_state, tag);
        break;
    }
    g_mode_state = mode_state;
    if (STATE_IDLE == g_mode_state && g_mode_script) g_mode_state = STATE_WAITING;
    g_mode_last_time = g_curr_time;
    last_message_type = 0;
    last_event = 0;
}

//=================================================================================================
// Pi设备切换 callback
//=================================================================================================
void turn_off_Pi() {
    if (g_edev_connected_sn[0] != 0) {
        
        lwl_eend_dev_t *pdev = find_eend_dev_by_sn(g_edev_connected_sn);
        if (pdev) {
            bdo_turn_off(pdev);
        } else {
            log_i("[wss_request]turn_off_Pi find_eend_dev_by_sn %s NULL\n", g_edev_connected_sn);
        }
        g_edev_connected_sn[0] = 0;
    }
}

static void e_turn_on_cb(bdo_code_e code, void *priv)
{
    // if (g_mode_state != STATE_TTS_BIND) return;
    os_mutex_pend(&g_request_mutex, 0);
    lwl_eend_dev_t *pdev = (lwl_eend_dev_t*)(priv);
    
    char sn_string[SN_SIZE + 1] = "(NULL)";
    if (pdev) {
        pdev->get_string(pdev, sn_string, sizeof(sn_string));
    }
    switch(code) {
    case BDOCode_Success:
        log_i("[wss_request]Switch PortE %s OK\n", sn_string);
        break;
    case BDOCode_Connection_Timeout:
        log_i("[wss_request]Switch PortE BDOCode_Connection_Timeout\n");
        break;
    case BDOCode_Disconnect_Timeout:
        log_i("[wss_request]Switch PortE BDOCode_Disconnect_Timeout\n");
        break;
    case BDOCode_Scan_Adv_Stat_Timeout:
        log_i("[wss_request]Switch PortE BDOCode_Scan_Adv_Stat_Timeout\n");
        break;
    case BDOCode_Unexpected_Error:
        log_i("[wss_request]Switch PortE Error\n");
        break;
    case BDOCode_Ready_to_Play:
        log_i("[wss_request]Switch PortE Ready_to_Play\n");
        break;
    }
    if (code == BDOCode_Success) { // 连接成功
        memcpy(g_edev_connected_sn, sn_string, SN_SIZE+1);
        if (strcmp(sn_string,g_edev_target_sn) == 0) { // 连接的是目标E
            send_message = ORMET_SWITCH_SUCCESS;
            log_i("[wss_request]send_message = %d", send_message);
        } else {
            turn_off_Pi();
        }
    } else if (BDOCode_Ready_to_Play == code) {
        if (strcmp(sn_string,g_edev_target_sn) == 0) { // 连接的是目标E
            send_message = ORMET_PLAY_START;
            log_i("[wss_request]send_message = %d", send_message);
        }
    } else {
        if (strcmp(sn_string,g_edev_target_sn) == 0) { // 连接的目标E失败
            send_message = ORMET_SWITCH_FAILED;
        }
    }
    os_mutex_post(&g_request_mutex);
}

void turn_on_Pi(const char *pdev_sn) {
    send_message = 0; // 重置发送消息标志
    log_i("[wss_request]turn_on_Pi %s\n", pdev_sn);
// 如果sn和g_edev_connected_sn 不相同则关闭之前的连接
    if (strncmp(g_edev_connected_sn, pdev_sn, SN_SIZE) != 0) {
        turn_off_Pi();
    }
    lwl_eend_dev_t *pdev = find_eend_dev_by_sn(pdev_sn);
    if (pdev == NULL) {
        log_i("[wss_request]turn_on_Pi find_eend_dev_by_sn %s NULL\n", pdev_sn);
        send_message = ORMET_SWITCH_FAILED;
        return;
    }
    //上传E的电量
    log_mqtt_set_battery_level(pdev->sn, (pdev->bat_level)*10);
    bdo_turn_on(pdev, 500, 5000, e_turn_on_cb, pdev);
    ble_dev_task_epcho();
}

//=================================================================================================
// WSS response callback
//=================================================================================================
static void wss_rsp_cbk(int type, odmp_package_t *pkg)
{
    os_mutex_pend(&g_request_mutex, 0);
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
        log_i("send_id: %s (%s)\n", pkg->tts_bind.send_id, g_edev_target_sn);
        log_i("wait_time: %d\n", pkg->tts_bind.wait_time);
        if (strlen(pkg->tts_bind.send_id)!= SN_SIZE) break;
        
        if (strncmp(g_edev_target_sn, pkg->tts_bind.send_id, SN_SIZE) != 0) {
            wss_ble_audio_stop();
            memcpy(g_edev_target_sn, pkg->tts_bind.send_id, SN_SIZE+1);
            turn_on_Pi(g_edev_target_sn);
            if (!g_mode_script) play_prompt_sound("TTSBind.mp3");
        } else if (g_mode_state == STATE_TTS_BIND) {
            //continue
        } else if (g_mode_state == STATE_DOWNLOAD || g_mode_state == STATE_PLAYING) {
            wss_ble_audio_stop();
            send_message = ORMET_SWITCH_SUCCESS;
        } else if (g_mode_state == STATE_IDLE || g_mode_state == STATE_WAITING) {
            turn_on_Pi(g_edev_target_sn);
            if (!g_mode_script) play_prompt_sound("TTSBind.mp3");
        }
        g_wss_last_time = g_curr_time;

        wait_time = pkg->tts_bind.wait_time;
        wss_request_update_state(STATE_TTS_BIND, "receive TTS_BIND");
    }   break;
    case OR_AUDIO_STATUS:
        log_i("AUDIO_STATUS\n");
        log_i("c_id: %s\n", pkg->audio_status.c_id);
        log_i("type: %s\n", odmp_ast_to_text(pkg->audio_status.type));
        log_i("event: %s\n", odmp_ase_to_text(pkg->audio_status.event));
        log_i("format: %s\n", pkg->audio_status.audio_param.format);
        log_i("sample_rate: %d\n", pkg->audio_status.audio_param.sample_rate);
        
        if (pkg->audio_status.type == OAST_ASR) {
            // if (pkg->audio_status.event == OASE_START) {
            //     last_message_type = OAST_ASR;
            //     last_event = OASE_START;
            // }
            // if (pkg->audio_status.event == OASE_END) {
            //     last_message_type = OAST_ASR;
            //     last_event = OASE_END;
            // }
        } else if (pkg->audio_status.type == OAST_TTS) {
            if (pkg->audio_status.event == OASE_START) {
                log_i("AUDIO_STATUS: OASE_START volume = %d\n", pkg->audio_status.volume);
                last_message_type = OAST_TTS;
                last_event = OASE_START;
                set_tts_volume(pkg->audio_status.volume);
            } else if (pkg->audio_status.event == OASE_END && g_curr_time - g_mode_last_time > 100) {
                log_i("AUDIO_STATUS: OASE_END\n");
                last_message_type = OAST_TTS;
                last_event = OASE_END;
            }
            g_wss_last_time = g_curr_time;
        } else if (pkg->audio_status.type == OAST_SCRIPT) {
            if (pkg->audio_status.event == OASE_START) {
                log_i("AUDIO_STATUS: SCRIPT START\n");
                g_mode_script = true;
                play_prompt_sound("script_st.mp3");
                wait_time = 0;
                wss_request_update_state(STATE_WAITING, "receive SCRIPT START");
            } else if (pkg->audio_status.event == OASE_END) {
                log_i("AUDIO_STATUS: SCRIPT END\n");
                g_mode_script = false;
                play_prompt_sound("script_ed.mp3");
                wss_request_update_state(STATE_IDLE, "receive SCRIPT END");
                wss_application_start_mic();// start vad
            }
            g_wss_last_time = g_curr_time;
        }
        break;
    case OR_ERROR_EVENT:
        log_i("ERROR_EVENT %d reason: %s\n", pkg->error.err_code, pkg->error.reason);
        if (pkg->error.err_code == 1001) {
            g_mode_script = false;
            play_prompt_sound("script_err.mp3");
            wss_handle_key_event();
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
    os_mutex_post(&g_request_mutex);
}

//=================================================================================================
// 请求中断相关接口
//=================================================================================================
void wss_request_stop()
{
    log_i("[wss_request]Stop WSS request\n");
    os_mutex_pend(&g_request_mutex, 0);

    if (g_mode_state == STATE_DOWNLOAD || g_mode_state == STATE_PLAYING || g_mode_state == STATE_TTS_BIND) {
        wss_ble_audio_stop();
        turn_off_Pi();
    }
    wss_request_send_event_target(ORMET_PLAY_STOP);

    // 重置状态到IDLE
    wss_request_update_state(STATE_IDLE, "wss_request_stop");
    last_message_type = 0;
    last_event = 0;
    g_mode_script = false;
    os_mutex_post(&g_request_mutex);
}

void wss_request_pause()
{
    log_i("[wss_request]Stop WSS pause\n");
    os_mutex_pend(&g_request_mutex, 0);
    odmp_register_rsp_cbk(NULL);

    if (g_mode_state == STATE_DOWNLOAD || g_mode_state == STATE_PLAYING || g_mode_state == STATE_TTS_BIND) {
        wss_ble_audio_stop();
        wss_request_send_event_target(ORMET_PLAY_STOP);
        turn_off_Pi();
    }

    // 重置状态到IDLE
    wss_request_update_state(STATE_IDLE, "wss_request_pause");
    last_message_type = 0;
    last_event = 0;
    g_mode_script = false;
    os_mutex_post(&g_request_mutex);
}

void wss_request_resume()
{
    odmp_register_rsp_cbk(wss_rsp_cbk);
}

//=================================================================================================
// 请求状态机初始化与循环接口
//=================================================================================================
void wss_request_init()
{
    log_i("[wss_request]Init WSS request\n");
    g_app_ctx = lwl_app_core_context_get_instance();
    /* 注册odmp事件回调 */
    g_edev_target_sn[0] = 0;

    g_mode_state = STATE_IDLE;
    g_curr_time = timer_get_ms();
    g_mode_last_time = g_curr_time;
    g_mode_script = false;
    wss_request_send_event_target(ORMET_INITIAL_SETUP);
    send_audio_format(OAST_TTS);
    if (!os_mutex_valid(&g_request_mutex)) {
        os_mutex_create(&g_request_mutex);
    }
    odmp_register_rsp_cbk(wss_rsp_cbk);
}

void wss_request_uninit()
{
    log_i("[wss_request]Uninit WSS request\n");
    /* 取消注册loswssp事件回调 */
    odmp_register_rsp_cbk(NULL);
    wss_request_stop();
}

bool wss_request_loop()
{
    // 使用 last_message_type 和 last_event 替代 last_audio_status_type、last_audio_status_event、last_tts_bind_received
    os_mutex_pend(&g_request_mutex, 0);
    g_curr_time = timer_get_ms();
    switch (g_mode_state) {
    case STATE_IDLE: // until receive TTS_BIND
        if (g_curr_time - g_wss_last_time > WSS_TIMEOUT) {
            send_ping();
            g_wss_last_time = g_curr_time;
        }
        break;
    case STATE_TTS_BIND: // until send_message == ORMET_SWITCH_SUCCESS or ORMET_SWITCH_FAILED
        // TTS下载流程
        if (send_message == ORMET_SWITCH_SUCCESS) {
            log_i("[wss_request]Switch success, send ORMET_SWITCH_SUCCESS");
            wss_request_send_event_target(send_message);
            wss_request_update_state(STATE_DOWNLOAD, "send ORMET_SWITCH_SUCCESS");
            send_message = 0; // 重置发送消息标志
        } else if (send_message == ORMET_SWITCH_FAILED) {
            wss_request_update_state(STATE_IDLE, "send ORMET_SWITCH_FAILED");
            wss_request_send_event_target(send_message);
            // g_edev_target_sn[0] = 0;
            send_message = 0; // 重置发送消息标志
            play_prompt_sound("SwFailed.mp3");
        } else if (g_curr_time - g_wss_last_time > WSS_TIMEOUT) {
            send_message = ORMET_SWITCH_FAILED;
        }
        break;

    case STATE_DOWNLOAD: // until receive AUDIO END
        // 下载结束，继续播放
        if (send_message == ORMET_PLAY_START) {
            wss_audio_playback_start();
            log_i("[wss_request]Switch success, starting audio");
            send_message = 0; // 重置发送消息标志
        } else if (last_message_type == OAST_TTS && last_event == OASE_START) {
            if (g_edev_connected_sn[0] != 0) {
                wss_ble_audio_start();
            }
            last_event = 0;
        }
        if (!is_audio_playing()) {
            if ((g_curr_time & 63) == 0) log_i("[wss_request]!is_audio_playing timeout %d > %d\n", g_curr_time - g_wss_last_time, WSS_TIMEOUT);
            if (g_curr_time - g_wss_last_time > WSS_TIMEOUT) {
                wss_request_stop();
                play_prompt_sound("SwFailed.mp3");
            }
        } else {
            g_wss_last_time = g_curr_time;
            if (last_message_type == OAST_TTS && last_event == OASE_END){
                wss_request_update_state(STATE_PLAYING, "receive AUDIO END");
            }
        }
        break;

    case STATE_PLAYING: // until player end
        // 播放结束，回到IDLE状态
        if (g_curr_time - g_mode_last_time > WSS_MUSIC_TIMEOUT) {
            if (!is_audio_playing()) {
                wss_request_update_state(STATE_IDLE, "[Decoder Play end cb]STEP_TO");
                wss_ble_audio_stop();
                turn_off_Pi();
            }
        } else {
            ble_dev_task_epcho();
        }
        break;

    case STATE_WAITING:
        if (g_curr_time - g_wss_last_time > WSS_TIMEOUT * 2) {
            g_mode_script = 0;
            wss_request_update_state(STATE_IDLE, "WSS_TIMEOUT");
        } else {
            wss_application_vailed();
        }
        break;
    }
    

    os_mutex_post(&g_request_mutex);
    return (g_mode_state == STATE_IDLE);
}

void wss_request_send_event(char *sn, int msg_event)
{
    g_wss_last_time = g_curr_time;
    if (sn != NULL && strlen(sn)!= SN_SIZE) {
        sn = NULL;
    }
    if (sn != NULL) {
        if (update_edev_time(sn) == false) {
            sn = NULL;
        }
    }
    send_req_message(sn, is_audio_playing(), msg_event);
}

void wss_request_send_event_target(int msg_event)
{
    wss_request_send_event(g_edev_target_sn, msg_event);
}