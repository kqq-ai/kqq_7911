#ifndef _APP_WIFI_MANAGE_H_
#define _APP_WIFI_MANAGE_H_

#include "wifi_app_task.h"
#include "wifi/wifi_connect.h"
#include "lfs.h"

#define STORE_WIFI_NUM_MAX 50
#define WIFI_IP_LEN 16
#define WIFI_SCAN_TO_RECONNECT_TIMEOUT 7000

typedef struct
{
    char ssid[33];
    char pwd[33];
} app_wifi_info_t;


/* 
    set_app_wifi_info
    get_app_wifi_info
    这两个函数用来 操作保存在 lfs中 的wifi信息 ---->为了自动重连
*/
/* 设置app_wifi_info */
void set_app_wifi_info(app_wifi_info_t *info);
/* 获取app_wifi_info */
void get_app_wifi_info(app_wifi_info_t *info);


/*-------------------------------------------------整体的 初始化相关   start-----------------------------------------*/

/* wifi 模式*/
typedef enum 
{
    WIFI_IDLE_MODE,    //默认的模式，一般不用到
    WIFI_SMP_CFG_MODE, //上电之后设置为smp配置模式
    WIFI_STA_MODE
}WIFI_MANAGER_MODE;
WIFI_MANAGER_MODE set_wifi_manager_mode(WIFI_MANAGER_MODE mode);
WIFI_MANAGER_MODE get_wifi_manager_mode(void *pri);


int wifi_manage_init(void *pri); // 初始化wifi管理模块

/*-------------------------------------------------WiFi 状态相关    end---------------------------------------------*/

/*-------------------------------------------------WiFi 状态相关    start-------------------------------------------*/
/* WiFi 扫描状态 : 用来处理wifi 扫描的逻辑状态*/
typedef enum
{
    WIFI_SCAN_STOP,
    WIFI_SCAN_RUNNING,
    WIFI_SCAN_TIMEOUT,
} WIFI_SCAN_STATUES;
WIFI_SCAN_STATUES set_wifi_scan_status(WIFI_SCAN_STATUES status);
WIFI_SCAN_STATUES get_wifi_scan_status(void *pri);

/* WiFi 连接状态 : 用来处理wifi 连接的逻辑状态*/
typedef enum
{
    WIFI_CONNECT_IDLE = 0x00,
    WIFI_CONNECT_SUCC,
    WIFI_CONNECT_ASSOCIAT_FAIL,
    WIFI_CONNECT_ASSOCIAT_TIMEOUT,
    WIFI_CONNECT_STA_DISCONNECT,
    WIFI_CONNECT_TIMEOUT_OR_NO_SSID,
} WIFI_connect_STATUES;

WIFI_connect_STATUES set_wifi_connect_status(WIFI_connect_STATUES status);
WIFI_connect_STATUES get_wifi_connect_status(void *pri);

/* WiFi 模块状态 : 用来处理wifi 模块的逻辑状态 -----也就是此时 app中wifi处于一个什么模式状态下 运行*/
typedef enum
{
    WIFI_INFO_IDLE,        
    WIFI_INFO_SMP_CFG_MODE, 
    WIFI_INFO_STA_MODE_IDLE,
    WIFI_INFO_STA_MODE_SCANNING,
    WIFI_INFO_STA_MODE_CNNECTING
} WIFI_INFO_STATUES;

WIFI_INFO_STATUES set_wifi_info_status(WIFI_INFO_STATUES status);
WIFI_INFO_STATUES get_wifi_info_status(void *pri);


/* WiFi 命令状态 : 用来处理wifi 命令的逻辑状态 -----也就是收到小程序发来的命令，做的状态改变，来判断那些命令需要做处理*/
typedef enum
{
    WIFI_CMD_DEFAULT,
    WIFI_CMD_IDLE,
    WIFI_CMD_SCANNING,
    WIFI_CMD_CONNECTING,
    WIFI_CMD_RECONNECTING,
} WIFI_CMD_STATUES;

WIFI_CMD_STATUES set_wifi_cmd_status(WIFI_CMD_STATUES status);
WIFI_CMD_STATUES get_wifi_cmd_status(void *pri);


/*-------------------------------------------------WiFi 状态相关   end----------------------------------------------*/

/*-------------------------------------------------WiFi 连接相关   start--------------------------------------------*/
typedef enum
{
    CONNECT_FUNC_BLOCK_DEFAULT,
    CONNECT_FUNC_BLOCK_RUNNING
} CONNECT_FUNC_FLAG;

typedef enum
{
    DISCONNECT_FUNC_BLOCK_DEFAULT,
    DISCONNECT_FUNC_BLOCK_RUNNING
} DISCONNECT_FUNC_FLAG;

WIFI_connect_STATUES wifi_connect_block(char *ssid, char *pwd, int timeout_s);
WIFI_connect_STATUES wifi_auto_reconnect(void); /* 自动重连函数 */
WIFI_connect_STATUES wifi_connect(
    char *ssid,
    char *pwd,
    int timeout_s,
    wifi_connect_succ_cb connect_succ_func,
    wifi_connect_fail_cb connect_fail_func);

