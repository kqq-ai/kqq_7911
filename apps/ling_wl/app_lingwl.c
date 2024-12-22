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

extern int app_music_bt_event_handler(struct sys_event *event);

static struct app_lingwl_hdl g_lingwl_hdl;

#define __this (&g_lingwl_hdl)

music_player g_music_player;

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

static void app_lingwl_main(void)
{
    log_i("Hello World!\r\n");

    // disable mute 
    gpio_direction_output(IO_PORTB_02, 1);

    music_player_init(&g_music_player);

    simple_play_prompt(CONFIG_VOICE_PROMPT_FILE_PATH"PowerOn.mp3");
}

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
        FILE *file = fopen(CONFIG_VOICE_PROMPT_FILE_PATH"PowerOn.mp3", "r");
        if (file) {
            g_music_player.play_voice_prompt = 1;
            err = local_music_dec_file(&g_music_player, file, 0, NULL, 0);
            if (err) {
                key_event_enable();
                g_music_player.play_voice_prompt = 0;
            }
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
        case NET_EVENT_CONNECTED:
            log_i("NET_EVENT_CONNECTED");
            simple_play_prompt(CONFIG_VOICE_PROMPT_FILE_PATH"NetCfgSucc.mp3");
            // dhcp ready = true
            // TODO: 网络连接成功，可在此时连接后端云服务器
            // TODO: 可配置WIFI和BLE共存时的RF共存参数设置
            break;
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
