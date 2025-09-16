#include "mode_list.h"
#include "gpio.h"
#include "../../bt_ble/client/ble_distance.h"
#include "bt_ble/client/ble_dev_task_scheduler.h"

#include "../../protocol/cp/c_uart_p.h"

static void set_led_green(void)      { uart_send_set_led_color(COLOR_GREEN); }
static void set_led_blue(void)       { uart_send_set_led_color(COLOR_BLUE); }
static void set_led_white(void)      { uart_send_set_led_color(COLOR_WHITE); }
static void set_led_orange(void)     { uart_send_set_led_color(COLOR_ORANGE); }
 
// 模式9 E端设备相关变量
static lwl_eend_dev_t *g_mode9_current_pdev = NULL; // 模式9当前连接的E端设备指针
static u8 g_mode9_prompt_finished = 0;              // 模式9提示音是否播放完成

static void mode9_e_connect_cb(bdo_code_e code, void *priv);

// 模式9随机音频播放结束回调
void mode9_random_audio_end_cb(void *priv)
{
    if (get_current_mode() != WORK_MODE_9) {
        return;
    }
    log_i("MODE 9: Random audio playback finished on E-device.");
    g_audio_playing = 0;
    
    // 播放完成后，寻找RSSI值最大（信号最强）的设备
    lwl_eend_dev_t *strongest_pdev = NULL;
    int strongest_rssi = -128; // RSSI最小值，用作初始比较基准
    lwl_eend_dev_t *previous_pdev = g_mode9_current_pdev;
    int previous_rssi = -128;
    
    // 记录之前设备的RSSI值
    if (previous_pdev) {
        previous_rssi = previous_pdev->distance_info.rssi;
    }
    
    // 遍历所有设备，找到RSSI值最大的设备
    for (int i = 0; i < pdev_list_count; i++) {
        lwl_eend_dev_t *pdev = pdev_list[i];
        if (pdev && pdev->distance_info.rssi > strongest_rssi) {
            strongest_rssi = pdev->distance_info.rssi;
            strongest_pdev = pdev;
        }
    }
    
    // 断开当前E端设备
    if (g_mode9_current_pdev) {
        char sn_string[SN_SIZE] = "(NULL)";
        g_mode9_current_pdev->get_string(g_mode9_current_pdev, sn_string, sizeof(sn_string));
        log_i("MODE 9: Disconnecting E-device after playback: %s (RSSI: %d)", sn_string, previous_rssi);
        
        uint cmd_id;
        bdo_turn_off(g_mode9_current_pdev, 9000, NULL, NULL);
        g_mode9_current_pdev = NULL;
    }
    
    // 如果找到了信号更强的设备，立即连接并播放
    if (strongest_pdev && strongest_rssi > previous_rssi) {
        char sn_string[SN_SIZE] = "(NULL)";
        strongest_pdev->get_string(strongest_pdev, sn_string, sizeof(sn_string));
        log_i("MODE 9: Found stronger signal device after playback, switching to: %s (RSSI: %d -> %d)", 
            sn_string, previous_rssi, strongest_rssi);
        
        // 立即连接新设备
        uint cmd_id;
        bdo_turn_on(strongest_pdev, 9000, 9000, mode9_e_connect_cb, strongest_pdev);
        g_mode9_current_pdev = strongest_pdev;
        return; // 不重新启用C端扬声器，因为即将播放新音频
    }
    
    // 如果没有更强信号的设备，重新启用C端扬声器
    gpio_direction_output(IO_PORTA_00, 1);
    log_i("MODE 9: No stronger signal device found (current strongest RSSI: %d), C speaker re-enabled", strongest_rssi);
}