WIFI_connect_STATUES wifi_disconnect(wifi_disconnect_cb disconnect_func);
WIFI_connect_STATUES wifi_disconnect_block(void *pri);

// WIFI_connect_STATUES wifi_reconnect_config(lfs_t *lfs, int count);

/* block connect 所需要的sem 相关的函数*/

int block_connect_sem_creat(void *pri);
int block_connect_sem_pend(void *pri);
int block_connect_sem_post(void *pri);

/*disconnect 所需要的sem 相关的函数*/
int disconnect_sem_creat(void *pri);
int disconnect_sem_pend(void *pri);
int disconnect_sem_post(void *pri);

/*-------------------------------------------------WiFi 连接相关   end----------------------------------------------*/

/*-------------------------------------------------WiFi 扫描相关   start--------------------------------------------*/
/* 
返回值：
* 0: 扫描成功
* -1: 扫描失败 STA模式下调用返回-1代表WIFI OFF/正在扫描/连接中,无需启动扫描, 可以等待几秒或者扫描完成事件到来获取结果
*/
int wifi_scan_start(void);

void wifi_scan_stop(void *pri);

/*返回值为 wifi 的个数*/
int store_wifi_scan_result(void *pri); /*保存SCAN到的 WiFi info*/
int clean_wifi_scan_result(void *pri); /*清空SCAN到的 WiFi info*/
void get_wifi_scan_result(struct wifi_scan_ssid_info **ssid_info,int *num);/*获取SCAN到的 WiFi info*/



/*存储 wifi ip 信息*/
void store_wifi_ip_info( void *pri);

/*获取 wifi ip 信息*/
void get_wifi_ip_info(char *ip);
/*-------------------------------------------------WiFi 扫描相关   end----------------------------------------------*/





/*-------------------------------------------------wifi 交给线程池相关的task start----------------------------------*/

typedef struct {
    char ssid[33];
    char pwd[33];
    int timeout_s;
    wifi_connect_succ_cb connect_succ_func;
    wifi_connect_fail_cb connect_fail_func;
} wifi_connect_params_t;


void wifi_scan_task(void *arg);
void wifi_connect_task(void *arg);
void wifi_disconnect_task(void *arg);

/* -------------------------------------------------wifi 交给线程池相关的task end----------------------------------*/


/*
1.WIFI_EVENT_STA_SCAN_COMPLETED

扫描完成事件,通常由STA模式连接过程中扫描空中所有信道完成或者用户调用wifi_sta_scan_once请求扫描触发的事件,
接收到此事件后，应用程序的事件回调函数可以调用wifi_get_scan_result获取扫描结果

2.WIFI_EVENT_STA_CONNECT_SUCC

STA 已成功连接目标 AP的事件，接收到此事件后，Wi-Fi 驱动程序根据应用程序响应返回结果
决定是否启动 DHCP 客户端服务并开始获取 IP 地址,还是说使用静态IP地址

3.WIFI_EVENT_STA_CONNECT_TIMEOUT_NOT_FOUND_SSID

连接目标 AP超时事件,原因是超时时间内扫描不到指定SSID,超时时间应用程序可以通过
wifi_set_sta_connect_timeout接口配置

4.WIFI_EVENT_STA_CONNECT_TIMEOUT_ASSOCIAT_FAIL

连接目标 AP超时失败事件,原因是超时时间内关联失败,通常原因是密码错误,或者距离太远/干扰太多导致通信失败

5.WIFI_EVENT_STA_DISCONNECT

STA模式下与AP断开事件,此事件将在以下情况下产生:

Wi-Fi station 已成功连接至 AP的情况下调用了wifi_enter_ap_mode,wifi_enter_sta_mode,wifi_enter_smp_cfg_mode,wifi_off函数触发

Wi-Fi 连接因为某些原因而中断，例如：由于信号太差,station 连续多次丢失 太多 beacon帧、AP 脱离 station 连接、AP 验证模式改变等

接收到此事件后，通常需要通知针对基于套接字编写的客户端应用程序关闭套接字.如果此事件不是用户期待的,可以调用wifi_enter_sta_mode进行重新连接

6.WIFI_EVENT_STA_NETWORK_STACK_DHCP_SUCC

当 DHCP 客户端成功从 DHCP 服务器获取 IP 地址，将引发此事件, 意味着应用程序可以获取到设备IP地址,并且可以创建套接字连接服务器

7.WIFI_EVENT_STA_NETWORK_STACK_DHCP_TIMEOUT

当 Wi-Fi station 已成功连接至 AP, 但是DHCP 客户端在指定超时时间内未能够从 DHCP 服务器获取 IP地址，将引发此事件, 
通常情况下是由于距离远信号不好,空间干扰大导致的,应用程序可以调用lwip_set_dhcp_timeout设定超时时间,默认不指定为15秒. 
引发事件后lwip协议栈内部会不断重试获取 IP地址


*/



#endif