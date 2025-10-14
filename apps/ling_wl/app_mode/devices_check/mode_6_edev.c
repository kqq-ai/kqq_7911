/**
 * @author luotianyu
 * @brief 
 * 
 * ── MODE_6 串行状态子状态机 ──
 * 
 *   +-------------------+
 *   |                   v
 * [IDLE] -> [PLAY_PROMPT] -> [CONNECT_1] -> [PLAY_1] -> [CONNECT_2] -> [PLAY_2]
 *    ^                                                      |
 *    +------------------------------------------------------+
 * 
 * 说明：
 *   - IDLE         ：空闲
 *   - PLAY_PROMPT  ：播放提示音
 *   - CONNECT_1    ：连接第1个E端设备
 *   - PLAY_1       ：第1设备连接成功后播放音频
 *   - CONNECT_2    ：连接第2个E端设备
 *   - PLAY_2       ：第2设备连接成功后播放音频
 *   - PLAY_2后回到CONNECT_1，循环
 */

#include "mode_list.h"
#include "gpio.h"
#include "bt_ble/client/ble_dev_task_scheduler.h"
 
static u8 g_mode6_playing_prompt = 1; // 模式6状态：1=播放提示音，0=循环播放测试音

// 模式6 E端设备相关变量
static int g_current_e_device_index = -1;      // 当前E端设备索引
lwl_eend_dev_t *target_pdev = NULL; // 当前目标E端设备指针
lwl_eend_dev_t *current_pdev = NULL; // 当前连接的E端设备指针
// ========== 模式6 串行状态机重构 ===========
typedef enum {
    MODE6_IDLE = 0,        // 空闲状态，未进行任何操作
    MODE6_PLAY_PROMPT,     // 播放提示音（如开机提示、操作提示）
    MODE6_CONNECT_1,       // 尝试连接第一个目标设备
    MODE6_PLAY_1,          // 第一个设备连接成功后，进入播放状态
    MODE6_CONNECT_2,       // 尝试连接第二个目标设备
    MODE6_PLAY_2,          // 第二个设备连接成功后，进入播放状态
    // 移除DISCONNECT状态 // 该状态已废弃，不再使用
} mode6_state_e;

static mode6_state_e g_mode6_state = MODE6_IDLE;
static int g_mode6_e_index = 0; // 0:第1设备, 1:第2设备
// 标志“模式6”相关动作是否正在进行
// 1：有动作在执行（如连接、播放等），防止重复或并发操作
// 0：无动作，允许推进新流程
static u8 g_mode6_action_in_progress = 0;


