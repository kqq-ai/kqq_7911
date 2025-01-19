#include "app_wifi_manage.h"
#include "wifi/wifi_connect.h"
#include "os/os_api.h"
#include "app_lfs.h"

/*-------------------------------------------------本文件的 变量及其 所有函数的定义-----------------------------------*/
static OS_SEM g_disconnect_sem;                                                     // 断连函数(阻塞方式)所用到的sem
static CONNECT_FUNC_FLAG g_connect_func_flag = CONNECT_FUNC_BLOCK_DEFAULT;          // 连接函数(阻塞方式)的flag
static DISCONNECT_FUNC_FLAG g_disconnect_func_flag = DISCONNECT_FUNC_BLOCK_DEFAULT; // 断连函数(阻塞方式)的flag

static WIFI_MANAGER_MODE g_wifi_manager_mode = WIFI_IDLE_MODE;        // 当前wifi 模式
static WIFI_connect_STATUES g_wifi_connect_state = WIFI_CONNECT_SUCC; // 当前wifi 连接状态
static WIFI_INFO_STATUES g_wifi_info_state = WIFI_INFO_IDLE;          // 当前wifi 模块状态
static WIFI_SCAN_STATUES g_wifi_scan_state = WIFI_SCAN_STOP;          // 当前wifi 扫描状态
static WIFI_CMD_STATUES g_wifi_cmd_state = WIFI_CMD_DEFAULT;          // 当前命令状态

static struct wifi_scan_ssid_info *g_all_wifi_info = NULL;
static int g_all_wifi_info_num = 0;

// 申明
int get_wifi_scan_ssid_info(struct wifi_scan_ssid_info **ssid_info);

// 状态相关的函数定义

WIFI_SCAN_STATUES set_wifi_scan_status(WIFI_SCAN_STATUES status)
{
    g_wifi_scan_state = status;
    return g_wifi_scan_state;
}
WIFI_SCAN_STATUES get_wifi_scan_status(void *pri)
{
    return g_wifi_scan_state;
}

WIFI_CMD_STATUES set_wifi_cmd_status(WIFI_CMD_STATUES status)
{
    g_wifi_cmd_state = status;
    return g_wifi_cmd_state;
}
WIFI_CMD_STATUES get_wifi_cmd_status(void *pri)
{
    return g_wifi_cmd_state;
}

WIFI_MANAGER_MODE set_wifi_manager_mode(WIFI_MANAGER_MODE mode)
{
    g_wifi_manager_mode = mode;
    return g_wifi_manager_mode;
}
WIFI_MANAGER_MODE get_wifi_manager_mode(void *pri)
{
    return g_wifi_manager_mode;
}

WIFI_connect_STATUES set_wifi_connect_status(WIFI_connect_STATUES status)
{
    g_wifi_connect_state = status;
    return g_wifi_connect_state;
}
WIFI_connect_STATUES get_wifi_connect_status(void *pri)
{
    return g_wifi_connect_state;
}
WIFI_INFO_STATUES set_wifi_info_status(WIFI_INFO_STATUES status)
{
    g_wifi_info_state = status;
    return g_wifi_info_state;
}
WIFI_INFO_STATUES get_wifi_info_status(void *pri)
{
    return g_wifi_info_state;
}

// 连接相关的函数定义
/*
 * 返回值：
 * WIFI_CONNECT_SUCC : 连接成功
 * WIFI_CONNECT_FAIL : 连接失败
 */
