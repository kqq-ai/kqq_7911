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

static u32 g_curr_time = 0;
static u32 g_wss_last_time = 0;
static OS_MUTEX g_request_mutex;

static lwl_app_context_t *g_app_ctx = NULL;
/*=================================================================================================
 * 请求状态
 *=================================================================================================*/

static char g_edev_target_sn[SN_SIZE+1]; // 服务器维护
static char g_edev_connected_sn[SN_SIZE+1]; // E 已连接
static char g_stream_id[64]; // 保存 stream_id
static int g_stream_id_len; // 保存 stream_id的长度
static u32 g_tts_bind_start_time = 0; // TTS_BIND开始时间，用于计算切换耗时


//=================================================================================================
// Pi设备切换 callback
//=================================================================================================
static const char* get_bdo_code_desc(bdo_code_e code) {
    switch(code) {
        case BDOCode_Skip:                   return "操作被跳过";
        case BDOCode_Unknown_Broadcast_Code: return "操作失败，广播码未知";
        case BDOCode_Null_Pointer:           return "操作失败，空指针错误";
        case BDOCode_BLE_Api_Error:          return "操作失败，BLE 接口调用失败";
        case BDOCode_BLE_Buffer_Full:        return "操作失败，BLE 缓冲满了";
        case BDOCode_Connection_Timeout:     return "操作失败，BLE 连接超时";
        case BDOCode_Disconnect_Timeout:     return "操作失败，BLE 断连超时";
        case BDOCode_Scan_Adv_Stat_Timeout:  return "操作失败，扫描ADV状态变化超时";
        case BDOCode_Dev_Busy:               return "操作失败，设备繁忙";
        case BDOCode_Unexpected_Error:       return "意外错误";
        default:                             return "未知错误";
    }
}

//=================================================================================================
// 关闭Pi内部函数
//=================================================================================================
static void turn_off_Pi_internal() {
    if (g_edev_connected_sn[0] != 0) {
        lwl_app_core_dev_map_lock();
        lwl_eend_dev_t *pdev = find_eend_dev_by_sn(g_edev_connected_sn);
        if (pdev) {
            bdo_turn_off(pdev);
        } else {
            log_i("[wss_request]turn_off_Pi find_eend_dev_by_sn %s NULL\n", g_edev_connected_sn);
        }
        lwl_app_core_dev_map_unlock();
        g_edev_connected_sn[0] = 0;
    }
}

//=================================================================================================
// 关闭Pi
//=================================================================================================
void turn_off_Pi() {
    os_mutex_pend(&g_request_mutex, 0);
    turn_off_Pi_internal();
    os_mutex_post(&g_request_mutex);
}

