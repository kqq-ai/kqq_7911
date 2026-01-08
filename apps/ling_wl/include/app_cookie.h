#ifndef __APP_COOKIE_H__
#define __APP_COOKIE_H__

#include "cpu.h"
#define SN_SIZE 15

struct app_cookie_hdl {

};
void disable_gsensor();

#define LOCAL_VERSION_CODE "v0.0.72" 
#define LOW_BATT_VOLT 370 
#define LOW_BATT_VERY_SHUTDOWN 350 

#define WRITE_SN_KEY 0 // 设置为1以强制写入SN/Key，忽略已有SN
#define TOUCH_KEY_AVAILABLE 1// 设置触摸按键功能可用

#define TEST_LOCOL_SERVER "test"// 设置为连接本地调试服务器

#define SERVER_DOMAIN  "test.coolqq.com"
// #define SERVER_DOMAIN  "pre.coolqq.com"
// #define SERVER_DOMAIN  "coolqq.com"

#define PRINT_RSSI_LOG 0 // 是否打印RSSI日志

#define LED_R_IO			IO_PORTA_08					//LED 红
#define LED_G_IO			IO_PORTA_10					//LED 绿
#define LED_B_IO			IO_PORTA_09					//LED 蓝

#define G_INT_IO		    IO_PORTC_00					//加速度传感器中断
#define TOUCH_IO            IO_PORTC_06                 //触摸按键IO
#define KEY_IO			    IO_PORTC_09					//按键


#define APP_COOKIE_NAME "app_cookie"

#endif