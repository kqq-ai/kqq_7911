#ifndef __WIFI_APP_TASK_H__
#define __WIFI_APP_TASK_H__

typedef void(*wifi_connect_succ_cb)(void *);
void register_wifi_connect_succ_func(wifi_connect_succ_cb callback,void * pri);   //连接成功

typedef void(*wifi_connect_fail_cb)(void *);
void register_wifi_connect_fail_func(wifi_connect_fail_cb callback,void * pri);    //连接失败

typedef void (*wifi_disconnect_cb)(void *pri); //断开连接
void register_wifi_disconnect_func(wifi_disconnect_cb callback,void *pri);


#endif