//=================================================================================================
// E 开启回调函数
//=================================================================================================
static void e_turn_on_cb(bdo_code_e code, void *priv, const char *sn)
{
    u32 callback_start_time = timer_get_ms();
    os_mutex_pend(&g_request_mutex, 0);
    // lwl_eend_dev_t *pdev = (lwl_eend_dev_t*)(priv); // 不再直接使用指针，可能已失效
    
    char sn_string[SN_SIZE + 1] = "(NULL)";
    if (sn) {
        strncpy(sn_string, sn, SN_SIZE);
        sn_string[SN_SIZE] = '\0';
    }
    
    switch(code) {
    case BDOCode_Success: {
        // 连接成功
        memcpy(g_edev_connected_sn, sn_string, SN_SIZE+1);
        if (strcmp(sn_string, g_edev_target_sn) == 0) { // 连接的是目标E
            // 计算从TTS_BIND到切换成功的耗时
            if (g_tts_bind_start_time != 0) {
                u32 duration = timer_get_ms() - g_tts_bind_start_time;
                log_mqtt("【flow】切换设备耗时: %u ms, esn=%s, stream_id=%s", 
                      duration, g_edev_connected_sn, g_stream_id);
                g_tts_bind_start_time = 0; // 清除时间戳
            }
            log_mqtt("【flow】发送bind_result成功消息给服务器: esn=%s, stream_id=%s", 
                    g_edev_connected_sn, g_stream_id);
            // 检查所有必要条件
            if (g_edev_connected_sn[0] == 0) {
                log_e("【flow】ERROR: g_edev_connected_sn is empty, cannot start audio");
                os_mutex_post(&g_request_mutex);
                return;
            }
            if (g_stream_id[0] == '\0') {
                log_e("【flow】ERROR: g_stream_id is empty, cannot start audio");
                os_mutex_post(&g_request_mutex);
                return;
            }
            // 发送 bind_result 消息
            send_bind_result(g_edev_connected_sn, true, "connected", g_stream_id);
        } else {
            // 连接的不是目标设备，关闭连接
            log_mqtt("[ERROR]【flow】连接的不是目标设备，关闭连接");
            turn_off_Pi_internal();
        }
        break;
    }
    
    case BDOCode_Ready_to_Play: {
        // 播放器准备完毕
        log_i("【flow】Switch PortE %s Ready_to_Play\n", sn_string);
        if (strcmp(sn_string, g_edev_target_sn) == 0) {
            log_mqtt("【flow】e端播放器准备完毕，播放线程开始");
            wss_ble_audio_start();
        }
        break;
    }
    
    default: {
        // 错误：所有其他错误情况
        const char* error_desc = get_bdo_code_desc(code);
        if (strcmp(sn_string, g_edev_target_sn) == 0) { // 连接的目标E失败
            log_mqtt("[ERROR]【flow】 ❌切换设备失败,E端无法连接: g_edev_target_sn=%s, g_stream_id=%s, code=%d (%s)", 
                  g_edev_target_sn, g_stream_id, code, error_desc);
            
            play_prompt_sound("SwFailed.mp3");
            
            // 清理状态
            g_edev_target_sn[0] = 0;
            g_edev_connected_sn[0] = 0;
            g_stream_id[0] = '\0';
            g_tts_bind_start_time = 0; // 清除时间戳
            
            // 关闭可能已连接的设备
            bdo_disable(0);
        } else {
            log_i("【flow】Connection failed for non-target device %s (target=%s), code=%d (%s)", 
                  sn_string, g_edev_target_sn, code, error_desc);
        }
        break;
    }
    }
    
    os_mutex_post(&g_request_mutex);
    
    u32 callback_end_time = timer_get_ms();
    u32 callback_duration = callback_end_time - callback_start_time;
    if (callback_duration > 30) {
        log_w("[e_turn_on_cb] Callback took %u ms (code: %d)", callback_duration, code);
    }
}

//=================================================================================================
// 开启Pi
//=================================================================================================
void turn_on_Pi(const char *pdev_sn) {
    os_mutex_pend(&g_request_mutex, 0);
    
    // 如果sn和g_edev_connected_sn 不相同则关闭之前的连接
    if (strcmp(g_edev_connected_sn, pdev_sn) != 0) {
        turn_off_Pi_internal();
    }
    
    lwl_app_core_dev_map_lock();
    lwl_eend_dev_t *pdev = find_eend_dev_by_sn(pdev_sn);
    if (pdev == NULL) {
        // log_mqtt("【flow】切换设备失败,设备不存在");
        log_mqtt("[ERROR]【flow】 ❌切换设备失败,E端不存在: g_edev_target_sn=%s, g_stream_id=%s", 
            g_edev_target_sn, g_stream_id);
        // send_bind_result(g_edev_target_sn, false, "device_not_found", g_stream_id);
        play_prompt_sound("SwFailed.mp3");
        // 清理状态
        g_edev_target_sn[0] = 0;
        g_edev_connected_sn[0] = 0;
        g_stream_id[0] = '\0';
        g_tts_bind_start_time = 0; // 清除时间戳
        lwl_app_core_dev_map_unlock();
        os_mutex_post(&g_request_mutex);
        return;
    }
    
    // 如果sn和已连接的相同，检查设备是否真的已连接
    if (strcmp(g_edev_connected_sn, pdev_sn) == 0) {
        // 检查设备是否已连接且是目标设备
        if (pdev->adv_state == BDPAS_CONNECTED && 
            strcmp(g_edev_target_sn, pdev_sn) == 0) {
            log_mqtt("【flow】设备已连接，跳过重复连接: esn=%s, stream_id=%s", 
                g_edev_connected_sn, g_stream_id);
            
            // 检查必要条件
            if (g_stream_id[0] != '\0') {
                // 上传E的电量
                log_mqtt_set_battery_level(pdev->sn, (pdev->bat_level)*10);
                // 发送 bind_result 消息
                send_bind_result(g_edev_connected_sn, true, "already_connected", g_stream_id);
                // 设备已连接，直接启动音频播放（相当于 Ready_to_Play 状态）
                log_mqtt("【flow】e端播放器已准备就绪，播放线程开始");
                wss_ble_audio_start();
            }
            
            lwl_app_core_dev_map_unlock();
            os_mutex_post(&g_request_mutex);
            return;
        }
        log_mqtt("debug】设备未连接或不是目标设备，继续执行连接流程");
        // 如果设备未连接或不是目标设备，继续执行连接流程
    }
    
    //上传E的电量
    log_mqtt_set_battery_level(pdev->sn, (pdev->bat_level)*10);
    
    // 释放锁后再调用 bdo_turn_on，避免死锁（回调函数会再次获取锁）
    lwl_app_core_dev_map_unlock();
    os_mutex_post(&g_request_mutex);
    
    //异步开启E，并等待回调结果
    bdo_turn_on(pdev, 1000, 5000, e_turn_on_cb, pdev);
}

