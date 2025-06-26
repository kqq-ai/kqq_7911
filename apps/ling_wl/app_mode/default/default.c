/**
 * @author chuxuanhuang
 * @brief 
 *   +----> IDLE
 *   |
 * POWER_ON -------> NET_CONNECTING <-------> NET_CONNECTED
 *   |                   ^    ^                     ^
 *   |                   |    |                     |
 *   +----> DEFAULT <----+    +---- WSS_CONNECTED <-+
 * 
 * POWER 上电模式
 * IDLE 空闲模式（无任务不代表低功耗）
 * DEFAULT 默认模式：
 *     默认开启BLE ADV，提供配网功能
 * NET_CONNECTING 连接模式
 *     尝试连接WIFI
 * NET_CONNECTED 已连接WIFI的模式
 *     尝试连接后端服务器
 * WSS_CONNECTED 后端服务器连接成功模式
 *     可以进行应用逻辑处理
 */
#include "default.h"
#include "../lwl_app_framework.h"
#include "log.h"
#include "bt_ble/server/ble_simple_server.h"
#include "bt_ble/client/ble_simple_client.h"
#include "app_config.h"
#include "protocol/cp/c_uart_p.h"

#include "bt_ble/client/ble_distance.h"
#include <sys_time.h>

static void default_play_music(const char *fname) 
{
    lwl_app_context_t *context = lwl_app_core_context_get_instance();
    
    // g_music_player.play_voice_prompt = 1;
    log_i("play: %s", fname);
    int err = app_music_play_local_music_with_callback(context->music_player, fname, NULL, NULL);
    if (err) {
        // key_event_enable();
        // g_music_player.play_voice_prompt = 0;
    }
}

//=================================================================================================
// pDev response callback
//=================================================================================================
static void default_cb(bka_cmd_code_e code, void *priv)
{
    lwl_eend_dev_t *pdev = (lwl_eend_dev_t*)(priv);
    log_i("default_cb\n");
    
    char sn_string[SN_SIZE * 2 + 1] = "(NULL)";
    if (pdev) {
        pdev->get_string(pdev, sn_string, sizeof(sn_string));
    }
    switch(code) {
    case BKA_CMD_CODE_COMPL:
        log_i("Switch PortE %s OK\n", sn_string);
        default_play_music(CONFIG_VOICE_PROMPT_FILE_PATH"NetCfgSucc.mp3");
        break;
    case BKA_CMD_CODE_DEVLOST:
        log_i("Switch PortE Dev Lost\n");
        break;
    case BKA_CMD_CODE_BUFFER_FULL:
        log_i("Switch PortE BLE TX Buffer Full\n");
        break;
    case BKA_CMD_CODE_SEARCH_PROFILE_TIMEOUT:
        log_i("Switch PoertE search GATT timeout\n");
        break;
    case BKA_CMD_CODE_E_TURN_ON_TIMEOUT:
        log_i("Switch PortE Timeout\n");
        break;
    case BKA_CMD_CODE_E_TURN_ON_ALREADY_BOUND:
        log_i("Switch PortE Failed: E is already bound\n");
        break;
    case BKA_CMD_CODE_UNEXPECTED:
        log_i("Switch PortE Error\n");
        break;
    }
    if (code != BKA_CMD_CODE_COMPL) {
    }
}


