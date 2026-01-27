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
#include "LED/LED.h"
#include "app_mode/lwl_app_framework.h"

#include "protocol/odm/odm_secure.h"
#include "app_mode/lwl_app_core.h"

#include "bt_ble/client/ble_distance.h"
#include "bt_ble/client/ble_dev_task_scheduler.h"
#include "log/log_mqtt.h"
#include "system/sys_time.h"

static void set_led_red_low_power(void)         { set_rgb_color(COLOR_RED); }

extern int app_music_bt_event_handler(struct sys_event *event);

static struct app_cookie_hdl g_cookie_hdl;

#define __this (&g_cookie_hdl)

music_player g_music_player;
recorder_hdl g_recorder;
threadpool_t *pool = NULL;

void *uart0_hdl = NULL;

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

void disable_gsensor()
{
    extern void * g_iic_hdl;
    int ret = 0;
    log_i("NOTIFY_GSENSOR_OFF\n");
    ret = sc7a20h_disable(g_iic_hdl);
    if (0 == ret)
    {
        log_i("GSENSOR OFF\n");
    }
}


void base_lwl_event_handler(lwl_event_t *e)
{
    switch (e->event_type) {
    case LWL_EVENT_TYPE_SHUTDOWN:
        log_i("base_lwl_event_handler LWL_EVENT_TYPE_SHUTDOWN\r\n");
        disable_gsensor();
        lwl_app_mode_switch_to(APP_POWER_OFF_MODE);
        break;
    case LWL_EVENT_TYPE_SLEEP:
        lwl_app_mode_switch_to(APP_POWER_OFF_MODE);
        break;
    case LWL_EVENT_TYPE_TOUCH_KEY:
        g_sensor_reset_alive_timer();
        if (e->touch_key_cmd.touch_key_event_type == 0) {
            log_i("Touch key pressed down\n");
        } else {
            log_i("Touch key released\n");
        }
        // play 提示音
        break;
    case LWL_EVENT_TYPE_KEY:
        g_sensor_reset_alive_timer();
        log_i("PRESS NORMAL KEY\n");
        break;
    case LWL_EVENT_TYPE_LOW_BATTERY:
        u16 voltage = get_vbat_level();
        log_i("LOW BATTERY EVENT voltage : %d\n", voltage);
        if (voltage < LOW_BATT_VOLT) {
            set_led_red_low_power();
        }
        break;
    case LWL_EVENT_TYPE_RST_KEY:
        g_sensor_reset_alive_timer();
        // Reset WiFi SSID
        app_del_stored_wifi_ssid(); // 清除设备保存过的所有wifi
        wifi_off();
        //增加音效提示
        play_system_music("reset_wifi.mp3");
        log_i("PRESS RESET KEY\n");
        break;
    // case LWL_EVENT_TYPE_UART_CLEAR_WIFI_SSID:
    //     log_i("CLEAR_WIFI\n");
    //     // Reset WiFi SSID
    //     app_del_stored_wifi_ssid(); // 清除设备保存过的所有wifi
    //     uart_response_clear_wifi_ssid();
    //     system_reset(); //restart
    //     break;
    //电量同步
    case LWL_EVENT_TYPE_UART_SYNC:
        log_i("7016 SYNC v=1; battery: %d \n", e->uart_cmd_sync.battery);
        uart_response_synch_status(1, MAX_LE_AUDIO_VOLUME);/// 超时后，返回idle
        // log_i("7016 SYNC battery: %d", e->uart_cmd_sync.battery);
        // if(lastbattery != e->uart_cmd_sync.battery) {
        //     lastbattery = e->uart_cmd_sync.battery;
        //     log_mqtt_set_battery_level(NULL, lastbattery);
        // }
        // if(e->uart_cmd_sync.battery < 30 ) {
        //     // set_led_red_low_power();
        // }
        // if (e->uart_cmd_sync.charging == 1) {
        //     // log_mqtt_set_battery_level(NULL, lastbattery); // 上报充电状态
        // }
        break;
    }
}


static void moni_task(void *priv)
{
    while (1) {
        u32 vaild_len = 0;
        ble_op_multi_att_get_remain(client_con_handle, &vaild_len);
        log_i("moni valid_len: %d\n", vaild_len);           
        msleep(1000);
    }
}
    
#include "util/led_breath.h"

static void* led_breath_thread(void* arg)
{
    quick_test_with_macros(20); // 直接在线程里调用呼吸灯测试函数,20次循环
    return NULL;
}

#define SEC_PER_DAY     (24 * 60 * 60U)
#define MS_PER_DAY      (SEC_PER_DAY * 1000U)

static void sync_log_time_to_beijing(void)
{
    u32 ntp_sec = lwl_rtc_timer_get_sec();
    if (!ntp_sec) {
        return;
    }

    u32 beijing_sec = (ntp_sec + 8 * 3600U) % SEC_PER_DAY;
    u32 beijing_ms = beijing_sec * 1000U;
    u32 uptime_ms = timer_get_ms() % MS_PER_DAY;
    int offset = (int)beijing_ms - (int)uptime_ms;

    log_set_time_offset(offset);

    u32 hour = beijing_sec / 3600U;
    u32 minute = (beijing_sec % 3600U) / 60U;
    u32 second = beijing_sec % 60U;
    log_i("[LogTime] synced to Beijing %02u:%02u:%02u \n", hour, minute, second);
}

