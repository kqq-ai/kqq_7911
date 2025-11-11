#include "app_config.h"
#include "app_cookie.h"

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
#include "util/threadpool.h"
#include "wait.h"
#include "app_mode/7911_uart_7016/app_uart_task.h"
#include "SC7A20/app_sc7a20_task.h"
#include "protocol/cp/c_uart_p.h"
#include "app_mode/lwl_app_framework.h"

#include "protocol/odm/odm_secure.h"
#include "app_mode/lwl_app_core.h"

#include "bt_ble/client/ble_distance.h"
#include "bt_ble/client/ble_dev_task_scheduler.h"
#include "log/log_mqtt.h"

static void set_led_red_low_power(void)        { uart_send_set_led_color(COLOR_RED); } //red~ low power
static void set_led_power_off(void)      { uart_send_set_led_color(COLOR_BLACK); } /// off LED before power Off
static void set_led_power_on(void)      { uart_send_set_led_color(COLOR_BLUE); }

extern int app_music_bt_event_handler(struct sys_event *event);

static struct app_cookie_hdl g_cookie_hdl;

#define __this (&g_cookie_hdl)

music_player g_music_player;
recorder_hdl g_recorder;
threadpool_t *pool = NULL;

void *uart0_hdl = NULL;
//"000000000000000"
const u8 ns_null[SN_SIZE] = { 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x00 };
//"WCCN24H28FEEEFC"
const u8 sn1[SN_SIZE] = { 0x57, 0x43, 0x43, 0x4E, 0x32, 0x34, 0x48, 0x32, 0x38, 0x46, 0x45, 0x45, 0x45, 0x46, 0x37, 0x00 };
//"WCCN24H28FEEEFD"
const u8 sn2[SN_SIZE] = { 0x57, 0x43, 0x43, 0x4E, 0x32, 0x34, 0x48, 0x32, 0x38, 0x46, 0x45, 0x45, 0x45, 0x46, 0x38, 0x00 };
//"WCCN24H28FEEEFE"
const u8 sn3[SN_SIZE] = { 0x57, 0x43, 0x43, 0x4E, 0x32, 0x34, 0x48, 0x32, 0x38, 0x46, 0x45, 0x45, 0x45, 0x46, 0x00, 0x00 };