static void test_turn_off_callback(uint8_t code, void *priv)
{
    log_i("turn off result code: %d", code);
    current_pdev = NULL; // 清空当前连接设备指针
}
// ========== 模式6回调函数声明 ===========
static void mode6_e_connect_cb(bdo_code_e code, void *priv)
{
    lwl_eend_dev_t *pdev = (lwl_eend_dev_t*)(priv);
    char sn_string[SN_SIZE] = "(NULL)";
    if (pdev) {
        pdev->get_string(pdev, sn_string, sizeof(sn_string));
    }
    log_i("bdo_turn_on code %d %s\n", code, sn_string);

    if (BDOCode_Success == code) {
        current_pdev = pdev; // 设置当前连接的E端设备指针
    } else {
        if (pdev == target_pdev) target_pdev = NULL; // 连接失败，清空当前设备指针
        current_pdev = NULL; // 设置当前连接的E端设备指针
    }

    ///log_i("~~~~~~~~~~~    e_connect_cb,  No PLAY.");
    
    gpio_direction_output(IO_PORTA_00, 0);  //disable    C speaker

    // 播放结束后，先关闭当前已连接的E端设备（如果有）
    if (current_pdev) {
        char sn_string[SN_SIZE] = "(NULL)";
        current_pdev->get_string(current_pdev, sn_string, sizeof(sn_string));
        log_i("~~~~~~~~~~~    e_connect_cb,  No PLAY. 6: Turn OFF 2s delay & 3s  current E device: %s", sn_string);
        uint cmd_id;
        vTaskDelay(2000 / portTICK_PERIOD_MS);
        //mdelay(2000);
        //delay(2000);
        bdo_turn_off(current_pdev, 3000, test_turn_off_callback, current_pdev);
        target_pdev = NULL; // 清空目标设备指针
    }
    if (g_mode6_playing_prompt) {
        // 提示音播放完成，开始播放循环音频
        log_i("~~~~~~~~~~~    e_connect_cb,  No PLAY. 6: Prompt audio callback - finished, starting loop audio");
        g_mode6_playing_prompt = 0;
        g_audio_playing = 0;
    } else {
        // 循环音频播放完成，准备重新播放
        log_i("~~~~~~~~~~~    e_connect_cb,  No PLAY. 6: Loop audio callback - finished, will restart");
        g_audio_playing = 0;
    }
}
static void switch_to_e_target(lwl_eend_dev_t* pdev)
{
    if (pdev == NULL) return;
    char sn_string[SN_SIZE] = "(NULL)";
    pdev->get_string(pdev, sn_string, sizeof(sn_string));
    log_i("Switching to target: %s", sn_string);
    
    // 设置新的目标设备
    target_pdev = pdev;
    // -- pdev->handshake_info.need_handshake = true;
    bdo_turn_on(pdev, 9000, 9000, mode6_e_connect_cb, pdev); // E ON~  start_handshake
    log_i("+-- -+");
    // 参考default_e_turn_on函数中的命令发送方式
}


// 音频播放回调
void mode6_audio_callback(void *priv)
{
    // 检查当前模式，如果不是模式6，则忽略回调
    if (get_current_mode() != WORK_MODE_6) {
        log_i("MODE 6: Audio callback ignored - not in mode 6 (current mode: %d)", get_current_mode());
        return;
    }
    
    gpio_direction_output(IO_PORTA_00, 0);  //disable    C speaker

    // 播放结束后，先关闭当前已连接的E端设备（如果有）
    if (current_pdev) {
        char sn_string[SN_SIZE] = "(NULL)";
        current_pdev->get_string(current_pdev, sn_string, sizeof(sn_string));
        log_i("MODE 6: Turn OFF with delay 2s current E device: %s", sn_string);
        uint cmd_id;
        bdo_turn_off(current_pdev, 1000, test_turn_off_callback, current_pdev);
        target_pdev = NULL; // 清空目标设备指针
    }
    if (g_mode6_playing_prompt) {
        // 提示音播放完成，开始播放循环音频
        log_i("MODE 6: Prompt audio callback - finished, starting loop audio");
        g_mode6_playing_prompt = 0;
        g_audio_playing = 0;
    } else {
        // 循环音频播放完成，准备重新播放
        log_i("MODE 6: Loop audio callback - finished, will restart");
        g_audio_playing = 0;
    }
}

static void devices_check_cb(uint8_t code, void *priv)
{
    lwl_eend_dev_t *pdev = (lwl_eend_dev_t*)(priv);
    log_i("devices_check_cb  %s\n", pdev->sn);

    switch (code) {
    case BDOCode_Success:                //!< 操作成功通用返回码
        log_i("BDOCode_Success");
        break;
    case BDOCode_Skip:                   //!< 操作被跳过
        log_i("BDOCode_Skip");
        break;
    case BDOCode_Null_Pointer:           //!< 操作失败，空指针错误
        log_i("BDOCode_Null_Pointer");
        break;
    case BDOCode_BLE_Api_Error:          //!< 操作失败，BLE 接口调用失败
        log_i("BDOCode_BLE_Api_Error");
        break;
    case BDOCode_BLE_Buffer_Full:        //!< 操作失败，BLE 缓冲满了
        log_i("BDOCode_BLE_Buffer_Full");
        break;
    case BDOCode_Connection_Timeout:     //!< 操作失败，连接超时
        log_i("BDOCode_Connection_Timeout");
        break;
    case BDOCode_Scan_Adv_Stat_Timeout:  //!< 操作失败，扫描ADV状态变化超时
        log_i("BDOCode_Scan_Adv_Stat_Timeout");
        break;
    case BDOCode_Unexpected_Error:       //!< 意外错误
        log_i("BDOCode_Unexpected_Error");
        break;
    }
}

