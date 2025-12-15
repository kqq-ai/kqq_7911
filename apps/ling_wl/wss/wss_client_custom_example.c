/**
 * WebSocket自定义实现使用示例
 * 展示如何使用新的WebSocket实现替换SDK的实现
 */

#include "wss_client_custom.h"
#include "log.h"

// 示例：接收数据回调
static void example_recv_callback(u8 *buf, u32 len, bool is_text)
{
    if (is_text) {
        log_i("[ws_example]Received text: %.*s", len, buf);
        // 处理文本数据，例如：odmp_rsp_handler(buf, len);
    } else {
        log_i("[ws_example]Received binary: len=%u", len);
        // 处理二进制数据，例如：odmp_audio_data_handler(buf, len);
    }
}

// 示例：断开连接回调
static void example_disconnect_callback(const char *reason)
{
    log_i("[ws_example]WebSocket disconnected: %s", reason);
    // 处理断开连接，例如：odmp_disconnect_handler();
}

/**
 * 示例：使用自定义WebSocket实现连接服务器
 */
void example_websocket_usage(void)
{
    // 1. 创建WebSocket客户端
    ws_client_t client = ws_client_create();
    if (!client) {
        log_e("[ws_example]Failed to create WebSocket client");
        return;
    }
    
    // 2. 连接到服务器
    const char *url = "wss://your-server.com/path"; // 或 "ws://your-server.com/path"
    int ret = ws_client_connect(client, url, example_recv_callback, example_disconnect_callback);
    if (ret < 0) {
        log_e("[ws_example]Failed to connect to WebSocket server");
        ws_client_destroy(client);
        return;
    }
    
    log_i("[ws_example]WebSocket connected successfully");
    
    // 3. 发送数据
    const char *text_msg = "Hello, WebSocket!";
    ws_client_send_text(client, (u8 *)text_msg, strlen(text_msg));
    
    u8 binary_data[] = {0x01, 0x02, 0x03, 0x04};
    ws_client_send_binary(client, binary_data, sizeof(binary_data));
    
    // 4. 发送Ping
    ws_client_send_ping(client);
    
    // 5. 检查连接状态
    ws_state_t state = ws_client_get_state(client);
    log_i("[ws_example]WebSocket state: %d", state);
    
    // 6. 断开连接（当不再需要时）
    // ws_client_disconnect(client);
    
    // 7. 销毁客户端（当完全不再使用时）
    // ws_client_destroy(client);
}

/**
 * 如何替换现有的wss_server_connect调用：
 * 
 * 原来的代码：
 *   struct websocket_struct *ws = wss_server_connect(url);
 *   wss_send_message(ws, buf, len);
 *   wss_server_disconnect(ws);
 * 
 * 替换为：
 *   ws_client_t client = ws_client_create();
 *   ws_client_connect(client, url, wss_recv_callback, wss_exit_callback);
 *   ws_client_send_text(client, buf, len);
 *   ws_client_disconnect(client);
 *   ws_client_destroy(client);
 */