//=================================================================================================
// WSS response callback 回调函数，实际处理wss请求的回调函数
//=================================================================================================
static void wss_rsp_cbk(int type, odmp_package_t *pkg)
{
    u32 callback_start_time = timer_get_ms();
    os_mutex_pend(&g_request_mutex, 0);
    log_i("[wss_request]Received WSS response type=%d\n", type);
    if (type == TTS_BIND) {
        g_stream_id_len = strlen(pkg->tts_bind.stream_id);
        strncpy(g_stream_id, pkg->tts_bind.stream_id, g_stream_id_len);
    }
    switch (type) {
    case TTS_BIND: {
        log_mqtt("【flow】收到TTS_BIND消息,开始切换设备: sn: %s (%s), stream_id: %s, role_volume: %d", 
            pkg->tts_bind.sn, g_edev_target_sn, pkg->tts_bind.stream_id, pkg->tts_bind.role_volume);  
        if (pkg->tts_bind.sn == NULL || strlen(pkg->tts_bind.sn) != SN_SIZE) {
            os_mutex_post(&g_request_mutex);
            break;
        }
        //此处拿到目标E的SN，保存到g_edev_target_sn
        memcpy(g_edev_target_sn, pkg->tts_bind.sn, SN_SIZE+1);
        g_tts_bind_start_time = timer_get_ms(); // 记录开始时间
        turn_on_Pi(g_edev_target_sn);
        play_prompt_sound("TTSBind.mp3");
        break;
    } 
    case AUDIO_STREAM_START: {
        log_mqtt("【flow】收到下放音频流开始信息，允许播放线程开始播放");
        wss_stream_start();
    } break;
    case AUDIO_STREAM_END: {
        log_mqtt("【flow】收到下放音频流结束信息，允许停止播放线程");
        wss_stream_stop();
    } break;


    }
    os_mutex_post(&g_request_mutex);
    
    u32 callback_end_time = timer_get_ms();
    u32 callback_duration = callback_end_time - callback_start_time;
    if (callback_duration > 150) {
        log_w("[wss_rsp_cbk] Callback took %u ms (type: %d)", callback_duration, type);
    }
}

//=================================================================================================
// 初始化WSS请求
//=================================================================================================
void wss_request_init()
{
    log_i("[wss_request]Init WSS request\n");
    g_app_ctx = lwl_app_core_context_get_instance();
    /* 注册odmp事件回调 */
    g_edev_target_sn[0] = 0;
    g_curr_time = timer_get_ms();
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
    //停止音频播放并关闭Pi
    wss_ble_audio_stop();
    //关闭Pi
    turn_off_Pi();
   
}

//=================================================================================================
// 发送WSS请求
//=================================================================================================

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