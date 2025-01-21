#include "app_config.h"
#include "app_lingwl.h"

#include "cpu.h"
#include "app_core.h"
#include "action.h"
#include "key_event.h"
#include "device_event.h"
#include "net_event.h"
#include "bt_event.h"
#include "log.h"
#include "net_music_play_my.h"
#include "gpio.h"

#include "bt_ble/ble_combined.h"
#include "bt_ble/server/ble_simple_server.h"
#include "bt_ble/client/ble_simple_client.h"
#include "uart.h"
#include "syscfg_id.h"
#include "audio/recorder.h"
#include "util/cqueue.h"
#include "async_msg_loop.h"
#include "wait.h"

#include "app_mode/lwl_app_framework.h"

#include "app_mode/lwl_app_core.h"

#include "app_lfs.h"

extern int app_music_bt_event_handler(struct sys_event *event);

static struct app_lingwl_hdl g_lingwl_hdl;

#define __this (&g_lingwl_hdl)

music_player g_music_player;
recorder_hdl g_recorder;
void *uart0_hdl = NULL;
uint8_t en_data_0[] = { // 0xA0, "WCCN24H28FEEEF7"
    0xA0, 
    0x57, 0x43, 0x43, 0x4E, 0x32, 0x34, 0x48, 0x32, 
    0x38, 0x46, 0x45, 0x45, 0x45, 0x46, 0x37, 0x00
};
uint8_t en_data_1[] = { // 0xA0, "WCCN24H28FEEEF8"
    0xA0, 
    0x57, 0x43, 0x43, 0x4E, 0x32, 0x34, 0x48, 0x32, 
    0x38, 0x46, 0x45, 0x45, 0x45, 0x46, 0x38, 0x00
};
uint8_t en_data_2[] = { // 0xA0, "WCCN24H28FEEEF9"
    0xA0, 
    0x57, 0x43, 0x43, 0x4E, 0x32, 0x34, 0x48, 0x32, 
    0x38, 0x46, 0x45, 0x45, 0x45, 0x46, 0x39, 0x00
};

static void simple_play_prompt(const char *fname) 
{
    FILE *file = fopen(fname, "r");
    if (file) {
        g_music_player.play_voice_prompt = 1;
        int err = local_music_dec_file(&g_music_player, file, 0, NULL, 0);
        if (err) {
            key_event_enable();
            g_music_player.play_voice_prompt = 0;
        }
    }
}

static void wifi_monitor_thread(void *priv)
{
    while (true) {
        log_i("WIFI U= %d KB/s, D= %d KB/s\r\n", wifi_get_upload_rate() / 1024, wifi_get_download_rate() / 1024);
        mdelay(1000);
    }
}

static void lwl_app_task(void *priv)
{
    log_i("lwl_app_task");
    lwl_app_init(APP_POWER_ON_MODE);

    // 初始化录音
    int ret = recorder_mode_init(&g_recorder);
    log_i("ret: %d\r\n", ret);

    lwl_app_context_t *context = lwl_app_core_context_get_instance();
    context->recorder = &g_recorder;
    context->uartdev = uart0_hdl;

    while (true) {
        lwl_app_mode_loop();
    }
}


static void app_lingwl_main(void)
{
    log_i("Hello World2!\r\n");
    log_i("%s %s\r\n", __DATE__, __TIME__);
    
    uart0_hdl = dev_open("uart0", NULL);
    if (!uart0_hdl) { 
        printf("open uart err !!!\n");
        return ;
    }

    /* 使能特殊串口,启动收发数据 */
    dev_ioctl(uart0_hdl, UART_START, 0);

    // 取消静音
    gpio_direction_output(IO_PORTA_00, 1);   // 
    // gpio_direction_output(IO_PORTB_02, 1);  // 

    music_player_init(&g_music_player);

    simple_play_prompt(CONFIG_VOICE_PROMPT_FILE_PATH"PowerOn.mp3");

    start_async_msg_loop();

    // thread_fork("wifi_monitor", 15, 512 * 3, 0, 0, wifi_monitor_thread, NULL);

    // 初始化lfs
    lfs_t *lfs_hdl = app_lfs_init("mnt/sdfile/app/exif");
    ASSERT(lfs_hdl);

    thread_fork("lwl_app_task", 15, 512 * 3, 0, 0, lwl_app_task, NULL);
}