WIFI_connect_STATUES wifi_connect_block(char *ssid, char *pwd, int timeout_s)
{
    int ret = 0;
    if (g_connect_func_flag == CONNECT_FUNC_BLOCK_DEFAULT)
    {

        wifi_set_sta_connect_timeout(timeout_s);
        wifi_set_connect_sta_block(1);
        ret = wifi_enter_sta_mode(ssid, pwd);
        if (0 == ret)
        {
            printf("the sta_mode_connect_block success connect\r\n");
        }
        else if (-1 == ret)
        {
            printf("the sta_mode_connect_block fail conncet\r\n");
        }
        enum wifi_sta_connect_state state_flag = wifi_get_sta_connect_state();
        printf("the sta_mode_connect_state is %d    !!!!!!!!!!!!!!!!\r\n", state_flag);
        if ((state_flag == WIFI_STA_CONNECT_SUCC) && (ret == 0))
        {
            ret = WIFI_CONNECT_SUCC;
        }
        else if ((state_flag == WIFI_STA_CONNECT_ASSOCIAT_FAIL) && (ret == 1))
        {
            ret = WIFI_CONNECT_FAIL;
        }
        wifi_set_connect_sta_block(0); // 恢复连接为非阻塞方式

        return ret;
    }
    else if (g_connect_func_flag == CONNECT_FUNC_BLOCK_RUNNING)
    {
        printf("the wifi_sta_mode_connect_block is running\r\n");
        return -5;
    }
}
WIFI_connect_STATUES wifi_connect(
    char *ssid,
    char *pwd,
    int timeout_s,
    wifi_connect_succ_cb connect_succ_func,
    wifi_connect_fail_cb connect_fail_func)
{
    int ret = 0;

    wifi_set_sta_connect_timeout(timeout_s);
    wifi_set_connect_sta_block(0); // 确保连接为非阻塞方式
    wifi_enter_sta_mode(ssid, pwd);
    register_wifi_connect_succ_func(connect_succ_func, NULL);
    register_wifi_connect_fail_func(connect_fail_func, NULL);
}
WIFI_connect_STATUES wifi_disconnect(void (*wifi_disconnect_cb)(void *pri))
{
    int ret = 0;
    wifi_enter_smp_cfg_mode();
    return 0;
}
WIFI_connect_STATUES wifi_disconnect_block(void *pri)
{
    int ret = 0;
    if (g_disconnect_func_flag == DISCONNECT_FUNC_BLOCK_DEFAULT)
    {
        struct wifi_mode_info info;
        wifi_enter_smp_cfg_mode();
        if (!disconnect_sem_pend(NULL))
        {
            wifi_get_mode_cur_info(&info);
            if (info.mode == SMP_CFG_MODE)
            {
                ret = WIFI_CONNECT_FAIL;
                return ret;
            }
        }
    }
    else if (g_disconnect_func_flag == DISCONNECT_FUNC_BLOCK_RUNNING)
    {
        printf("the wifi_sta_mode_disconnect_block is running\r\n");
        return -5;
    }
}

WIFI_connect_STATUES wifi_reconnect_config(int count)
{
    change_wifi_retry_connect_count(count);
    return 0;
}



/* disconnect 所需要的sem 相关的函数 */
int disconnect_sem_creat(void *pri)
{
    int ret = 0;
    ret = os_sem_create(&g_disconnect_sem, 0);
    if (0 != ret)
    {
        printf("the disconnect_sem_creat fail\r\n");
    }
    return ret;
}
int disconnect_sem_pend(void *pri)
{
    int ret = 0;
    ret = os_sem_pend(&g_disconnect_sem, 0);
    if (0 != ret)
    {
        printf("the disconnect_sem_pend fail\r\n");
    }
    return ret;
}
int disconnect_sem_post(void *pri)
{
    int ret = 0;
    ret = os_sem_post(&g_disconnect_sem);
    if (0 != ret)
    {
        printf("the disconnect_sem_post fail\r\n");
    }
    return ret;
}

/*扫描相关的函数定义*/
int wifi_scan_start(void *pri)
{
    int ret = 0;
    ret = wifi_scan_req();
    return ret;
}
// void wifi_scan_stop(void *pri);
int get_wifi_scan_ssid_info(struct wifi_scan_ssid_info **ssid_info)
{
    printf("wifi_scan_ssid_info\r\n");
    struct wifi_scan_ssid_info *sta_ssid_info;
    u32 sta_ssid_num = 0;
    sta_ssid_info = wifi_get_scan_result(&sta_ssid_num);
    wifi_clear_scan_result();

    for (u32 i = 0; i < sta_ssid_num; i++)
    {
        printf("find scan ssid = [%s]\r\n", sta_ssid_info[i].ssid);
    }

    *ssid_info = sta_ssid_info;
    return sta_ssid_num;
}

int store_wifi_scan_result(void *pri)
{
    int ret = 0;
    ret = get_wifi_scan_ssid_info(&g_all_wifi_info);
    g_all_wifi_info_num = ret;
    return ret;
}
void get_wifi_scan_result(struct wifi_scan_ssid_info **ssid_info,int *num)
{
    *ssid_info = g_all_wifi_info;
    *num = g_all_wifi_info_num;
}

// 初始化wifi管理模块
int wifi_manage_init(void *pri)
{
    int ret = 0;
    ret = disconnect_sem_creat(NULL);
    if (0 != ret)
    {
        printf("the wifi_manage_init fail\r\n");
    }

    return ret;
}