// 模式9 E端设备连接回调
static void mode9_e_connect_cb(bdo_code_e code, void *priv)
{
    lwl_eend_dev_t *pdev = (lwl_eend_dev_t*)(priv);
    char sn_string[SN_SIZE] = "(NULL)";
    if (pdev) {
        pdev->get_string(pdev, sn_string, sizeof(sn_string));
    }
    log_i("MODE 9: bdo_turn_on code %d %s", code, sn_string);

    if (BDOCode_Success == code) {
        // 连接成功，播放随机音频
        log_i("MODE 9: E-device connected successfully, playing random audio");
        gpio_direction_output(IO_PORTA_00, 0);  // 禁用C端扬声器，使用E端播放
        
        const char *audio_files[] = {
            CONFIG_VOICE_PROMPT_FILE_PATH"d_jd.MP3",
            CONFIG_VOICE_PROMPT_FILE_PATH"d_kk.MP3",
            CONFIG_VOICE_PROMPT_FILE_PATH"d_l.MP3"
        };
        int num_files = sizeof(audio_files) / sizeof(audio_files[0]);
        int random_index = rand() % num_files;
        const char *file_to_play = audio_files[random_index];

        log_i("MODE 9: Randomly selected audio for E-device: %s", file_to_play);

        lwl_app_context_t *context = lwl_app_core_context_get_instance();
        if (context && context->music_player) {
            g_audio_playing = 1;
            int err = app_music_play_local_music_with_callback(
                context->music_player,
                file_to_play,
                mode9_random_audio_end_cb, NULL);
            if (err != 0) {
                log_e("MODE 9: Failed to play random audio %s, error: %d", file_to_play, err);
                g_audio_playing = 0;
            } else {
                log_i("MODE 9: Started playing %s on E-device", file_to_play);
            }
        }
    } else {
        // 连接失败
        log_i("MODE 9: E-device connection failed");
        if (pdev == g_mode9_current_pdev) {
            g_mode9_current_pdev = NULL;
        }
    }
}


 // 模式9处理函数：E端设备距离检测并播放随机音频
void process_mode_9(void)
{
    // 如果提示音还在播放中，则等待播放完成
    if (g_audio_playing) {
        return;
    }

    // 提示音播放完成后，监控E端设备距离
    if (g_mode9_prompt_finished) {
        // 找到RSSI值最大（信号最强）的设备并设置对应的灯效
        lwl_eend_dev_t *strongest_pdev = NULL;
        int strongest_rssi = -128; // RSSI最小值，用作初始比较基准
        int closest_distance_type = DISTANCE_3; // 用于LED显示，基于最强信号设备的距离类型
        
        // 遍历所有设备，找到RSSI值最大的设备
        for (int i = 0; i < pdev_list_count; i++) {
            lwl_eend_dev_t *pdev = pdev_list[i];
            if (pdev && pdev->distance_info.rssi > strongest_rssi) {
                strongest_rssi = pdev->distance_info.rssi;
                strongest_pdev = pdev;
                closest_distance_type = pdev->distance_info.distance_type; // 更新LED显示依据
            }
        }
        
        // 根据最强信号设备的距离类型设置LED灯效
        switch (closest_distance_type) {
            case DISTANCE_0: // 小于2米 - 黄色
                set_led_orange();
                log_d("MODE 9: LED set to YELLOW - strongest device within 2m (RSSI: %d)", strongest_rssi);
                break;
            case DISTANCE_1: // 2米到5米 - 绿色  
                set_led_blue();
                log_d("MODE 9: LED set to GREEN - strongest device 2-5m away (RSSI: %d)", strongest_rssi);
                break;
            case DISTANCE_2: // 大于5米 - 灰色
                set_led_white();
                log_d("MODE 9: LED set to GRAY - strongest device >5m away (RSSI: %d)", strongest_rssi);
                break;
            case DISTANCE_3: // 无检测到或RSSI=-128 - 深蓝色
            default:
                set_led_green();
                log_d("MODE 9: LED set to DARK_BLUE - no device detected");
                break;
        }
        
        // 检查是否有RSSI更强的E端设备需要连接
        if (strongest_pdev) {
            // 比较当前设备和最强信号设备
            int current_rssi = -128;
            if (g_mode9_current_pdev) {
                current_rssi = g_mode9_current_pdev->distance_info.rssi;
            }
            int best_rssi = strongest_pdev->distance_info.rssi;
            
            if (g_mode9_current_pdev != strongest_pdev && best_rssi > current_rssi) {
                // 如果找到的设备不是当前设备，且RSSI更强，需要切换
                // 但是如果正在播放音频，则等待播放完成后再切换
                if (!g_audio_playing) {
                    if (g_mode9_current_pdev) {
                        // 先断开当前连接的设备
                        char current_sn[SN_SIZE] = "(NULL)";
                        g_mode9_current_pdev->get_string(g_mode9_current_pdev, current_sn, sizeof(current_sn));
                        log_i("MODE 9: Disconnecting current device: %s (RSSI: %d)", current_sn, current_rssi);
                        uint cmd_id;
                        bdo_turn_off(g_mode9_current_pdev, 9000, NULL, NULL);
                        g_mode9_current_pdev = NULL;
                    }
                    
                    // 连接信号更强的设备
                    char sn_string[SN_SIZE] = "(NULL)";
                    strongest_pdev->get_string(strongest_pdev, sn_string, sizeof(sn_string));
                    log_i("MODE 9: Connecting to stronger signal device: %s (RSSI: %d -> %d)", 
                        sn_string, current_rssi, best_rssi);
                    
                    uint cmd_id;
                    bdo_turn_on(strongest_pdev, 9000, 9000, mode9_e_connect_cb, strongest_pdev);
                    g_mode9_current_pdev = strongest_pdev;
                } else {
                    log_i("MODE 9: Audio playing, waiting for completion before switching to stronger signal device");
                }
            }
        }
    }
}
 
