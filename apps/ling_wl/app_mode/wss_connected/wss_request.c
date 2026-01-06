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
static bool ready_to_play = false;

static char g_edev_target_sn[SN_SIZE+1]; // 服务器维护
static char g_edev_connected_sn[SN_SIZE+1]; // E 已连接
static char g_stream_id[64]; // 保存 stream_id


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
            ready_to_play = false;
            log_i("[wss_request]send_message = %d", send_message);
        } else {
            turn_off_Pi();
        }
    } else if (BDOCode_Ready_to_Play == code) {
        if (strcmp(sn_string,g_edev_target_sn) == 0) { // 连接的是目标E
            ready_to_play = true;
            log_i("[wss_request]send_message = %d", send_message);
        }
    } else {
        if (strcmp(sn_string,g_edev_target_sn) == 0) { // 连接的目标E失败
            send_message = ORMET_SWITCH_FAILED;
            log_i("【dg】[e_turn_on_cb]Connection failed for target device %s, code=%d, send_message=%d", 
                  sn_string, code, send_message);
        } else {
            log_i("【dg】[e_turn_on_cb]Connection failed for non-target device %s (target=%s), code=%d", 
                  sn_string, g_edev_target_sn, code);
        }
    }
    os_mutex_post(&g_request_mutex);
}

void turn_on_Pi(const char *pdev_sn) {
    send_message = 0; // 重置发送消息标志
    ready_to_play = false;
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
    bdo_turn_on(pdev, 1000, 5000, e_turn_on_cb, pdev);
    ble_dev_task_epcho();
}

//=================================================================================================
// WSS response callback
//=================================================================================================
static void wss_rsp_cbk(int type, odmp_package_t *pkg)
{
    os_mutex_pend(&g_request_mutex, 0);
    switch (type) {
    case OR_TTS_BIND: {
        log_i("TTS_BIND\n");
        log_i("sn: %s (%s)\n", pkg->tts_bind.sn, g_edev_target_sn);
        log_i("stream_id: %s\n", pkg->tts_bind.stream_id);
        log_i("role_volume: %d\n", pkg->tts_bind.role_volume);
        
        // 保存 stream_id 到全局变量
        if (pkg->tts_bind.stream_id != NULL) {
            strncpy(g_stream_id, pkg->tts_bind.stream_id, sizeof(g_stream_id) - 1);
            g_stream_id[sizeof(g_stream_id) - 1] = '\0';
        } else {
            g_stream_id[0] = '\0';
        }
        if (pkg->tts_bind.sn == NULL || strlen(pkg->tts_bind.sn) != SN_SIZE) break;
        // const char *stream_id = wss_request_get_stream_id();
        // if (stream_id != NULL && stream_id[0] != '\0') {
        //     wss_ble_audio_stop(stream_id);
        // } else {
        //     log_e("[wss_request]stream_id is empty, cannot stop audio");
        // }
        memcpy(g_edev_target_sn, pkg->tts_bind.sn, SN_SIZE+1);
        turn_on_Pi(g_edev_target_sn);
        play_prompt_sound("TTSBind.mp3");
    }   break;

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

    const char *stream_id = wss_request_get_stream_id();
    if (stream_id != NULL && stream_id[0] != '\0') {
        wss_ble_audio_stop(stream_id, false);
    } else {
        log_e("[wss_request]stream_id is empty, cannot stop audio");
    }
    turn_off_Pi();

    // 重置状态
    last_message_type = 0;
    last_event = 0;
    os_mutex_post(&g_request_mutex);
}

void wss_request_pause()
{

    log_i("[wss_request]Pause WSS request\n");
    os_mutex_pend(&g_request_mutex, 0);
    odmp_register_rsp_cbk(NULL);

    const char *stream_id = wss_request_get_stream_id();
    if (stream_id != NULL && stream_id[0] != '\0') {
        wss_ble_audio_stop(stream_id, false);
    } else {
        log_e("[wss_request]stream_id is empty, cannot stop audio");
    }
    turn_off_Pi();

    last_message_type = 0;
    last_event = 0;
    os_mutex_post(&g_request_mutex);
}
void wss_request_stop_audio()
{
    log_i("[wss_request]Stop WSS request audio\n");
    os_mutex_pend(&g_request_mutex, 0);
    const char *stream_id = wss_request_get_stream_id();
    if (stream_id != NULL && stream_id[0] != '\0') {
        wss_ble_audio_stop(stream_id, false);
    }
    turn_off_Pi();
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
    g_curr_time = timer_get_ms();
    g_mode_last_time = g_curr_time;

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
    // 只做一件事：turn_on_Pi 切换成功之后马上发送切换成功消息
    os_mutex_pend(&g_request_mutex, 0);
    
    if (send_message == ORMET_SWITCH_SUCCESS) {
        log_mqtt("💔💔💔 切换设备成功，开始播放音频");
        log_i("【dg】[wss_request_loop]ORMET_SWITCH_SUCCESS: g_edev_connected_sn=%s, g_stream_id=%s", 
              g_edev_connected_sn, g_stream_id);
        
        // 检查所有必要条件
        if (g_edev_connected_sn[0] == 0) {
            log_e("[wss_request_loop]ERROR: g_edev_connected_sn is empty, cannot start audio");
            send_message = 0;
            os_mutex_post(&g_request_mutex);
            return false;
        }
        
        if (g_stream_id[0] == '\0') {
            log_e("[wss_request_loop]ERROR: g_stream_id is empty, cannot start audio");
            send_message = 0;
            os_mutex_post(&g_request_mutex);
            return false;
        }
        
        wss_ble_audio_start(g_stream_id);
        // 发送 bind_result 消息
        send_bind_result(g_edev_connected_sn, true, "connected", g_stream_id);
        send_message = 0; // 重置发送消息标志
    } else if (send_message == ORMET_SWITCH_FAILED) {
        log_mqtt("💔💔💔 切换设备失败");
        log_i("【dg】[wss_request_loop]ORMET_SWITCH_FAILED: g_edev_target_sn=%s, g_stream_id=%s", 
              g_edev_target_sn, g_stream_id);
        
        // 连接失败，发送失败消息给服务器
        if (g_stream_id[0] != '\0' && g_edev_target_sn[0] != 0) {
            send_bind_result(g_edev_target_sn, false, "connection_failed", g_stream_id);
        } else {
            log_e("[wss_request_loop]ERROR: cannot send bind_result, g_edev_target_sn or g_stream_id is empty");
        }
       play_prompt_sound("SwFailed.mp3");
        // 清理状态
        g_edev_target_sn[0] = 0;
        g_edev_connected_sn[0] = 0;
        g_stream_id[0] = '\0';
        send_message = 0; // 重置发送消息标志
        
        // 关闭可能已连接的设备
        turn_off_Pi();
    }
    
    os_mutex_post(&g_request_mutex);
    return false;
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

const char *wss_request_get_stream_id(void)
{
    return g_stream_id;
}