# ws_recv_thread 函数与 SSL 读取错误流程文档

## 1. ws_recv_thread 函数工作流程

### 1.1 函数初始化阶段

1. **线程启动**（第1045行）
   - 函数接收 `ws_client_t` 类型的参数作为客户端句柄
   - 分配接收缓冲区 `frame_buf`，大小为 `WS_RECV_BUF_SIZE`（32KB）
   - 分配响应缓冲区 `resp_buf`，大小为 1024 字节，用于构建自动响应（PING/CLOSE）
   - 初始化退出原因字符串 `exit_reason`，默认值为 "io_thread_exit"

2. **内存分配检查**（第1056-1064行）
   - 如果缓冲区分配失败，记录错误日志
   - 设置 `client->thread_running = false`
   - 设置退出原因为 "alloc_failed"
   - 释放已分配的内存并返回

### 1.2 主循环阶段

3. **循环条件检查**（第1068行）
   - 循环条件：`client->thread_running == true` 且 `client->state` 为 `WS_STATE_CONNECTED` 或 `WS_STATE_CLOSING`
   - 每次循环开始时初始化 `fd_set` 结构体

4. **Socket 文件描述符检查**（第1073-1075行）
   - 获取 `client->sock_fd`
   - 如果 `fd < 0`，退出循环

5. **设置 select 监听**（第1077-1086行）
   - 将 socket 文件描述符添加到 `read_fds` 集合
   - 检查发送 RingBuffer 是否有数据需要发送
   - 如果有数据需要发送，将 socket 添加到 `write_fds` 集合

6. **select 调用**（第1088-1092行）
   - 设置超时时间为 50ms
   - 调用 `select()` 监听 socket 的可读/可写事件
   - 如果 `select()` 返回错误（`ret < 0`），记录错误并退出循环

7. **处理写事件**（第1100-1137行）
   - 如果 `select()` 返回大于 0 且 `write_fds` 中有事件：
     - 获取互斥锁
     - 从发送 RingBuffer 中 peek 数据
     - 根据是否使用 SSL，调用 `mbedtls_ssl_write()` 或 `send()` 发送数据
     - 如果发送成功，调用 `rb_advance()` 移动读指针
     - 如果发送失败且不是 `WANT_READ/WANT_WRITE` 错误，记录错误日志
     - 释放互斥锁

8. **处理读事件**（第1139-1267行）
   - 如果 `select()` 超时（`ret == 0`），继续下一轮循环
   - 如果 `select()` 返回大于 0 且 `read_fds` 中有事件：
     - **SSL 模式读取**（第1145-1164行）：
       - 调用 `mbedtls_ssl_read()` 读取数据
       - 如果返回 `MBEDTLS_ERR_SSL_WANT_READ` 或 `MBEDTLS_ERR_SSL_WANT_WRITE`，将 `n` 设为 0（忽略）
       - 如果返回 0，表示连接 EOF，记录日志并退出循环
       - 如果返回小于 0 且不等于 `MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY`，记录错误日志并退出循环
     - **TCP 模式读取**（第1165-1186行）：
       - 调用 `recv()` 读取数据
       - 如果 `errno` 为 `EAGAIN` 或 `EWOULDBLOCK`，将 `n` 设为 0
       - 如果返回错误，记录错误日志并退出循环
       - 如果返回 0，表示连接关闭，记录日志并退出循环

9. **WebSocket 帧解析**（第1188-1265行）
   - 如果读取到数据（`n > 0`）：
     - 更新 `client->recv_buf_len`
     - 循环解析 WebSocket 帧：
       - 调用 `parse_ws_frame()` 解析帧
       - 如果返回 0，表示数据不完整，退出解析循环
       - 如果返回小于 0，表示帧格式错误，设置 `thread_running = false` 并退出
       - 根据帧类型处理：
         - `WS_FRAME_TEXT` / `WS_FRAME_BINARY`：调用接收回调函数
         - `WS_FRAME_CLOSE`：构建 CLOSE 响应帧，设置 `thread_running = false`，跳转到 `thread_exit`
         - `WS_FRAME_PING`：构建 PONG 响应帧并写入发送 RingBuffer
         - `WS_FRAME_PONG`：仅记录日志
       - 从接收缓冲区中移除已处理的帧数据

### 1.3 线程退出阶段

10. **清理资源**（第1270-1298行）
    - 记录线程退出日志
    - 释放 `frame_buf` 和 `resp_buf`
    - 调用断开回调函数 `disconnect_cb`，传入退出原因
    - 设置 `thread_running = false`
    - 获取互斥锁进行清理：
      - 如果使用 SSL，调用 `mbedtls_ssl_close_notify()` 发送关闭通知
      - 释放 SSL 相关资源
      - 关闭 socket 文件描述符
      - 设置 `client->state = WS_STATE_DISCONNECTED`
    - 释放互斥锁

## 2. SSL 读取错误 -0x004c 产生流程

### 2.1 错误码定义

