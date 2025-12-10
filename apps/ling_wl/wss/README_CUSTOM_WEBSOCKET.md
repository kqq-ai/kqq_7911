# 自定义WebSocket实现使用说明

## 问题背景

SDK自带的WebSocket实现存在误判DISCONN的问题：服务器没有发送关闭帧，但SDK错误地解析了数据包，将正常数据误判为DISCONN（0x08）类型，导致连接被意外断开。

## 解决方案

本实现基于**RFC 6455标准**，使用**lwIP socket API**直接实现WebSocket客户端，完全绕过SDK的WebSocket库，避免误判问题。

## 特性

- ✅ 完全符合RFC 6455标准
- ✅ 支持WS和WSS（SSL/TLS）
- ✅ 正确处理WebSocket帧解析，避免误判
- ✅ 支持文本和二进制数据传输
- ✅ 支持Ping/Pong心跳
- ✅ 线程安全的API设计
- ✅ 详细的日志输出，便于调试

## 文件说明

- `wss_client_custom.h` - WebSocket客户端API头文件
- `wss_client_custom.c` - WebSocket客户端实现
- `wss_client_custom_example.c` - 使用示例

## 使用方法

### 1. 在Makefile中添加源文件

编辑 `apps/ling_wl/board/wl82/Makefile`，在 `c_SRC_FILES` 中添加：

```makefile
c_SRC_FILES := \
    ...
    ../../../../apps/ling_wl/wss/wss_client_custom.c \
    ...
```

### 2. 替换现有的WebSocket调用

#### 原来的代码（使用SDK实现）：

```c
#include "wss/wss_client.h"

struct websocket_struct *ws = wss_server_connect(url);
wss_send_message(ws, buf, len);
wss_server_disconnect(ws);
```

#### 新的代码（使用自定义实现）：

```c
#include "wss/wss_client_custom.h"

// 创建客户端
ws_client_t client = ws_client_create();

// 连接服务器
ws_client_connect(client, url, recv_callback, disconnect_callback);

// 发送数据
ws_client_send_text(client, buf, len);      // 文本数据
ws_client_send_binary(client, buf, len);   // 二进制数据

// 断开连接
ws_client_disconnect(client);
ws_client_destroy(client);
```

### 3. 实现回调函数

```c
// 接收数据回调
void recv_callback(u8 *buf, u32 len, bool is_text)
{
    if (is_text) {
        // 处理文本数据
        odmp_rsp_handler(buf, len);
    } else {
        // 处理二进制数据
        odmp_audio_data_handler(buf, len);
    }
}

// 断开连接回调
void disconnect_callback(const char *reason)
{
    log_i("WebSocket disconnected: %s", reason);
    odmp_disconnect_handler();
}
```

### 4. 在wss_app_util.c中集成

修改 `apps/ling_wl/app_mode/wss_connected/wss_app_util.c`：

```c
#include "wss/wss_client_custom.h"

static ws_client_t g_ws_client = NULL;

bool wss_connect() {
    if (g_ws_client) {
        ws_client_disconnect(g_ws_client);
        ws_client_destroy(g_ws_client);
    }
    
    g_ws_client = ws_client_create();
    if (!g_ws_client) {
        return false;
    }
    
    char *url = get_wss_connection_url();
    int ret = ws_client_connect(g_ws_client, url, 
                                wss_recv_callback, 
                                wss_exit_callback);
    return (ret == 0);
}

void wss_disconnect() {
    if (g_ws_client) {
        ws_client_disconnect(g_ws_client);
        ws_client_destroy(g_ws_client);
        g_ws_client = NULL;
    }
}

int wss_send_message(const u8 *buf, int size) {
    if (!g_ws_client) return -1;
    return ws_client_send_text(g_ws_client, buf, size);
}

int wss_send_bin(const u8 *buf, int size) {
    if (!g_ws_client) return -1;
    return ws_client_send_binary(g_ws_client, buf, size);
}
```

## API参考

### ws_client_t ws_client_create(void)

创建WebSocket客户端实例。

**返回值：** 成功返回客户端句柄，失败返回NULL

### int ws_client_connect(ws_client_t client, const char *url, 
                         ws_recv_callback_t recv_cb, 
                         ws_disconnect_callback_t disconnect_cb)

连接到WebSocket服务器。

**参数：**
- `client`: WebSocket客户端句柄
- `url`: WebSocket URL（格式：`ws://host:port/path` 或 `wss://host:port/path`）
- `recv_cb`: 接收数据回调函数
- `disconnect_cb`: 断开连接回调函数

**返回值：** 0表示成功，负数表示失败

### int ws_client_send_text(ws_client_t client, const u8 *data, u32 len)

发送文本数据。

**返回值：** 发送的字节数，负数表示失败

### int ws_client_send_binary(ws_client_t client, const u8 *data, u32 len)

发送二进制数据。

**返回值：** 发送的字节数，负数表示失败

### int ws_client_send_ping(ws_client_t client)

发送Ping帧（用于心跳检测）。

**返回值：** 0表示成功，负数表示失败

### void ws_client_disconnect(ws_client_t client)

关闭WebSocket连接。

### void ws_client_destroy(ws_client_t client)

销毁WebSocket客户端，释放所有资源。

### ws_state_t ws_client_get_state(ws_client_t client)

获取连接状态。

**返回值：**
- `WS_STATE_DISCONNECTED` - 未连接
- `WS_STATE_CONNECTING` - 连接中
- `WS_STATE_CONNECTED` - 已连接
- `WS_STATE_CLOSING` - 关闭中

## 调试建议

1. **启用日志**：代码中已包含详细的日志输出，使用`log_i`、`log_e`等宏
2. **检查帧解析**：如果仍有问题，可以在`parse_ws_frame`函数中添加更详细的日志
3. **网络抓包**：使用Wireshark等工具抓包，对比服务器发送的实际数据和解析结果

## 注意事项

1. **内存管理**：确保在不再使用时调用`ws_client_destroy`释放资源
2. **线程安全**：API内部已使用互斥锁保护，但回调函数中不要进行耗时操作
3. **URL格式**：确保URL格式正确，WSS需要有效的SSL证书
4. **缓冲区大小**：默认接收缓冲区32KB，发送缓冲区16KB，可根据需要调整

## 故障排查

### 问题：连接失败

- 检查URL格式是否正确
- 检查网络连接是否正常
- 检查防火墙设置
- 查看日志中的错误信息

### 问题：仍然出现DISCONN

- 检查服务器发送的数据格式是否符合RFC 6455
- 在`parse_ws_frame`函数中添加日志，查看实际接收到的数据
- 确认服务器没有发送CLOSE帧（0x88）

### 问题：SSL连接失败

- 检查mbedTLS配置
- 检查CA证书设置
- 查看SSL握手日志

## 与SDK实现的区别

| 特性 | SDK实现 | 自定义实现 |
|------|---------|-----------|
| 帧解析 | 可能有误判 | 严格按照RFC 6455 |
| 源码可见性 | 闭源 | 开源 |
| 调试能力 | 有限 | 完全可控 |
| 维护性 | 依赖SDK更新 | 自主维护 |

## 技术支持

如有问题，请：
1. 查看日志输出
2. 使用Wireshark抓包分析
3. 检查RFC 6455标准实现细节

