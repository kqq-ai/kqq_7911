#ifndef _APP_WIFI_MANAGE_H_
#define _APP_WIFI_MANAGE_H_

#include "wifi_app_task.h"
#include "wifi/wifi_connect.h"

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
    WIFI_CONNECT_SUCC,
    WIFI_CONNECT_FAIL,
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
WIFI_connect_STATUES wifi_connect(
    char *ssid,
    char *pwd,
    int timeout_s,
    wifi_connect_succ_cb connect_succ_func,
    wifi_connect_fail_cb connect_fail_func);

WIFI_connect_STATUES wifi_disconnect(wifi_disconnect_cb disconnect_func);
WIFI_connect_STATUES wifi_disconnect_block(void *pri);

WIFI_connect_STATUES wifi_reconnect_config(int count);

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
int wifi_scan_start(void *pri);

void wifi_scan_stop(void *pri);

/*返回值为 wifi 的个数*/
int store_wifi_scan_result(void *pri); /*保存SCAN到的 WiFi info*/
void get_wifi_scan_result(struct wifi_scan_ssid_info **ssid_info,int *num);/*获取SCAN到的 WiFi info*/

/*-------------------------------------------------WiFi 扫描相关   end----------------------------------------------*/










#endif