void play_end_callback(void *priv)
{
    log_i("play end\n");
}

static u8 en = 1;
static int app_music_key_click(struct key_event *key)
{
    int err = 0;
    switch (key->value) {
    // case KEY_OK:
    //     break;
    case KEY_1:
        log_i("KEY 1\n");
        app_music_play_net_music(&g_music_player, "https://www.ihaoge.net/server/1/426045276.mp3");
        break;
    case KEY_2:
        log_i("KEY 2\n");
        app_music_play_net_music_with_callback(&g_music_player, "https://www.ihaoge.net/server/1/263523744.mp3", play_end_callback, NULL);
        break;
    case KEY_3:
        log_i("KEY 3\n");
        // 指定设备序列号
        // 通知C2将序列号设置到广播中
        // E端设备检查序列号是否与自己匹配，匹配则播放，不匹配则停止播放
        log_i("en = %d\n", en);

        en++;
        if (en == 3) {
            en = 0;
        }

        if (en == 0) {
            dev_write(uart0_hdl, en_data_0, sizeof(en_data_0));
        } else if (en == 1) {
            dev_write(uart0_hdl, en_data_1, sizeof(en_data_1));
        } else if (en == 2) {
            dev_write(uart0_hdl, en_data_2, sizeof(en_data_2));
        }
        break;
    case KEY_4:
        log_i("KEY 4\n");
        app_music_play_stop(&g_music_player);
        break;
    case KEY_OK:
        log_i("KEY OK\n");
        break;
        
    case KEY_VOLUME_DEC:
    case KEY_UP:
        if (0 == set_dec_volume(&g_music_player, -VOLUME_STEP)) {
#ifdef CONFIG_STORE_VOLUME
            syscfg_write(CFG_MUSIC_VOL, &g_music_player.volume, sizeof(g_music_player.volume));
#endif
        }
        break;
    case KEY_VOLUME_INC:
    case KEY_DOWN:
        if (0 == set_dec_volume(&g_music_player, VOLUME_STEP)) {
#ifdef CONFIG_STORE_VOLUME
            syscfg_write(CFG_MUSIC_VOL, &g_music_player.volume, sizeof(g_music_player.volume));
#endif
        }
        break;
    default:
        break;
    }
    return false;
}

static int app_music_key_hold(struct key_event *key)
{
    switch (key->value) {
    // case KEY_OK:
    //     break;
    default:
        break;
    }
    return false;
}

static int app_music_key_up(struct key_event *key)
{
    switch (key->value) {
    // case KEY_OK:
    //     break;
    default:
        break;
    }
    return false;
}

static int app_music_key_long(struct key_event *key)
{
    switch (key->value) {
    // case KEY_OK:
    //     break;
    default:
        break;
    }
    return false;
}

static int app_lingwl_key_event_handler(struct key_event *key)
{
    int ret = false;
    switch (key->action) {
    case KEY_EVENT_CLICK:
        ret = app_music_key_click(key);
        break;
    case KEY_EVENT_LONG:
        ret = app_music_key_long(key);
        break;
    case KEY_EVENT_HOLD:
        ret = app_music_key_hold(key);
        break;
    case KEY_EVENT_UP:
        ret = app_music_key_up(key);
        break;
    default:
        break;
    }
    return ret;
}

/*
 *设备响应函数
 */
static int app_lingwl_device_event_handler(struct sys_event *sys_eve)
{
    struct device_event *device_eve = (struct device_event *)sys_eve->payload;

    if (sys_eve->from == DEVICE_EVENT_FROM_POWER) {
    }
    return false;
}

