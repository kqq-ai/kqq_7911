# WebSocket 错误代码 -80 分析文档

## 错误信息
```
[WEBSOCKET]  . ! data send err mbtls ret = -80!!!
```

## 错误代码定义

**错误代码：`-80` (即 `-0x0050`)**

来源文件：`include_lib/net/mbedtls_2_2_1/mbedtls/net.h`

```c
#define MBEDTLS_ERR_NET_CONN_RESET  -0x0050  /**< Connection was reset by peer. */
```

## 错误含义

**连接被对端重置** (Connection was reset by peer)

这个错误发生在 WebSocket 数据发送阶段，通过 mbedTLS 进行 SSL/TLS 加密传输时，表示 TCP 连接被远程对端强制关闭或重置。

## 常见原因

### 1. **服务器端异常**
- WebSocket 服务器崩溃或异常退出
- 服务器处理数据时出现严重错误
- 服务器主动关闭了连接

### 2. **SSL/TLS 握手或认证失败**
- CA 证书验证失败
- 服务器证书无效或过期
- TLS 版本协商失败
- 密码套件不兼容

### 3. **网络传输问题**
- 网络连接不稳定，在发送数据过程中中断
- 防火墙或网关主动中断连接
- 代理服务器干预连接

### 4. **会话超时**
- 长时间没有数据交互，连接被服务器关闭
- keepalive 探针失败

### 5. **数据包大小问题**
- 发送的数据包过大（超过 SSL/TLS 分段限制）
- 根据 `wss_client.c` 中的注释，SSL 加密时一次发送数据不能超过 16KB

### 6. **内存或资源不足**
- 系统内存耗尽
- 缓冲区分配失败

## 对应代码位置

文件：[apps/ling_wl/wss/wss_client.c](../apps/ling_wl/wss/wss_client.c)

关键函数：
- `wss_send_message()` - 发送文本消息
- `wss_send_bin()` - 发送二进制数据
- `websockets_client_send()` - 底层发送函数（包含断言检查 16KB 限制）

```c
static int websockets_client_send(struct websocket_struct *websockets_info, u8 *buf, int len, char type)
{
    //SSL加密时一次发送数据不能超过16K，用户需要自己分包
    assert(len < 160000);
    int ret = websockets_info->_send(websockets_info, buf, len, type);
    if (ret < 0) {
        log_i("[wss_client]send failed type=0x%x len=%d ret=%d", type, len, ret);
    } else if (ret != len) {
        log_i("[wss_client]send partial type=0x%x len=%d ret=%d", type, len, ret);
    }
    return ret;
}
```

## 调试步骤

### 1. 检查服务器状态
```bash
# 确认 WebSocket 服务器正常运行
# 确认服务器监听正确的端口
# 查看服务器日志中是否有异常
```

### 2. 验证 SSL/TLS 配置
- 检查 CA 证书是否正确配置
- 验证证书是否过期
- 确认客户端和服务器的 TLS 版本兼容

### 3. 检查数据大小
- 确保单次发送的数据不超过 16KB
- 如果需要发送更大数据，应该在应用层进行分包

### 4. 网络诊断
```bash
# 检查网络连接
ping <server_address>

# 检查端口是否开放
telnet <server_address> <port>
```

### 5. 启用详细日志
- 启用 mbedTLS 的调试日志
- 查看 WebSocket 库的详细日志输出
- 记录连接建立和断开的时间点

### 6. 监视连接生命周期
- 关注 `wss_exit_callback()` 的调用时机
- 检查是否有异常的线程终止
- 验证 ping/pong 心跳机制是否正常

## 相关回调函数

| 函数 | 作用 |
|-----|------|
| `wss_recv_callback()` | 接收数据回调，处理文本/二进制/ping/pong/close 帧 |
| `wss_exit_callback()` | 连接断开通知回调 |
| `wss_send_pong()` | 响应 ping 帧的 pong 发送 |

## 后续连锁反应

从日志分析可以看到，错误 -80 导致的连锁反应：

```
[10:41:32.488](error): <Error>: [WEBSOCKET]  . ! data send err mbtls ret = -80!!!
[10:41:32.489][RECV RST]          // TCP 重置包持续到达
[10:41:32.497][wss_client]send partial type=0x1 len=175 ret=0
[10:41:32.498](error): Error ../../../../apps/ling_wl/app_mode/wss_connected/wss_app_util.c 200.
[10:41:32.499][wss_client]server_disconnect threads(ping=0 recv=67963968)
[10:41:32.504]sock_send hdl err
[10:41:32.504](error): <Error>: [WEBSOCKET]  . ! data send err ret = -1!!!
```

### 堆栈信息分析

```
[10:41:32.511]psr:  0
[10:41:32.511]rets: 0x2000462  // log_putbyte (log.c:418)
[10:41:32.511]reti: 0x2000452  // uart_putbyte (uart.c:361) inlined at uart.c:367
[10:41:32.511]exception reason : axi_rd_inv  // AXI 读取无效
[10:41:32.511]system_reset...
```

**意义**：
- `rets` 和 `reti` 显示程序在 UART 日志输出函数中崩溃
- `axi_rd_inv` 表示非法内存访问（可能是内存指针损坏）
- 系统被迫重启

### 可能的根本原因

1. **堆内存损坏**
   - WebSocket 发送缓冲区溢出
   - 内存块越界写入
   - 释放后使用 (use-after-free)

2. **连接断开时的资源清理失败**
   - `wss_server_disconnect()` 可能在清理过程中访问了无效内存
   - 线程 ID 异常 (recv=67963968) 可能指示内存污染

3. **并发访问冲突**
   - WebSocket 接收线程与主线程的同步问题
   - 互斥锁操作异常

## 总结

错误代码 -80 表示在 WebSocket 数据发送过程中，TCP 连接被远程对端重置。此错误会导致：

1. **直接影响**
   - 数据发送失败 (ret=0)
   - 连接断开通知

2. **间接影响**
   - 可能导致内存损坏或资源泄漏
   - 系统在尝试输出日志时崩溃

解决此问题需要：

1. **确认服务器稳定运行** - 检查服务器日志，确保不存在异常
2. **验证 SSL/TLS 配置** - 确保证书和密钥正确配置
3. **控制数据包大小** - 单次发送不超过 16KB，大数据需分包
4. **稳定网络连接** - 排除网络中断因素，避免频繁连接/断开
5. **增加错误处理和重连逻辑**
   - 优雅处理连接重置错误
   - 实现指数退避的重连机制
   - 添加健康检查和心跳机制
6. **防止内存泄漏和资源污染**
   - 检查 WebSocket 断开连接时的资源清理逻辑
   - 使用内存检测工具排查堆损坏问题
   - 验证多线程同步的正确性

---

文档更新日期：2025-12-28 (含堆栈分析和后续连锁反应)