int default_e_turn_on(lwl_eend_dev_t *p_eend_dev, bka_cmd_cb_funp_t cb, void *priv, uint32_t *cmd_id_out) 
{
    ASSERT(p_eend_dev != NULL);
    lwl_app_context_t *ctx = lwl_app_core_context_get_instance();

    uart_send_set_sn(p_eend_dev->sn);

    u8 cmd_turn_on[] = {0xA0, 0x01, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    memcpy(cmd_turn_on + 2, ctx->mac, 6);

    return ble_keep_alive_device_push_cmd(p_eend_dev, cmd_turn_on, sizeof(cmd_turn_on), cb, priv, cmd_id_out);
}

lwl_eend_dev_t *pdev;

//=================================================================================================
// LWL event callback
//=================================================================================================
static void default_lwl_event_handler_cb(lwl_event_t *e)
{
    static int en = 1;
    lwl_eend_dev_t *p_eend_dev = e->distance_change.pdev;
    switch (e->event_type) {
    case LWL_EVENT_TYPE_EEND_DEV_CLOSEST_CHANGE: {
        log_i("LWL_EVENT_TYPE_EEND_DEV_CLOSEST_CHANGE %d %d", pdev, p_eend_dev);
        if (p_eend_dev != NULL) {
            char sn_string[SN_SIZE] = "(NULL)";
            p_eend_dev->get_string(p_eend_dev, sn_string, sizeof(sn_string));
            log_i("LWL_EVENT_TYPE_EEND_DEV_CLOSEST_CHANGE %s", sn_string);
        }
        if (pdev != p_eend_dev) {
            pdev = p_eend_dev;
            uint32_t cmd_id;
            int ret = default_e_turn_on(pdev, default_cb, pdev, &cmd_id);
            log_i("turn on cmd_id: %d", ret);
        }
    }   break;
    case LWL_EVENT_TYPE_EEND_DEV_DISTANCE_CHANGE: {
        char sn_string[SN_SIZE * 2 + 1] = "(NULL)";
        p_eend_dev->get_string(p_eend_dev, sn_string, sizeof(sn_string));
        log_i("[MEASDIST] %s: %d %d\r\n", sn_string, p_eend_dev->distance_info.rssi, p_eend_dev->distance_info.distance_type);
    }   break;
    case LWL_EVENT_TYPE_EEND_DEV_NEW_IN:
        log_i("ORMET_NEW_DEVICE_IN");
        break;
    case LWL_EVENT_TYPE_UART_IO_KEY_SHORT_CLICK: {
        log_i("SHORT_CLICK");

    }   break;
    case LWL_EVENT_TYPE_UART_SYNC:
        log_i("SYNC");
        log_i("battery: %d", e->uart_cmd_sync.battery);
        // if(e->uart_cmd_sync.battery < 30 )
        //     uart_send_set_led_color(6000, 10000, 10000); //red~ low power
        uart_response_synch_status(1, 12);/// 音量 E only
        break;
    case LWL_EVENT_TYPE_UART_TOUCH_KEY_DOWN:
        log_i("TOUCH KEY DOWN");
        break;
    case LWL_EVENT_TYPE_UART_SHUTDOWN:
        log_i("SHUTDOWN");
        break;
    case LWL_EVENT_TYPE_UART_MOVE_MORE_THAN_ONE_METER:
        log_i("MOVE_MORE_THAN_ONE_METER");
        break;
    }
}



int app_default_app_event_handler(struct application *app, struct sys_event *event)
{
    return false;
}

static int default_init(void)
{

    lwl_app_context_t *g_app_ctx = lwl_app_core_context_get_instance();
    

    msleep(5000);
    log_i("default_init");
    ble_simple_server_module_enable(0);
    ble_simple_client_module_enable(1);
    /* UART指令开启C端le audio */
    uart_send_open_le_audio(g_app_ctx->mac);

    /* 注册lwl事件处理回调 */
    lwl_event_handler_add(default_lwl_event_handler_cb);
    
    // simple_play_prompt(CONFIG_VOICE_PROMPT_FILE_PATH"NetCfgSucc.mp3");
    uart_send_set_led_color(COLOR_WHITE); /// on LED after power ON
    return 0;
}

static int default_unint(void)
{
    lwl_event_handler_remove(default_lwl_event_handler_cb);
    return 0;
}

#define HEAT_TIMEOUT 1000
void app_default_mode(void)
{
    default_init();
    static u32 g_heat_timer;
    g_heat_timer = timer_get_ms();

    while (1) {
        // 应用代码
        mdelay(10);

        u32 g_curr_time = timer_get_ms();

        if (g_curr_time - g_heat_timer >= HEAT_TIMEOUT) {
            dump_all_eend_info();
            g_heat_timer = g_curr_time;
        }

        if (lwl_app_mode_exitting()) {
            default_unint();
            return;
        }
    }
}