// 模式6处理函数
void process_mode_6(void)
{
    lwl_app_context_t *context = lwl_app_core_context_get_instance();
    
    // 如果不在播放提示音阶段，且当前没有音频在播放，则开始播放循环音频
    if (!g_mode6_playing_prompt && !g_audio_playing && (target_pdev == current_pdev && target_pdev != NULL)) {
        log_i("MODE 6: Starting loop audio playback");
        
        // 播放循环测试音频，使用回调
        set_dec_volume(context->music_player, 50); // 保护E的电池，需要设置最高默认音量为50
        int err = 0;
/*         if((g_mode6_e_index&1) == 0 ){
            err = app_music_play_local_music_with_callback(
            context->music_player, 
            CONFIG_CP_VOICE_FILE_PATH"test_dev1.mp3", 
            mode6_audio_callback, NULL);
        }else{
            err = app_music_play_local_music_with_callback(
            context->music_player, 
            CONFIG_CP_VOICE_FILE_PATH"test_dev2.mp3", 
            mode6_audio_callback, NULL);
        } */
        
            
        if (err == 0) {
            g_audio_playing = 1;
            log_i("MODE 6: Loop audio started successfully");
        } else {
            log_i("MODE 6: Loop audio start failed, error: %d", err);
        }
    }
    if (!target_pdev && !current_pdev) {
        // 没有目标设备，尝试设置一个
        if (pdev_list_count > 0) {
            switch_to_e_target(pdev_list[g_mode6_e_index]);
            g_mode6_e_index++;
            if (g_mode6_e_index >= pdev_list_count) g_mode6_e_index = 0;
        } else {
            log_i("MODE 6: No devices in pdev_list, cannot set target");
        }
    }
}

void mode6_init()
{
    gpio_direction_output(IO_PORTA_00, 1);  // 启用C端扬声器
    ble_simple_client_module_enable(1); // enable BLE client to connect E device
    g_mode6_state = MODE6_PLAY_PROMPT;
    g_mode6_e_index = 0;
    g_mode6_action_in_progress = 0;
    {
        lwl_app_context_t *context6 = lwl_app_core_context_get_instance();
        app_music_play_local_music_with_callback(
            context6->music_player,
            CONFIG_CP_VOICE_FILE_PATH"test_edev.mp3",
            mode6_audio_callback, NULL);
        g_mode6_action_in_progress = 1;
        g_mode6_playing_prompt = 1;
    }
    log_i("MODE 6: 串行状态机启动");
}

void mode6_uninit()
{
    log_i("Cleaning up MODE 6 audio functionality");
    ble_simple_client_module_enable(0);
    g_audio_playing = 0;
    g_mode6_playing_prompt = 0;
    g_mode6_action_in_progress = 0;  // 重置模式6动作进行标志
    g_mode6_state = MODE6_IDLE;      // 重置模式6状态机
    target_pdev = NULL;              // 清空目标设备指针
    current_pdev = NULL;             // 清空当前设备指针
    // 停止当前播放的音频
    lwl_app_context_t *context = lwl_app_core_context_get_instance();
    set_dec_volume(context->music_player, INIT_VOLUME_VALUE); // 恢复默认音量
    if (context && context->music_player) {
        // 这里可能需要调用停止音频的函数，但先尝试重置状态
        // app_music_stop(context->music_player);
    }
    log_i("MODE 6: All state variables reset");
}
