#include "mode_list.h"
#include "lwip.h"
#include "bt_ble/app_wifi_manage.h"
// 模式7 wifi相关变量
static int g_mode_7_wifi_checked = 0;
static int g_mode_7_last_wifi_status = -1;
static int g_mode_7_wifi_connected = 0;

bool mode7_is_wifi_connected()
{
    return g_mode_7_wifi_checked && g_mode_7_wifi_connected;
}

// 模式7音频播放回调函数
void mode7_audio_callback(void *priv)
{
    // 检查当前模式，如果不是模式5，则忽略回调
    if (get_current_mode() != WORK_MODE_7) {
        log_i("MODE 7: Audio callback ignored - not in mode 7 (current mode: %d)", get_current_mode());
        return;
    }
    
    log_i("MODE 7: Prompt audio callback - finished, starting wifi status monitoring");
    g_audio_playing = 0;
}
static int ip_is_ready() 
{
    struct netif_info g_netip_info = {0};
    char g_wifi_ip[16] = {'\0'};

    memset(&g_netip_info, 0, sizeof(g_netip_info));
    lwip_get_netif_info(1, &g_netip_info);

    if (g_netip_info.ip == 0) {
        printf("Wating...");
        return 0;
    }

    u32 temp_ip = htonl(g_netip_info.ip);
    snprintf(g_wifi_ip, 32, "%d.%d.%d.%d",
            (temp_ip >> 24) & 0xFF, // 第一字节
            (temp_ip >> 16) & 0xFF, // 第二字节
            (temp_ip >> 8) & 0xFF,  // 第三字节
            temp_ip & 0xFF          // 第四字节
    );
    printf("====================== wifi ip:%s #######################", g_wifi_ip);
    return 1;
}

void process_mode_7(void)
{
    // 等待NetConnting.mp3播放完成
    if (g_audio_playing) {
        return;
    }

    lwl_app_context_t *context = lwl_app_core_context_get_instance();

    enum wifi_sta_connect_state wifi_status = wifi_get_sta_connect_state();

    printf("net_state = %d\n", wifi_status);
        
    if ( wifi_status == WIFI_STA_CONNECT_TIMEOUT_NOT_FOUND_SSID ||
            wifi_status == WIFI_STA_CONNECT_ASSOCIAT_FAIL ||
            wifi_status == WIFI_STA_DISCONNECT ||
            wifi_status == WIFI_STA_CONNECT_ASSOCIAT_TIMEOUT ||
            wifi_status == WIFI_STA_NETWORK_STACK_DHCP_TIMEOUT) {
        // 连接失败
        if (!g_mode_7_wifi_checked || g_mode_7_wifi_connected) {
            g_mode_7_wifi_connected = 0;
            g_mode_7_wifi_checked = 1;
            ble_simple_server_name_enable(1);
            if (context && context->music_player) {
                g_audio_playing = 1;
                lwl_app_mode_play(CONFIG_CP_VOICE_FILE_PATH"NetCfgFail.mp3", mode7_audio_callback);
            }
            log_i("MODE 7: WiFi connect failed, playing NetCfgFail.mp3");
            return;
        }
    } else if ((wifi_status == WIFI_STA_CONNECT_SUCC && ip_is_ready()) || wifi_status == WIFI_STA_NETWORK_STACK_DHCP_SUCC) {
            // 连接成功
        if (!g_mode_7_wifi_checked || !g_mode_7_wifi_connected) {
            g_mode_7_wifi_connected = 1;
            g_mode_7_wifi_checked = 1;
            if (context && context->music_player) {
                g_audio_playing = 1;
                lwl_app_mode_play(CONFIG_CP_VOICE_FILE_PATH"NetCfgSucc.mp3", mode7_audio_callback);
            }
            log_i("MODE 7: WiFi connected, playing NetCfgSucc.mp3");
            return;
        }
    }

    // 成功后允许按键切换到WORK_MODE_8，否则停留
    // 这里不做自动切换，按键事件里判断
}


void mode7_init()
{
    wifi_and_network_on();
    ble_simple_server_init();

    g_mode_7_wifi_checked = 0;
    g_mode_7_wifi_connected = 0;
    g_mode_7_last_wifi_status = -1;
    lwl_app_mode_play(CONFIG_CP_VOICE_FILE_PATH"NetConnting.mp3", mode7_audio_callback);
    g_audio_playing = 1;
    log_i("MODE 7: WiFi connecting, playing NetConnting.mp3");
}

void mode7_uninit()
{
    ble_simple_server_exit();
}