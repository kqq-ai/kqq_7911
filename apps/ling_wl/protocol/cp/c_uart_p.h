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
    UART_CMD_MOVE_DETECT_EN    = 0xA5, // 使能移动检测
    UART_CMD_MOVE_OVER_A_METER = 0xA6, // 通知检测距离大于一米
    UART_CMD_SET_LED_COLOR     = 0xA7, // 设置LED颜色
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
// 发送事件包
int uart_send_inquiry(uint8_t *payload, uint16_t len);

// 针对CMD 指定的发送函数
/* 1. 获取设备状态 的响应函数 */
int uart_response_synch_status(uint8_t busy_status, uint8_t volume);

/* 2. 设置设备SN 的发送函数 */
int uart_send_set_sn(char *sn);

/* 3. 使能移动检测 的发送函数 */
int uart_send_move_detect_en(uint8_t en);

/* 
    4. 设置LED颜色 的发送函数
    r   0~10000
    g   0~10000
    b   0~10000
*/
// 占空比SDK中是最大10000
// 颜色定义 (R, G, B) —— 共阳极模式（值越高实际输出越低）
// 以下只做参考的RGB值 实际灯光颜色看调整 （r g b的值是根据你们给的表格中的255转化而来，在此基础上根据实际效果努力做了调整，是否满足需求可自行更改）
#define COLOR_CYAN        (10000), (0),     (0)       // 青色(0,255,255)
#define COLOR_RED         (0),     (10000), (10000)   // 红色(255,0,0)
#define COLOR_WHITE       (5000),     (0),     (0)    // 白色(255,255,255)
#define COLOR_GREEN       (10000), (0),     (10000)   // 绿色(0,255,0)
#define COLOR_BLUE        (10000), (10000), (0)       // 蓝色(0,0,255)
#define COLOR_ORANGE      (784),   (6392),  (6392)    // 橙色(235,92,92)
#define COLOR_PINK        (509),   (3882),  (3058)    // 粉红(242,156,177)
#define COLOR_DARK_YELLOW (2980),  (4980),  (10000)   // 深黄(128,128,0)
#define COLOR_MAROON      (4980),  (10000), (10000)   // 栗色(128,0,0)
#define COLOR_DARK_BLUE   (10000), (10000), (4549)    // 深蓝(0,0,139)
#define COLOR_BLACK       (10000), (10000), (10000)   // 黑色(0,0,0)
#define COLOR_BROWN       (3529),  (8352),  (8352)    // 褐色(165,42,42)
#define COLOR_PURPLE      (4980),  (10000), (4980)    // 紫色(128,0,128)
#define COLOR_GRAY        (4980),  (4980),  (4980)    // 灰色(128,128,128)
int uart_send_set_led_color(uint16_t r, uint16_t g, uint16_t b);

#endif