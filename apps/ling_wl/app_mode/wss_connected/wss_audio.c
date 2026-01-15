#include "wss_audio.h"
#include "wss_app_util.h"
#include "protocol/odm/odmp.h"
#include "wss_request.h"

#include "app_mode/lwl_app_core.h"
#include "app_mode/lwl_app_audio.h"
#include "os/os_api.h"

static lwl_app_context_t *g_app_ctx = NULL;

/**
 * @brief 初始化WSS音频模块
 * @details
 *   该模块现在基于统一的音频管理模块(lwl_app_audio)实现。
 *   初始化前级设备(7016)的电源。
 */
void wss_audio_init()
{
    log_i("[AUDIO]Init WSS audio\n");
    g_app_ctx = lwl_app_core_context_get_instance();
    lwl_peri_dev_7016_on(true);  // 开启 7016
}

/**
 * @brief 反初始化WSS音频模块
 * @details
 *   停止流式音频播放，关闭前级设备(7016)的电源。
 */
void wss_audio_uninit()
{
    log_i("[AUDIO]Uninit WSS audio\n");
    wss_ble_audio_stop();
    lwl_peri_dev_7016_on(false);  // 关闭 7016
}

/**
 * @brief 开始蓝牙音频播放
 * @details
 *   启动流式音频播放，通过统一的音频管理模块实现。
 *   会自动关闭任何正在进行的本地文件播放。
 */
void wss_ble_audio_start()
{
    log_i("[AUDIO]wss_ble_audio_start");
    log_mqtt("❤️‍🔥❤️‍🔥❤️‍🔥 开始蓝牙音频播放");
    
    // 使用统一的音频管理模块启动流式播放
    lwl_app_audio_stream_start();
}

/**
 * @brief 停止蓝牙音频播放
 * @details
 *   停止流式音频播放，通过统一的音频管理模块实现。
 */
void wss_ble_audio_stop()
{
    log_i("[AUDIO]wss_ble_audio_stop");
    log_mqtt("💔💔💔 停止蓝牙音频播放");
    
    // 使用统一的音频管理模块停止流式播放
    lwl_app_audio_stream_stop();
    wss_stream_reset();
    
    send_device_status(DEVICE_STATUS_BLE_AUDIO_STOP);
}

/**
 * @brief 获取当前音频播放状态
 * @details
 *   检查是否有音频正在播放（包括本地文件和流式播放）
 * @return true 正在播放，false 未播放或已停止
 */
bool is_audio_playing()
{
    return lwl_app_audio_is_playing();
}