#include "xiaozhi.h"
#include "log/log_mqtt.h"
#include "log.h"
#include "../lwl_app_framework.h"
#include "../lwl_app_core.h"
#include "net_event.h"
#include "wifi/wifi_connect.h"
#include "protocol/cp/c_uart_p.h"
#include "app_mode/wss_connected/wss_app_util.h"
#include "app_mode/wss_connected/wss_connected.h"
#include "protocol/odm/odmp.h"

extern u32 last_req_time;
static u32 g_curr_time;
bool xiaozhi_inited=false;
char *xiaozhi_session_id;
static void set_led_connected(void)         { uart_send_set_led_color(COLOR_WHITE); }  /* 控制 led : 联网成功 白色 */
static void set_led_connect_error(void)     { uart_send_set_led_color(COLOR_RED); }    /* 控制 led : 网络连接中 红色 */
static void set_led_idle(void)      { uart_send_set_led_color(COLOR_WHITE); }   //空闲时灯光变白色
bool xiaozhi_prompt_flag = 0;
static int wss_connected_unint(void)
{
    wss_application_uninit();//clear all pdev

    /* 退出wss_connected模式时，关闭WSS连接，释放资源 */
    wss_disconnect();
    return 0;
}
bool xiaozhi_init_status(){
    return xiaozhi_inited;
}
void set_xiaozhi_init_status(bool status){
    xiaozhi_inited=status;
}

bool xiaozhi_application_loop(){ 
    g_curr_time = timer_get_ms();
    if (wss_check_exit()) {
        return true;
    }
    // log_i("last_req_time %d,g_curr_time %d", last_req_time, g_curr_time);
    if(g_curr_time>last_req_time&&xiaozhi_init_status()&&!is_audio_playing()) {
        last_req_time = g_curr_time + 2000000;
        xiaozhi_send_msg(SEND_TEXT); // 通知服务器开始录音
        // wss_ble_audio_start();
        // wss_mic_start(last_req_time); 
    }
    if (wss_application_check_touch_key()) {
            set_led_idle();
            xiaozhi_send_msg(SEND_STOP);
            // wss_mic_stop(0);
        }
    msleep(10);
    return false;
}



void app_xiaozhi_mode(){
    wss_connected_init();//application init 和blk RSSI测距 
    // xiaozhi_prompt_flag = 1;
    
    xiaozhi_send_msg(HELLO);
    wss_ble_audio_start();
    // wss_ble_audio_start();
    while (1) {
        // 应用代码
        if (xiaozhi_application_loop()) {
            set_led_connect_error();// WSS ERROR
            if (wifi_get_sta_connect_state() == WIFI_STA_DISCONNECT) {
                lwl_app_mode_switch_to(APP_NET_CONNECTING_MODE);
                printf("Network connection is WIFI_STA_DISCONNECT!\n");
                play_system_music("NetCfgFail.mp3");
            } else if (wss_application_check_touch_key()) {
                if (!wss_reconnect()) {
                    lwl_app_mode_switch_to(APP_NET_CONNECTED_MODE);
                } else {
                    set_led_connected();
                    xiaozhi_prompt_flag = 1;
                }
            } else if (xiaozhi_prompt_flag) {
                xiaozhi_prompt_flag = 0;
                play_system_music("NetDisc.mp3");
            }
        }

        mdelay(10);

        if (lwl_app_mode_exitting()) {
            wss_connected_unint();
            return;
        }


    }
}