static void peri_events(void *priv)
{
    while (1) {
        u32 peri_curr_time = timer_get_ms();
        lwl_peri_dev_loop(peri_curr_time);
        ///log_i("peri_events : " );           
        msleep(10);
    }
}

static void app_cookie_main(void)
{
    log_i("Hello app_cookie world!\r\n");
    log_i("Version: IO_PORTC_10\r\n");
    log_i("%s %s\r\n", __DATE__, __TIME__);

    lwl_app_core_init();
    
    lwl_app_context_t *context = lwl_app_core_context_get_instance();

    lwl_event_handler_add(base_lwl_event_handler);

    led_breath_init(); // 初始化LED呼吸灯
    // pthread_t monitor_thread;
    // pthread_create(&monitor_thread, NULL, led_breath_thread, NULL);
    
    /* 初始化数据通信串口 */
    uart0_hdl = app_uart_dev_init("uart0");
    context->uartdev = uart0_hdl;

    // 初始化录音
    recorder_mode_init(&g_recorder);
    context->recorder = &g_recorder;

    gpio_direction_output(IO_PORTC_10, 1);                  // 拉高 C 口wifi-LDO，输出
    // 取消静音
    gpio_direction_output(IO_PORTA_00, 1);                  // 拉高 C 口功放使能脚，恢复扬声器输出
    gpio_direction_output(IO_PORTB_02, 0);                  // 备用方案：拉高 E 口功放使能
    uart_response_synch_status(1, MAX_LE_AUDIO_VOLUME);     // 同步初始音量状态给 7016
    /* 创建网络&提示音解码播放器 */
    music_player_init(&g_music_player);                     // 初始化音乐播放器，支持本地/网络音频
    context->music_player = &g_music_player;                // 记录播放器指针以便模式和提示音调用

    wifi_set_pwr(6);                                     // 设置 WiFi 发射功率为最大

    ble_dev_task_scheduler_init();                          // 启动 BLE 任务调度器，管理扫描/连接

    /* 创建线程池 */
    pool = threadpool_create(4, 16);                        // 创建 4 个线程、队列深度 16 的线程池
    context->threadpool = pool;                             // 保存线程池实例供异步任务复用

    /*wifi manager 所需要的一些初始化*/
    wifi_manage_init(NULL);                                 // 初始化 WiFi 管理器（STA/BLE 共存配置）

    /* 初始化RSSI测距 */
    monitor_init();                                      // 可选：初始化 RSSI 测距组件

    /* 开始RSSI测距 */
    monitor_start();                                     // 可选：启动测距任务

    // 初始化lfs
    lfs_t *lfs_hdl = app_lfs_init("mnt/sdfile/app/exif");   // 挂载 LittleFS，用于配置和资源读写
    ASSERT(lfs_hdl);                                        // 确保文件系统初始化成功
    context->lfs_hdl = lfs_hdl;                             // 保存文件系统句柄
    //app_lfs_cfgfile_init(context->lfs_hdl);
    get_flash_app_cfg_data(context->lfs_hdl);               // 从配置区加载设备参数（SN、OTA 等）


    /* 创建sc7a20 task 陀螺仪 */
    app_sc7a20_init();                                      // 启动 SC7A20 加速度计任务，支持姿态/震动检测

    lwl_peri_dev_init(); // 按键、LED、触摸、充电等外设初始化
    thread_fork("peri_events", 15, 512 * 4, 0, 0, peri_events, NULL);
    // 启动MQTT
    log_mqtt_start();

    /* APP_MDOE 开始 */
    thread_fork("lwl_app_task", 15, 512 * 4, 4, 0, lwl_app_task, NULL); // 创建 LWL 模式主任务，驱动状态机

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
    log_i("switch completed: %d \n", code);
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
            log_i("[NET] NET_EVENT_CONNECTED (arg=%s)", event->arg);
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
            log_i("[NET] NET_EVENT_DISCONNECTED (arg=%s)\n", event->arg);
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
            log_i("[NET] NET_EVENT_DISCONNECTED_AND_REQ_CONNECT\n");
            break;
        case NET_NTP_GET_TIME_SUCC:	//NTP获取成功事件返回 同步网络时间
            lwl_rtc_timer_update();
            log_i("NET_NTP_GET_TIME_SUCC %d\n", lwl_rtc_timer_get_sec());
            sync_log_time_to_beijing();
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

// 状态机：管理APP的生命周期和状态转换
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
    case APP_STA_DESTROY: // STA销毁
        break;
    }

    return 0;
}

static const struct application_operation app_cookie_ops = {
    .state_machine  = app_cookie_state_machine,
    .event_handler 	= app_cookie_event_handler,
};

REGISTER_APPLICATION(app_cookie) = {
    .name 	= APP_COOKIE_NAME,
    .ops 	= &app_cookie_ops,
    .state  = APP_STA_DESTROY,
};
