#include "app_wifi_manage.h"
#include "wifi/wifi_connect.h"
#include "os/os_api.h"
#include "app_lfs.h"
#include "lwip.h"
#include "le/att.h"

/*-------------------------------------------------本文件的 变量及其 所有函数的定义-----------------------------------*/
static OS_SEM g_disconnect_sem;                                                     // 断连函数(阻塞方式)所用到的sem
static OS_SEM g_block_connect_sem;                                                  // 连接函数(阻塞方式)所用到的sem
static CONNECT_FUNC_FLAG g_connect_func_flag = CONNECT_FUNC_BLOCK_DEFAULT;          // 连接函数(阻塞方式)的flag
static DISCONNECT_FUNC_FLAG g_disconnect_func_flag = DISCONNECT_FUNC_BLOCK_DEFAULT; // 断连函数(阻塞方式)的flag

static WIFI_MANAGER_MODE g_wifi_manager_mode = WIFI_IDLE_MODE;        // 当前wifi 模式
static WIFI_connect_STATUES g_wifi_connect_state = WIFI_CONNECT_IDLE; // 当前wifi 连接状态
static WIFI_INFO_STATUES g_wifi_info_state = WIFI_INFO_IDLE;          // 当前wifi 模块状态
static WIFI_SCAN_STATUES g_wifi_scan_state = WIFI_SCAN_STOP;          // 当前wifi 扫描状态
static WIFI_CMD_STATUES g_wifi_cmd_state = WIFI_CMD_DEFAULT;          // 当前命令状态

static struct wifi_scan_ssid_info g_all_wifi_info[STORE_WIFI_NUM_MAX] = {0};
static int g_all_wifi_info_num = 0;

static struct netif_info g_netip_info = {0};
static char *g_wifi_ip[WIFI_IP_LEN] = {0};

static app_wifi_info_t g_app_wifi_info = {0};

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
        g_connect_func_flag = CONNECT_FUNC_BLOCK_RUNNING;
        wifi_set_sta_connect_timeout(timeout_s);
        wifi_enter_sta_mode(ssid, pwd);
        ret = block_connect_sem_pend(NULL); // 等待连接成功或者失败
        printf("the ret = ---------------------------------------------------------------------%d\r\n", ret);
        if (0 == ret)
        {
            g_connect_func_flag = CONNECT_FUNC_BLOCK_DEFAULT;
            return get_wifi_connect_status(NULL);
        }
        else
        {
            g_connect_func_flag = CONNECT_FUNC_BLOCK_DEFAULT;
            return -1;
        }
    }
    else if (g_connect_func_flag == CONNECT_FUNC_BLOCK_RUNNING)
    {
        printf("the wifi_sta_mode_connect_block is running\r\n");
        return -5;
    }

    // int ret = 0;
    // if (g_connect_func_flag == CONNECT_FUNC_BLOCK_DEFAULT)
    // {
    //     g_connect_func_flag = CONNECT_FUNC_BLOCK_RUNNING;
    //     wifi_set_connect_sta_block(1);
    //     wifi_set_sta_connect_timeout(timeout_s);
    //     ret = wifi_enter_sta_mode(ssid, pwd);
    //     if (0 == ret)
    //     {
    //         printf("the sta_mode_connect_block success connect\r\n");
    //     }
    //     else if (-1 == ret)
    //     {
    //         printf("the sta_mode_connect_block fail conncet\r\n");
    //     }
    //     enum wifi_sta_connect_state state_flag = wifi_get_sta_connect_state();
    //     printf("the sta_mode_connect_state is %d    !!!!!!!!!!!!!!!!\r\n", state_flag);
    //     if ((state_flag == WIFI_STA_CONNECT_SUCC) && (ret == 0))
    //     {
    //         ret = WIFI_CONNECT_SUCC;
    //     }
    //     else if ((state_flag == WIFI_STA_CONNECT_ASSOCIAT_FAIL) && (ret == 1))
    //     {
    //         ret = WIFI_CONNECT_FAIL;
    //     }
    //     wifi_set_connect_sta_block(0); // 恢复连接为非阻塞方式
    //     g_connect_func_flag = CONNECT_FUNC_BLOCK_DEFAULT; // 恢复connect_func_flag为默认值
    //     return ret;
    // }
    // else if (g_connect_func_flag == CONNECT_FUNC_BLOCK_RUNNING)
    // {
    //     printf("the wifi_sta_mode_connect_block is running\r\n");
    //     return -5;
    // }
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
                ret = WIFI_CONNECT_IDLE;
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

// WIFI_connect_STATUES wifi_reconnect_config(lfs_t *lfs, int count)
// {
//     change_wifi_retry_connect_count(lfs, count);
//     return 0;
// }


/* block connect 所需要的sem 相关的函数*/