static void simple_play_prompt(const char *fname) 
{
    // g_music_player.play_voice_prompt = 1;
    log_i("play: %s", fname);
    int err = app_music_play_local_music_with_callback(&g_music_player, fname, NULL, NULL);
    if (err) {
        // key_event_enable();
        // g_music_player.play_voice_prompt = 0;
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
    lwl_app_init();
    while (true) {
        lwl_app_mode_loop();
    }
    lwl_app_uninit();
}

// 关机提示音播放完成后的回调：播放结束再触发关机事件
static void power_off_cb(void *priv)
{
    log_i("----------------------BYE-------------------------\r\n");
    // 播放完成后再灭灯，防止对侧熄灯动作顺带关闭功放导致听不到提示音
    set_led_power_off();
    power_set_soft_poweroff();
}

int lastbattery=0;
static void do_shutdown()
{
    int err = -1;
    lwl_app_mode_switch_to(APP_DEFAULT_MODE);
    lwl_app_context_t *context = lwl_app_core_context_get_instance();
    if (context && context->music_player) {
        log_i("Try play PowerOff prompt before shutdown\r\n");
        msleep(100);
        gpio_direction_output(IO_PORTA_00, 1);  // 设置C出声
        err = app_music_play_local_music_with_callback(
                context->music_player,
                CONFIG_CP_VOICE_FILE_PATH"PowerOff.mp3",
                power_off_cb,
                NULL);
    }

    if (err < 0) {
        // 没有播放器或播放失败，直接进入关机流程
        log_w("PowerOff prompt play failed or unavailable, shutdown immediately\r\n");
        power_off_cb(NULL);
    }
}


void base_lwl_event_handler(lwl_event_t *e)
{
    switch (e->event_type) {
    case LWL_EVENT_TYPE_UART_SHUTDOWN:
        log_i("base_lwl_event_handler LWL_EVENT_TYPE_UART_SHUTDOWN\r\n");
        do_shutdown();
        break;
    case LWL_EVENT_TYPE_UART_TOUCH_KEY_DOWN:
        if (e->uart_cmd_touch.touch_key_event_type == 0) {
            log_i("Touch key pressed down");
        } else {
            log_i("Touch key released");
        }
        // play 提示音
        break;
    case LWL_EVENT_TYPE_UART_IO_KEY_SHORT_CLICK:
        log_i("PRESS NORMAL KEY");
        break;
    case LWL_EVENT_TYPE_UART_CLEAR_WIFI_SSID:
        log_i("PRESS RESET KEY");
        // Reset WiFi SSID
        app_del_stored_wifi_ssid(); // 清除设备保存过的所有wifi
        uart_response_clear_wifi_ssid();
        // uart_send_apply_soft_poweroff(); // TODO restart
        system_reset(); //restart
        break;
    case LWL_EVENT_TYPE_UART_SYNC:
        log_i("7016 SYNC battery: %d", e->uart_cmd_sync.battery);
        if(lastbattery != e->uart_cmd_sync.battery) {
            lastbattery = e->uart_cmd_sync.battery;
            log_mqtt_set_battery_level(NULL, lastbattery);
        }
        if(e->uart_cmd_sync.battery < 30 ) {
            set_led_red_low_power();
        }
        if (e->uart_cmd_sync.charging == 1) {
            // log_mqtt_set_battery_level(NULL, lastbattery); // 上报充电状态
        }
        break;
    case LWL_EVENT_TYPE_UART_NOTIFY_GSENSOR_OFF:
        log_i("NOTIFY_GSENSOR_OFF");
        extern void * g_iic_hdl;
        int ret = 0;
        ret = sc7a20h_disable(g_iic_hdl);
        if (0 == ret)
        {
            uart_response_notify_gsensor_off();
            log_i("GSENSOR OFF");
        }
    }
}


static void moni_task(void *priv)
{
    while (1) {
        u32 vaild_len = 0;
        ble_op_multi_att_get_remain(client_con_handle, &vaild_len);
        log_i("moni valid_len: %d", vaild_len);           
        msleep(1000);
    }
}
    
#include "util/led_breath.h"

static void* led_breath_thread(void* arg)
{
    quick_test_with_macros(20); // 直接在线程里调用呼吸灯测试函数,20次循环
    return NULL;
}


static void app_cookie_main(void)
{
    log_i("Hello World2!\r\n");
    log_i("Version: 12\r\n");
    log_i("%s %s\r\n", __DATE__, __TIME__);

    lwl_app_core_init();
    
    lwl_app_context_t *context = lwl_app_core_context_get_instance();

    lwl_event_handler_add(base_lwl_event_handler);

    // led_breath_init(); // 初始化LED呼吸灯
    // pthread_t monitor_thread;
    // pthread_create(&monitor_thread, NULL, led_breath_thread, NULL);
    
    /* 初始化数据通信串口 */
    uart0_hdl = app_uart_dev_init("uart0");
    context->uartdev = uart0_hdl;

    // 初始化录音
    recorder_mode_init(&g_recorder);
    context->recorder = &g_recorder;

    // 取消静音
#if 1
    gpio_direction_output(IO_PORTA_00, 1);  // 设置C出声
#else 
    gpio_direction_output(IO_PORTB_02, 1);  // 设置E出声
#endif

    /* 创建网络&提示音解码播放器 */
    music_player_init(&g_music_player);
    context->music_player = &g_music_player;

    ble_dev_task_scheduler_init();
    bdo_disable();

    /* 创建线程池 */
    pool = threadpool_create(4, 16);
    context->threadpool = pool;

    /*wifi manager 所需要的一些初始化*/
    wifi_manage_init(NULL);

    /* 初始化RSSI测距 */
    monitor_init();

    /* 开始RSSI测距 */
    monitor_start();

    // 初始化lfs
    lfs_t *lfs_hdl = app_lfs_init("mnt/sdfile/app/exif");
    ASSERT(lfs_hdl);
    context->lfs_hdl = lfs_hdl;
    //app_lfs_cfgfile_init(context->lfs_hdl);
    get_flash_app_cfg_data(context->lfs_hdl);


    /* 创建sc7a20 task 陀螺仪 */
    app_sc7a20_init();

    /* APP_MDOE 开始 */
    thread_fork("lwl_app_task", 15, 512 * 4, 0, 0, lwl_app_task, NULL);

    
    /* 【测试】默认10s后关机 */
    // threadpool_add_task(pool, auto_shutdown, NULL);
    /* 【调试】监测att send buffer remaing size */
    // thread_fork("moni", 15, 512 * 3, 0, 0, moni_task, NULL);
}

void play_end_callback(void *priv)
{
    log_i("play end\n");
}

void test1_play_end_callback(void *priv)
{
    log_i("test1 play end\n");
    app_music_play_net_music_with_callback(&g_music_player, "http://192.168.46.71:8888/19.%201KHz-0dB-30s.mp3", test1_play_end_callback, NULL);
}

void test2_play_end_callback(void *priv)
{
    log_i("test2 play end\n");
    app_music_play_net_music_with_callback(&g_music_player, "https://lx-sycdn.kuwo.cn/d238c8266e6df50edd90b5b00d81e695/67e0ce41/resource/n2/49/23/1247020737.mp3", test2_play_end_callback, NULL);
}

static void switch_completed(int code, void *priv)
{
    log_i("switch completed: %d", code);
}

extern lfs_t g_lfs;
static u8 en = 1;
static int app_music_key_click(struct key_event *key)
{
    int err = 0;
    switch (key->value) {
    // case KEY_OK:
    //     break;
    case KEY_1:
        log_i("KEY 1\n");
        app_music_play_net_music_with_callback(&g_music_player, "https://lx-sycdn.kuwo.cn/d238c8266e6df50edd90b5b00d81e695/67e0ce41/resource/n2/49/23/1247020737.mp3", NULL, NULL);
        
        break;
    case KEY_2:
        log_i("KEY 2\n");
        app_music_play_net_music_with_callback(&g_music_player, "http://192.168.46.71:8888/19.%201KHz-0dB-30s.mp3", NULL, NULL);
        break;
    case KEY_3:
        log_i("KEY 3\n");
        // 指定设备序列号
        // 通知C2将序列号设置到广播中
        // E端设备检查序列号是否与自己匹配，匹配则播放，不匹配则停止播放
        log_i("en = %d\n", en);

        break;
    case KEY_4:
        log_i("KEY 4\n");
        app_music_play_stop(&g_music_player);
        break;
    case KEY_OK:
        log_i("KEY OK\n");
        lfs_file_t lfs_file;
        app_music_play_lfs_music_with_callback(&g_music_player, "opening_45.mp3", NULL, NULL);
        // app_music_play_pause(&g_music_player);
        break;
        
    case KEY_VOLUME_DEC:
    case KEY_UP:
        // power_set_soft_poweroff();

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

static int app_cookie_key_event_handler(struct key_event *key)
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
static int app_cookie_device_event_handler(struct sys_event *sys_eve)
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
            // 播放WiFi连接失败提示音（未找到SSID）
            break;
        case NET_CONNECT_ASSOCIAT_FAIL:
            log_i("NET_CONNECT_ASSOCIAT_FAIL");
            // TODO: 可能在配网时需要发送ble通知
            // 播放WiFi连接失败提示音（关联失败）
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
            //set_led_white();; /// Net Succ
            // 播放联网成功提示音
            // dhcp ready = true
            // TODO: 网络连接成功，可在此时连接后端云服务器
            // TODO: 可配置WIFI和BLE共存时的RF共存参数设置

            int index_before = get_rf_coexistence_config_index();
            switch_rf_coexistence_config_table(3);
            int index_after = get_rf_coexistence_config_index();

            log_i("rf_coexistence_config_index %d -> %d\n", index_before, index_after);
        }   break;
        case NET_EVENT_DISCONNECTED:{
            log_i("NET_EVENT_DISCONNECTED\n");
            // dhcp ready = false
            // TODO: 可能需要进入回连状态，或者重新进入配网状态

            lwl_event_t e;
            e.event_type = LWL_EVENT_TYPE_NET_DISCONNECTED;
            lwl_event_notify(&e);
        }   break;
        case NET_EVENT_SMP_CFG_TIMEOUT:
            log_i("NET_EVENT_SMP_CFG_TIMEOUT\n");
            break;
        case NET_SMP_CFG_COMPLETED:
            log_i("NET_SMP_CFG_COMPLETED\n");
            break;
        case NET_EVENT_DISCONNECTED_AND_REQ_CONNECT:
            log_i("NET_EVENT_DISCONNECTED_AND_REQ_CONNECT\n");
            break;
        case NET_NTP_GET_TIME_SUCC:	//NTP获取成功事件返回 同步网络时间
            lwl_rtc_timer_update();
            log_i("NET_NTP_GET_TIME_SUCC %d\n", lwl_rtc_timer_get_sec());
            break;
        default:
            break;
        }
    }
    return false;
}

static int app_cookie_event_handler(struct application *app, struct sys_event *event)
{
    switch (event->type) {
    case SYS_KEY_EVENT:
        return app_cookie_key_event_handler((struct key_event *)event->payload);
    case SYS_DEVICE_EVENT:
        return app_cookie_device_event_handler(event);
    case SYS_NET_EVENT:
        return app_music_net_event_handler((struct net_event *)event->payload);
    case SYS_BT_EVENT:
        return ble_demo_bt_event_handler(event);
    default:
        return false;
    }
}

static int app_cookie_state_machine(struct application *app, enum app_state state,
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
        case ACTION_COOKIE_MAIN:
            app_cookie_main();
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

static const struct application_operation app_cookie_ops = {
    .state_machine  = app_cookie_state_machine,
    .event_handler 	= app_cookie_event_handler,
};

REGISTER_APPLICATION(app_cookie) = {
    .name 	= "app_cookie",
    .ops 	= &app_cookie_ops,
    .state  = APP_STA_DESTROY,
};
