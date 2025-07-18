#include "wifi_app_task.h"
#include "init.h"
#include "wifi/wifi_connect.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "lwip/dns.h"
#include "lwip/ip.h"
#include "os/os_api.h"
#include "lwip.h"
#include "server/server_core.h"
#include "system/timer.h"
#include "asm/debug.h"
#include "app_config.h"
#include "system/timer.h"
#include "json_c/json_tokener.h"
#include "server/net_server.h"
#include "streaming_media_server/fenice_config.h"
#include "event/net_event.h"
#include "syscfg/syscfg_id.h"
#include "sock_api/sock_api.h"
#include "asm/rf_coexistence_config.h"

#include "bt_ble/app_wifi_manage.h"
#include "bt_ble/server/ble_simple_server.h"
#include "app_mode/lwl_app_core.h"
#include "app_mode/lwl_app_framework.h"
#include "protocol/cp/c_uart_p.h"

#define INIT_SSID "KQQ"
#define INIT_PWD  "quickquick"

static wifi_connect_succ_cb wifi_connect_succ_cb_func_pri = NULL;
static wifi_connect_fail_cb wifi_connect_fail_cb_func_pri = NULL;

        
static void set_led_connecting(void)        { uart_send_set_led_color(COLOR_BLUE); }   /* 控制 led : 正在连接网络 蓝色 */
static void set_led_connect_error(void)     { uart_send_set_led_color(COLOR_RED); }    /* 控制 led : 网络连接失败 红色 */
static void set_led_connected(void)         { uart_send_set_led_color(COLOR_WHITE); }  /* 控制 led : 联网成功 白色 */

void register_wifi_connect_succ_func(wifi_connect_succ_cb callback, void *pri)
{
    if (NULL != wifi_connect_succ_cb_func_pri)
    {
        wifi_connect_succ_cb_func_pri = callback;
    }
}

void register_wifi_connect_fail_func(wifi_connect_fail_cb callback, void *pri)
{
    if (NULL != wifi_connect_fail_cb_func_pri)
    {
        wifi_connect_fail_cb_func_pri = callback;
    }
}

int func_param = 0;
wifi_scan_timeout_clallback func_ptr = NULL;

void register_wifi_scan_func(wifi_scan_timeout_clallback callback, int timeout_s)
{
    if (callback != NULL)
    {
        func_ptr = callback;
        func_param = timeout_s;
    }
}