- **错误码**：`-0x004c`（十进制：-76）
- **mbedTLS 错误常量**：`MBEDTLS_ERR_NET_RECV_FAILED`
- **含义**：底层网络接收失败

### 2.2 错误产生路径

#### 路径1：底层 socket recv() 调用失败

1. **mbedtls_ssl_read() 调用**（第1146行）
   ```
   n = mbedtls_ssl_read(&client->ssl, client->recv_buf + client->recv_buf_len,
                       WS_RECV_BUF_SIZE - client->recv_buf_len);
   ```

2. **mbedTLS 内部调用 my_net_recv()**（第525-536行）
   - mbedTLS 库内部会调用注册的 BIO 回调函数 `my_net_recv()`
   - `my_net_recv()` 函数执行以下步骤：
     - 检查文件描述符有效性（第527行）
     - 调用系统调用 `recv(fd, buf, len, 0)`（第529行）

3. **recv() 返回错误**（第530行）
   - 如果 `recv()` 返回值小于 0，进入错误处理分支

4. **errno 检查**（第531-534行）
   - 如果 `errno == EWOULDBLOCK || errno == EAGAIN`：
     - 返回 `MBEDTLS_ERR_SSL_WANT_READ`（第531行）
   - 如果 `errno == EPIPE || errno == ECONNRESET`：
     - 返回 `MBEDTLS_ERR_NET_CONN_RESET`（第532行）
   - 如果 `errno == EINTR`：
     - 返回 `MBEDTLS_ERR_SSL_WANT_READ`（第533行）
   - **其他 errno 值**：
     - 返回 `MBEDTLS_ERR_NET_RECV_FAILED`（第534行）← **错误 -0x004c 产生点**

5. **错误向上传播**（第1156-1163行）
   - `mbedtls_ssl_read()` 返回 `MBEDTLS_ERR_NET_RECV_FAILED`（即 -0x004c）
   - 进入 `else if (n <= 0)` 分支（第1156行）
   - 检查 `n != MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY && n != 0`（第1157行）
   - 条件满足，执行错误日志记录（第1158-1161行）：
     ```
     char ebuf[128];
     log_e("[ws_custom]SSL read error: -0x%04x (%s)", -n,
           ws_mbedtls_errstr(n, ebuf, sizeof(ebuf)));
     ```
   - 设置退出原因（第1161行）：
     ```
     snprintf(exit_reason, sizeof(exit_reason), "ssl_read_error=-0x%04x", -n);
     ```
   - 执行 `break` 退出循环（第1163行）

6. **线程退出流程**
   - 跳出主循环（第1068行）
   - 执行清理代码（第1270-1298行）
   - 调用断开回调函数，传入退出原因 "ssl_read_error=-0x004c"

### 2.3 错误日志输出

当错误发生时，会输出以下日志：
```
[ws_custom]SSL read error: -0x004c (mbedTLS error (enable MBEDTLS_ERROR_C for details))
```

**日志产生位置**：第1159行

**日志格式说明**：
- `-0x004c`：错误码的十六进制表示（取反后显示）
- 括号内的字符串：通过 `ws_mbedtls_errstr()` 函数获取的错误描述
- 如果未启用 `MBEDTLS_ERROR_C` 宏，错误描述为 "mbedTLS error (enable MBEDTLS_ERROR_C for details)"

### 2.4 触发条件

错误 `-0x004c` 会在以下条件下触发：

1. **底层 socket recv() 系统调用返回错误**
2. **errno 值不是以下之一**：
   - `EWOULDBLOCK`
   - `EAGAIN`
   - `EPIPE`
   - `ECONNRESET`
   - `EINTR`

3. **可能的 errno 值包括**（但不限于）：
   - `ENOMEM`：内存不足
   - `EBADF`：无效的文件描述符
   - `EFAULT`：无效的内存地址
   - `ENETDOWN`：网络接口关闭
   - `ENETUNREACH`：网络不可达
   - 其他系统级错误

## 3. 函数调用链

### 3.1 正常读取流程
```
ws_recv_thread()
  └─> select() [监听socket事件]
      └─> mbedtls_ssl_read()
          └─> my_net_recv() [BIO回调]
              └─> recv() [系统调用]
                  └─> 返回成功，数据写入缓冲区
```

### 3.2 错误 -0x004c 产生流程
```
ws_recv_thread()
  └─> select() [返回可读事件]
      └─> mbedtls_ssl_read()
          └─> my_net_recv() [BIO回调]
              └─> recv() [系统调用]
                  └─> 返回 -1，errno 为非常见错误值
                      └─> 返回 MBEDTLS_ERR_NET_RECV_FAILED (-0x004c)
                          └─> 记录错误日志
                              └─> 退出循环，清理资源
```

## 4. 相关代码位置

- **ws_recv_thread 函数**：第1045-1298行
- **my_net_recv 函数**：第525-536行
- **SSL 读取错误处理**：第1145-1164行
- **错误日志输出**：第1159行
- **退出原因设置**：第1161行

