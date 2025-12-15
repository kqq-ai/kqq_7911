#ifndef __WSS_CLIENT_CUSTOM_H__
#define __WSS_CLIENT_CUSTOM_H__

#include "cpu.h"
#include "os/os_api.h"
// 注意：不包含 stdbool.h，因为项目中已有 bool 类型定义（unsigned char）
// #include <stdbool.h>

/**
 * WebSocket帧类型（RFC 6455）
 */
typedef enum {
    WS_FRAME_CONTINUATION = 0x0,
    WS_FRAME_TEXT         = 0x1,
    WS_FRAME_BINARY       = 0x2,
    WS_FRAME_CLOSE        = 0x8,
    WS_FRAME_PING         = 0x9,
    WS_FRAME_PONG         = 0xA,
} ws_frame_type_t;

/**
 * WebSocket连接状态
 */
typedef enum {
    WS_STATE_DISCONNECTED = 0,
    WS_STATE_CONNECTING,
    WS_STATE_CONNECTED,
    WS_STATE_CLOSING,
} ws_state_t;

/**
 * WebSocket客户端句柄（不透明结构）
 */
typedef struct ws_client_handle* ws_client_t;

/**
 * 接收数据回调函数类型
 * @param buf 接收到的数据缓冲区
 * @param len 数据长度
 * @param is_text true表示文本帧，false表示二进制帧
 */
typedef void (*ws_recv_callback_t)(u8 *buf, u32 len, bool is_text);

/**
 * 连接断开回调函数类型
 * @param reason 断开原因（可选的错误信息）
 */
typedef void (*ws_disconnect_callback_t)(const char *reason);

/**
 * 创建WebSocket客户端
 * @return WebSocket客户端句柄，失败返回NULL
 */
ws_client_t ws_client_create(void);

/**
 * 连接到WebSocket服务器
 * @param client WebSocket客户端句柄
 * @param url WebSocket URL (ws://host:port/path 或 wss://host:port/path)
 * @param recv_cb 接收数据回调
 * @param disconnect_cb 断开连接回调
 * @return 0成功，负数失败
 */
int ws_client_connect(ws_client_t client, const char *url, 
                     ws_recv_callback_t recv_cb, 
                     ws_disconnect_callback_t disconnect_cb);

/**
 * 发送文本数据
 * @param client WebSocket客户端句柄
 * @param data 要发送的数据
 * @param len 数据长度
 * @return 发送的字节数，负数表示失败
 */
int ws_client_send_text(ws_client_t client, const u8 *data, u32 len);

/**
 * 发送二进制数据
 * @param client WebSocket客户端句柄
 * @param data 要发送的数据
 * @param len 数据长度
 * @return 发送的字节数，负数表示失败
 */
int ws_client_send_binary(ws_client_t client, const u8 *data, u32 len);

/**
 * 发送Ping帧
 * @param client WebSocket客户端句柄
 * @return 0成功，负数失败
 */
int ws_client_send_ping(ws_client_t client);

/**
 * 关闭WebSocket连接
 * @param client WebSocket客户端句柄
 */
void ws_client_disconnect(ws_client_t client);

/**
 * 销毁WebSocket客户端
 * @param client WebSocket客户端句柄
 */
void ws_client_destroy(ws_client_t client);

/**
 * 获取连接状态
 * @param client WebSocket客户端句柄
 * @return 连接状态
 */
ws_state_t ws_client_get_state(ws_client_t client);

#endif /* __WSS_CLIENT_CUSTOM_H__ */