int block_connect_sem_creat(void *pri)
{
    int ret = 0;
    ret = os_sem_create(&g_block_connect_sem, 0);
    if (0 != ret)
    {
        printf("the block_connect_sem_creat fail\r\n");
    }
    return ret;
}
int block_connect_sem_pend(void *pri)
{
    int ret = 0;
    ret = os_sem_pend(&g_block_connect_sem, 0);
    if (0 != ret)
    {
        printf("the block_connect_sem_pend fail\r\n");
    }
    return ret;
}
int block_connect_sem_post(void *pri)
{
    int ret = 0;
    ret = os_sem_post(&g_block_connect_sem);
    if (0 != ret)
    {
        printf("the block_connect_sem_post fail\r\n");
    }
    return ret;
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
int wifi_scan_start(void)
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
    struct wifi_scan_ssid_info *temp_all_wifi_info = NULL;
    ret = get_wifi_scan_ssid_info(&temp_all_wifi_info);

    /*
        将temp_all_wifi_info 拷贝到g_all_wifi_info
        并更新，如果temp_all_wifi_info有g_all_wifi_info没有的ssid，则添加到g_all_wifi_info
    */
    for (int i = 0; i < ret; i++)
    {
        bool found = false;
        for (int j = 0; j < g_all_wifi_info_num; j++)
        {
            if (strcmp(temp_all_wifi_info[i].ssid, g_all_wifi_info[j].ssid) == 0)
            {
                // 如果ssid已经存在，更新信息
                memcpy(&g_all_wifi_info[j], &temp_all_wifi_info[i], sizeof(struct wifi_scan_ssid_info));
                found = true;
                break;
            }
        }
        if (!found)
        {
            // 如果ssid不存在且数组未满，添加到g_all_wifi_info
            if (g_all_wifi_info_num < STORE_WIFI_NUM_MAX)
            {
                memcpy(&g_all_wifi_info[g_all_wifi_info_num], &temp_all_wifi_info[i], sizeof(struct wifi_scan_ssid_info));
                g_all_wifi_info_num++; // 只增加新添加的SSID数量
            }
            else
            {
                // 处理数组已满的情况
                printf("Warning: g_all_wifi_info is full, cannot add new SSID: %s\n", temp_all_wifi_info[i].ssid);
            }
        }
    }
    ret = g_all_wifi_info_num;
    return ret;
}

/*清空SCAN到的 WiFi info*/
int clean_wifi_scan_result(void *pri)
{
    memset(g_all_wifi_info, 0, sizeof(g_all_wifi_info));
    g_all_wifi_info_num = 0;
    printf("WiFi scan result cleared.\r\n"); /* 清楚 计数器*/
    return 0;
}

/*存储 wifi ip 信息*/
void store_wifi_ip_info(void *pri)
{
    memset(&g_netip_info, 0, sizeof(g_netip_info));
    lwip_get_netif_info(1, &g_netip_info);
    u32 temp_ip = htonl(g_netip_info.ip);
    snprintf(g_wifi_ip, 32, "%d.%d.%d.%d",
             (temp_ip >> 24) & 0xFF, // 第一字节
             (temp_ip >> 16) & 0xFF, // 第二字节
             (temp_ip >> 8) & 0xFF,  // 第三字节
             temp_ip & 0xFF          // 第四字节
    );
    printf("====================current get wifi ip:%s ===============================\n", g_wifi_ip);
}

/*获取 wifi ip 信息*/
void get_wifi_ip_info(char *ip)
{
    memcpy(ip, g_wifi_ip, strlen(g_wifi_ip));
}

void get_wifi_scan_result(struct wifi_scan_ssid_info **ssid_info, int *num)
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
    ret = block_connect_sem_creat(NULL);
    if (0 != ret)
    {
        printf("the wifi_manage_init fail\r\n");
    }

    return ret;
}




/*-------------------------------------------------wifi 交给线程池相关的task start----------------------------------*/

void wifi_scan_task(void *arg)
{
    wifi_scan_start();
}
void wifi_connect_task(void *arg)
{
    wifi_connect_params_t *params = (wifi_connect_params_t *)arg;
    wifi_connect(
        params->ssid,
        params->pwd,
        params->timeout_s,
        params->connect_succ_func,
        params->connect_fail_func);
        free(params);
}
void wifi_disconnect_task(void *arg)
{
    wifi_disconnect(NULL);
}

/* -------------------------------------------------wifi 交给线程池相关的task end----------------------------------*/



/* ！！！！！！！！！！可能要加互斥锁 ！！！！！！！！！！*/
/* 设置app_wifi_info */
void set_app_wifi_info(app_wifi_info_t *info)
{
    if (info == NULL) {
        printf("Error: info is NULL in set_app_wifi_info\n");
        return;
    }
    memcpy(&g_app_wifi_info, info, sizeof(app_wifi_info_t));
    free(info);
}
/* 获取app_wifi_info */
void get_app_wifi_info(app_wifi_info_t *info)
{
    if (info == NULL) {
        printf("Error: info is NULL in get_app_wifi_info\n");
        return;
    }
    memcpy(info, &g_app_wifi_info, sizeof(app_wifi_info_t));
}
