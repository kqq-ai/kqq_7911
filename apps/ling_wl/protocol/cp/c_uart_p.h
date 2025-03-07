#ifndef _C_UART_P_H_
#define _C_UART_P_H_

#include "stdint.h"

// 数据包事件类型
typedef enum {
    UART_EVENT_PACKET_RECEIVED,    // 完整数据包接收
    UART_EVENT_CRC_ERROR,          // CRC校验失败
    UART_EVENT_INVALID_CMD,        // 无效命令
    UART_EVENT_OVERFLOW,           // 缓冲区溢出
    UART_EVENT_TIMEOUT             // 接收超时
} uart_event_type_t;

// CMD 命令
typedef enum {
    UART_NONE,
    UART_CMD_NORMAL_KEY        = 0xA0, // 普通按键
    UART_CMD_TOUCH_KEY         = 0xA1, // 触摸按键
    UART_CMD_SYNCH_STATUS      = 0xA2, // 同步状态 ： 电量 充电状态 告知给7911    7911需将工作状态返回给7016（相应包发送）
    UART_CMD_POWER_OFF         = 0xA3, // 关机
    UART_CMD_SET_BLESN         = 0xA4, // 设置蓝牙序列号
} uart_cmd_t;

// 返回码表
typedef enum {
    CPCODE_SUCCESS     = 0x00, // 命令执行成功
    CPCODE_FAILED      = 0x01  // 命令执行失败
} uart_ret_t;

// 协议定义
#define PACKET_HEADER_CMD     0xE5    // 命令包头
#define PACKET_HEADER_RESP    0x5E    // 响应包头
#define PACKET_TRAILER_CMD    0x7A    // 命令包尾
#define PACKET_TRAILER_RESP   0xA7    // 响应包尾

// 数据包回调函数类型
typedef void (*uart_packet_handler_t)(uint8_t cmd, uint8_t *payload, uint16_t len);
typedef void (*uart_resp_packet_handler_t)(uint8_t cmd, uint8_t code, uint8_t *payload, uint16_t len);
typedef void (*uart_event_handler_t)(uart_event_type_t event);

/* 注册回调函数 */
void uart_register_packet_handler(uart_packet_handler_t handler);
void uart_register_resp_packet_handler(uart_resp_packet_handler_t handler);
void uart_register_event_handler(uart_event_handler_t handler);

/* 回调函数 */

void handle_packet(uint8_t cmd, uint8_t *payload, uint16_t len);
void handle_resp_packet(uint8_t cmd, uint8_t code, uint8_t *payload, uint16_t len);
void handle_event(uart_event_type_t event);

// 发送响应包
int uart_send_response(uint8_t cmd, uint8_t code, uint8_t *payload, uint16_t len);

// 针对CMD 指定的发送函数
/* 1. 获取设备状态 的响应函数 */
int uart_response_synch_status(uint8_t busy_status);


int uart_send_set_sn(char *sn);

// 发送事件包
int uart_send_inquiry(uint8_t *payload, uint16_t len);

#endif