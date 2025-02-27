#ifndef _APP_UART_TASK_H_
#define _APP_UART_TASK_H_

#include "uart.h"
// #include "c_uart_p.h"

#define ENABLE_APP_UART_TASK 1
#define UART_BLOCK_CONFG     1 /* 0 : 不阻塞    1 : 阻塞 */
#define UART_RECV_TIMEOUT    1000 /* 串口接收 超时时间 */

#define UART_RECV_BUF_SIZE 1024

#define MAX_PAYLOAD_LEN       256     // 最大有效载荷长度




// 初始化UART并注册回调函数
void *app_uart_dev_init(const char *device_name);


#endif