static int wifi_event_callback(void *network_ctx, enum WIFI_EVENT event)
{
    int ret = 0;
    struct net_event net = {0};

    switch (event)
    {
    case WIFI_EVENT_MODULE_INIT:
        puts("|network_user_callback->WIFI_EVENT_MODULE_INIT\n");

        struct wifi_store_info wifi_default_mode_parm;
        memset(&wifi_default_mode_parm, 0, sizeof(struct wifi_store_info));
        wifi_default_mode_parm.mode = STA_MODE;
        wifi_default_mode_parm.sta_cnt = 1;
        wifi_default_mode_parm.connect_best_network = 1;
        memcpy(wifi_default_mode_parm.ssid[0], INIT_SSID, strlen(INIT_SSID));
        memcpy(wifi_default_mode_parm.pwd[0], INIT_PWD, strlen(INIT_PWD));
        

        wifi_set_sta_connect_timeout(5);
        // wifi_set_store_ssid_cnt(10);
        wifi_set_sta_connect_best_ssid(1);
        wifi_set_default_mode(&wifi_default_mode_parm, 5, 0);

        /*wifi manager 所需要的一些初始化*/
        wifi_manage_init(NULL);
        break;

    case WIFI_EVENT_MODULE_START:
        puts("|network_user_callback->WIFI_EVENT_MODULE_START\n");
        u32 tx_rate_control_tab = // 不需要哪个速率就删除掉,可以动态设定
            0 | BIT(0)            // 0:CCK 1M
            | BIT(1)              // 1:CCK 2M
            | BIT(2)              // 2:CCK 5.5M
            | BIT(3)              // 3:OFDM 6M
            | BIT(4)              // 4:MCS0/7.2M
            | BIT(5)              // 5:OFDM 9M
            | BIT(6)              // 6:CCK 11M
            | BIT(7)              // 7:OFDM 12M
            | BIT(8)              // 8:MCS1/14.4M
            | BIT(9)              // 9:OFDM 18M
            | BIT(10)             // 10:MCS2/21.7M
            | BIT(11)             // 11:OFDM 24M
            | BIT(12)             // 12:MCS3/28.9M
            | BIT(13)             // 13:OFDM 36M
            | BIT(14)             // 14:MCS4/43.3M
            | BIT(15)             // 15:OFDM 48M
            | BIT(16)             // 16:OFDM 54M
            | BIT(17)             // 17:MCS5/57.8M
            | BIT(18)             // 18:MCS6/65.0M
            | BIT(19)             // 19:MCS7/72.2M
            ;
        wifi_set_tx_rate_control_tab(tx_rate_control_tab);

        /*hzx add*/
        set_wifi_manager_mode(WIFI_IDLE_MODE);
        set_wifi_info_status(WIFI_INFO_IDLE);

        break;

    case WIFI_EVENT_MODULE_STOP:
        puts("|network_user_callback->WIFI_EVENT_MODULE_STOP\n");
        break;

    case WIFI_EVENT_AP_START:
        puts("|network_user_callback->WIFI_EVENT_AP_START\n");
        break;

    case WIFI_EVENT_AP_STOP:
        puts("|network_user_callback->WIFI_EVENT_AP_STOP\n");
        break;

    case WIFI_EVENT_STA_START:
        puts("|network_user_callback->WIFI_EVENT_STA_START\n");

        /*hzx add*/

        set_wifi_manager_mode(WIFI_STA_MODE);
        set_wifi_info_status(WIFI_INFO_STA_MODE_IDLE);
        set_wifi_cmd_status(WIFI_CMD_IDLE);
        set_wifi_connect_status(WIFI_CONNECT_IDLE);

        /* 控制 led : 正在连接网络 褐色 */
        set_led_connecting();

        break;

    case WIFI_EVENT_MODULE_START_ERR:
        puts("|network_user_callback->WIFI_EVENT_MODULE_START_ERR\n");
        break;

    case WIFI_EVENT_STA_STOP:
        puts("|netwk_user_callback->WIFI_EVENT_STA_STOP\n");

        set_wifi_manager_mode(WIFI_IDLE_MODE);
        set_wifi_info_status(WIFI_INFO_IDLE);
        set_wifi_connect_status(WIFI_CONNECT_IDLE);
        break;

    case WIFI_EVENT_STA_SCAN_COMPLETED:
        puts("|network_user_callback->WIFI_STA_SCAN_COMPLETED\n");
        store_wifi_scan_result(NULL);
        // 处理cmd发过来的scan
        if (func_ptr != NULL)
        {
            func_ptr(func_param);
        }
        break;

    case WIFI_EVENT_STA_CONNECT_SUCC:
        puts("|network_user_callback->WIFI_STA_CONNECT_SUCC\n");
        break;

    case WIFI_EVENT_MP_TEST_START:
        puts("|network_user_callback->WIFI_EVENT_MP_TEST_START\n");
        break;

    case WIFI_EVENT_MP_TEST_STOP:
        puts("|network_user_callback->WIFI_EVENorT_MP_TEST_STOP\n");
        break;

    case WIFI_EVENT_STA_CONNECT_TIMEOUT_NOT_FOUND_SSID:
        puts("|network_user_callback->WIFI_STA_CONNECT_TIMEOUT_NOT_FOUND_SSID\n");
        net.arg = "net";
        net.event = NET_CONNECT_TIMEOUT_NOT_FOUND_SSID;
        net_event_notify(NET_EVENT_FROM_USER, &net);

        /*hzx add*/

        // set_wifi_info_status(WIFI_INFO_STA_MODE_IDLE);
        set_wifi_cmd_status(WIFI_CMD_IDLE);
        set_wifi_connect_status(WIFI_CONNECT_TIMEOUT_NO_SSID);
        set_wifi_manager_mode(WIFI_IDLE_MODE);
        set_wifi_info_status(WIFI_INFO_IDLE);

        block_connect_sem_post(NULL); // 通知连接函数

        /* 控制 led : 网络连接失败 灰色 */
        //todo play audo 罗天宇///////////////////////////////////
        set_led_connect_error();

        break;

    case WIFI_EVENT_STA_CONNECT_ASSOCIAT_FAIL:
        puts("|network_user_callback->WIFI_STA_CONNECT_ASSOCIAT_FAIL\n");
        net.arg = "net";
        net.event = NET_CONNECT_ASSOCIAT_FAIL;
        net_event_notify(NET_EVENT_FROM_USER, &net);

        /*hzx 添加部分*/

        set_wifi_cmd_status(WIFI_CMD_IDLE);
        set_wifi_connect_status(WIFI_CONNECT_ASSOCIAT_FAIL);
        set_wifi_manager_mode(WIFI_IDLE_MODE);
        set_wifi_info_status(WIFI_INFO_IDLE);

        block_connect_sem_post(NULL); // 通知连接函数

        if (NULL != wifi_connect_fail_cb_func_pri)
        {
            wifi_connect_fail_cb_func_pri(NULL);
        }
        //todo play audo 罗天宇///////////////////////////////////
        /* 控制 led : 网络连接失败 灰色 */
        set_led_connect_error();

        break;

    case WIFI_EVENT_STA_CONNECT_ASSOCIAT_TIMEOUT:
        puts("|network_user_callback->WIFI_STA_CONNECT_ASSOCIAT_TIMEOUT .....\n");

        /*hzx add*/

        set_wifi_cmd_status(WIFI_CMD_IDLE);
        set_wifi_connect_status(WIFI_CONNECT_ASSOCIAT_TIMEOUT);
        set_wifi_manager_mode(WIFI_IDLE_MODE);
        set_wifi_info_status(WIFI_INFO_IDLE);
        block_connect_sem_post(NULL); // 通知连接函数
        //   //todo play audo 罗天宇///////////////////////////////////
        /* 控制 led : 网络连接失败 灰色 */
        set_led_connect_error();

        break;

    case WIFI_EVENT_STA_NETWORK_STACK_DHCP_SUCC:
        puts("|network_user_callback->WIFI_EVENT_STA_NETWPRK_STACK_DHCP_SUCC\n");
        net.arg = "net";
        net.event = NET_EVENT_CONNECTED;
        net_event_notify(NET_EVENT_FROM_USER, &net);

        /*hzx add*/

        set_wifi_cmd_status(WIFI_CMD_IDLE);
        set_wifi_connect_status(WIFI_CONNECT_SUCC);
        set_wifi_manager_mode(WIFI_STA_MODE);
        set_wifi_info_status(WIFI_INFO_STA_MODE_IDLE);
        block_connect_sem_post(NULL); // 通知连接函数

        // set_wifi_connect_end_time_ms(timer_get_ms()); // 为了计算连接时间的函数 （计算时间差）

        /* 控制 led : 联网成功 紫色 */
        set_led_connected();
        printf("the net connected enable led purple !\r\n");

        /* 存储 IP*/
        store_wifi_ip_info(NULL);

        lwl_app_context_t *ctx = lwl_app_core_context_get_instance();
        /* 通知 APP(小程序) 我已经获取到 IP */
        if (!threadpool_add_task(ctx->threadpool, notify_app_wifi_ip_succ, NULL))
        {
            printf("Failed to add the notify_app_wifi_ip_succ task !\r\n");
        }

        /* 获取 最新的 WIFI SSID 和 PWD */
        static app_wifi_info_t temp_wifi_info = {0};
        get_app_wifi_info(&temp_wifi_info);
        printf("the event callback get the current connected ssid: %s  pwd: %s\n", temp_wifi_info.ssid, temp_wifi_info.pwd);
        /* 存储到VM */
        if (temp_wifi_info.ssid[0] != '\0' && temp_wifi_info.pwd[0] != '\0'){
            wifi_store_mode_info(STA_MODE, temp_wifi_info.ssid, temp_wifi_info.pwd);
        }
        /* 将APP切换到 APP的连接成功模式 */
        lwl_app_mode_switch_to(APP_NET_CONNECTED_MODE);

        break;

    case WIFI_EVENT_STA_DISCONNECT:
        puts("|network_user_callback->WIFI_STA_DISCONNECT\n");
        net.arg = "net";
        net.event = NET_EVENT_DISCONNECTED;
        net_event_notify(NET_EVENT_FROM_USER, &net);

        set_wifi_cmd_status(WIFI_CMD_IDLE);
        set_wifi_connect_status(WIFI_CONNECT_STA_DISCONNECT);
        set_wifi_manager_mode(WIFI_IDLE_MODE);
        set_wifi_info_status(WIFI_INFO_IDLE);

        block_connect_sem_post(NULL); // 通知连接函数
        
        /* 控制 led : 网络连接失败 灰色 */
        // set_led_connect_error();
        //todo play audo 罗天宇///////////////////////////////////
        wifi_set_sta_connect_best_ssid(1);
        wifi_enter_sta_mode(INIT_SSID,INIT_PWD);
 

        break;

    case WIFI_EVENT_STA_NETWORK_STACK_DHCP_TIMEOUT:
        puts("|network_user_callback->WIFI_EVENT_STA_NETWPRK_STACK_DHCP_TIMEOUT\n");

        set_wifi_cmd_status(WIFI_CMD_IDLE);
        set_wifi_connect_status(WIFI_CONNECT_STA_DISCONNECT);
        set_wifi_manager_mode(WIFI_STA_MODE);
        set_wifi_info_status(WIFI_INFO_STA_MODE_IDLE);

        block_connect_sem_post(NULL); // 通知连接函数
        wifi_enter_smp_cfg_mode(); // 进入SMP模式
        break;

    case WIFI_EVENT_P2P_START:
        puts("|network_user_callback->WIFI_EVENT_P2P_START\n");
        break;

    case WIFI_EVENT_P2P_STOP:
        puts("|network_user_callback->WIFI_EVENT_P2P_STOP\n");
        break;

    case WIFI_EVENT_P2P_GC_DISCONNECTED:
        puts("|network_user_callback->WIFI_EVENT_P2P_GC_DISCONNECTED\n");
        break;

    case WIFI_EVENT_P2P_GC_NETWORK_STACK_DHCP_SUCC:
        puts("|network_user_callback->WIFI_EVENT_P2P_GC_NETWORK_STACK_DHCP_SUCC\n");
        break;

    case WIFI_EVENT_P2P_GC_NETWORK_STACK_DHCP_TIMEOUT:
        puts("|network_user_callback->WIFI_EVENT_P2P_GC_NETWORK_STACK_DHCP_TIMEOUT\n");
        break;

    case WIFI_EVENT_SMP_CFG_START:
        puts("|network_user_callback->WIFI_EVENT_SMP_CFG_START\n");

        // /*hzx add*/
        // set_wifi_cmd_status(WIFI_CMD_IDLE);
        // set_wifi_manager_mode(WIFI_SMP_CFG_MODE); // 设置当前模式为smp配置模式
        // set_wifi_info_status(WIFI_INFO_SMP_CFG_MODE);
        // set_wifi_connect_status(WIFI_CONNECT_IDLE);

        break;

    case WIFI_EVENT_SMP_CFG_STOP:
        puts("|network_user_callback->WIFI_EVENT_SMP_CFG_STOP\n");

        // set_wifi_cmd_status(WIFI_CMD_IDLE);
        // set_wifi_manager_mode(WIFI_IDLE_MODE); // 设置当前模式为IDLE配置模式
        // set_wifi_info_status(WIFI_INFO_IDLE);
        // set_wifi_connect_status(WIFI_CONNECT_IDLE);
        break;

    case WIFI_EVENT_SMP_CFG_TIMEOUT:
        puts("|network_user_callback->WIFI_EVENT_SMP_CFG_TIMEOUT\n");
        net.arg = "net";
        net.event = NET_EVENT_SMP_CFG_TIMEOUT;
        net_event_notify(NET_EVENT_FROM_USER, &net);
        break;

    case WIFI_EVENT_SMP_CFG_COMPLETED:
        puts("|network_user_callback->WIFI_EVENT_SMP_CFG_COMPLETED\n");
        net.arg = "net";
        net.event = NET_SMP_CFG_COMPLETED;
        net_event_notify(NET_EVENT_FROM_USER, &net);
        break;

    case WIFI_EVENT_PM_SUSPEND:
        puts("|network_user_callback->WIFI_EVENT_PM_SUSPEND\n");
        break;

    case WIFI_EVENT_PM_RESUME:
        puts("|network_user_callback->WIFI_EVENT_PM_RESUME\n");
        break;

    case WIFI_EVENT_AP_ON_ASSOC:;
        struct eth_addr *hwaddr = (struct eth_addr *)network_ctx;
        printf("WIFI_EVENT_AP_ON_ASSOC hwaddr = %02x:%02x:%02x:%02x:%02x:%02x \r\n\r\n",
               hwaddr->addr[0], hwaddr->addr[1], hwaddr->addr[2], hwaddr->addr[3], hwaddr->addr[4], hwaddr->addr[5]);
        break;

    case WIFI_EVENT_AP_ON_DISCONNECTED:
        hwaddr = (struct eth_addr *)network_ctx;
        printf("WIFI_EVENT_AP_ON_DISCONNECTED hwaddr = %02x:%02x:%02x:%02x:%02x:%02x \r\n\r\n",
               hwaddr->addr[0], hwaddr->addr[1], hwaddr->addr[2], hwaddr->addr[3], hwaddr->addr[4], hwaddr->addr[5]);
        break;

    default:
        break;
    }

    return ret;
}

static void wifi_app_task(void *priv) // 主要是create wifi 线程的
{
    wifi_set_store_ssid_cnt(NETWORK_SSID_INFO_CNT);
    // wifi_set_smp_cfg_just_monitor_mode(true);
    // wifi_set_sta_connect_best_ssid(0);
    wifi_set_event_callback(wifi_event_callback);
    wifi_on();
}

static int wireless_net_init(void) // 主要是create wifi 线程的
{
    puts("wireless_net_init \n\n");
    return thread_fork("wifi_app_task", 10, 1792, 0, 0, wifi_app_task, NULL);
} 
late_initcall(wireless_net_init);