static int app_music_net_event_handler(struct net_event *event)
{
    log_i("app_music_net_event_handler");
    if (!ASCII_StrCmp(event->arg, "net", 4)) {
        switch (event->event) {
        case NET_CONNECT_TIMEOUT_NOT_FOUND_SSID:
            log_i("NET_CONNECT_TIMEOUT_NOT_FOUND_SSID");
            // TODO: 可能在配网时需要发送ble通知
            break;
        case NET_CONNECT_ASSOCIAT_FAIL:
            log_i("NET_CONNECT_ASSOCIAT_FAIL");
            // TODO: 可能在配网时需要发送ble通知
            break;
        case NET_EVENT_SMP_CFG_FIRST:
            log_i("NET_EVENT_SMP_CFG_FIRST");
            break;
        case NET_EVENT_SMP_CFG_FINISH:
            log_i("NET_EVENT_SMP_CFG_FINISH");
            // TODO: 可能在配网时需要断开ble并连接指定wifi
            break;
        case NET_EVENT_CONNECTED: {
            log_i("NET_EVENT_CONNECTED");
            simple_play_prompt(CONFIG_VOICE_PROMPT_FILE_PATH"NetCfgSucc.mp3");
            // dhcp ready = true
            // TODO: 网络连接成功，可在此时连接后端云服务器
            // TODO: 可配置WIFI和BLE共存时的RF共存参数设置

            int index_before = get_rf_coexistence_config_index();
            switch_rf_coexistence_config_table(3);
            int index_after = get_rf_coexistence_config_index();

            log_i("rf_coexistence_config_index %d -> %d\n", index_before, index_after);
        }   break;
        case NET_EVENT_DISCONNECTED:
            log_i("NET_EVENT_DISCONNECTED\n");
            // dhcp ready = false
            // TODO: 可能需要进入回连状态，或者重新进入配网状态
            break;
        case NET_EVENT_SMP_CFG_TIMEOUT:
            log_i("NET_EVENT_SMP_CFG_TIMEOUT\n");
            break;
        case NET_SMP_CFG_COMPLETED:
            log_i("NET_SMP_CFG_COMPLETED\n");
            break;
        case NET_EVENT_DISCONNECTED_AND_REQ_CONNECT:
            log_i("NET_EVENT_DISCONNECTED_AND_REQ_CONNECT\n");
            break;
        case NET_NTP_GET_TIME_SUCC:	//NTP获取成功事件返回
            log_i("NET_NTP_GET_TIME_SUCC\n");
            break;
        default:
            break;
        }
    }
    return false;
}

static int app_lingwl_event_handler(struct application *app, struct sys_event *event)
{
    lwl_app_event_handler(app, event);
    switch (event->type) {
    case SYS_KEY_EVENT:
        return app_lingwl_key_event_handler((struct key_event *)event->payload);
    case SYS_DEVICE_EVENT:
        return app_lingwl_device_event_handler(event);
    case SYS_NET_EVENT:
        return app_music_net_event_handler((struct net_event *)event->payload);
    case SYS_BT_EVENT:
        return ble_demo_bt_event_handler(event);
    default:
        return false;
    }
}

static int app_lingwl_state_machine(struct application *app, enum app_state state,
                                   struct intent *it)
{
    switch (state) {
    case APP_STA_CREATE:
        break;
    case APP_STA_START:
        if (!it) {
            break;
        }
        switch (it->action) {
        case ACTION_LING_WL_MAIN:
            app_lingwl_main();
            break;
        }
        break;
    case APP_STA_PAUSE:
        break;
    case APP_STA_RESUME:
        break;
    case APP_STA_STOP:
        break;
    case APP_STA_DESTROY:
        break;
    }

    return 0;
}

static const struct application_operation app_lingwl_ops = {
    .state_machine  = app_lingwl_state_machine,
    .event_handler 	= app_lingwl_event_handler,
};

REGISTER_APPLICATION(app_lingwl) = {
    .name 	= "app_lingwl",
    .ops 	= &app_lingwl_ops,
    .state  = APP_STA_DESTROY,
};
