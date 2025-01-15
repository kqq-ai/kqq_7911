#ifndef __WIFI_APP_TASK_H__
#define __WIFI_APP_TASK_H__


typedef void(*wifi_scan_timeout_clallback)(int);
void register_wifi_scan_func(wifi_scan_timeout_clallback callback,int timeout_s);


#endif