#ifndef __APP_LINGWL_H__
#define __APP_LINGWL_H__

#include "cpu.h"

struct app_lingwl_hdl {

};
void do_shutdown();

#define WRITE_SN_KEY 0 // 设置为1以强制写入SN/Key，忽略已有SN
#define TEST_LOCOL_MODE 0// 设置为1连接本地调试服务器

#define SERVER_DOMAIN  "test.coolqq.com"
// #define SERVER_DOMAIN  "coolqq.com"

#define PRINT_RSSI_LOG 0 // 是否打印RSSI日志

#endif