// 模式9音频播放回调函数
static void mode9_audio_callback(void *priv)
{
    if (get_current_mode() != WORK_MODE_9) {
        log_i("MODE 9: Audio callback ignored - not in mode 9 (current mode: %d)", get_current_mode());
        return;
    }

    log_i("MODE 9: Prompt audio callback - finished, starting E-device monitoring");
    g_audio_playing = 0;
    g_mode9_prompt_finished = 1; // 标记提示音播放完成，开始监控E端设备
}

void mode9_init()
{
    ble_simple_client_module_enable(1); // enable BLE client to connect E device
    g_audio_playing = 1;
    g_mode9_prompt_finished = 0;            // 重置提示音状态
    g_mode9_current_pdev = NULL;            // 重置连接设备指针
    log_i("MODE 9: E-device distance detection mode started, C speaker enabled");
    // 播放进入模式9的提示音，使用回调
    lwl_app_context_t *context8 = lwl_app_core_context_get_instance();
    app_music_play_local_music_with_callback(
        context8->music_player,
        CONFIG_VOICE_PROMPT_FILE_PATH"test_random.mp3",
        mode9_audio_callback, NULL);
    log_i("Playing test_random.mp3 for MODE 9 with callback");
}

void mode9_uninit()
{
    log_i("Cleaning up MODE 9 E-device functionality");
    ble_simple_client_module_enable(0);
    g_audio_playing = 0;
    g_mode9_prompt_finished = 0;
    // 断开当前连接的E端设备
    if (g_mode9_current_pdev) {
        char sn_string[SN_SIZE] = "(NULL)";
        g_mode9_current_pdev->get_string(g_mode9_current_pdev, sn_string, sizeof(sn_string));
        log_i("MODE 9: Disconnecting E-device during cleanup: %s", sn_string);
        uint cmd_id;
        bdo_turn_off(g_mode9_current_pdev, 9000, NULL, NULL);
        g_mode9_current_pdev = NULL;
    }
    log_i("MODE 9: All state variables reset");
}