# WebSocket自定义实现集成日志

## 修改日期
2024年（当前日期）

## 修改内容

### 1. Makefile修改
**文件**: `apps/ling_wl/board/wl82/Makefile`

**修改**: 在 `c_SRC_FILES` 中添加了自定义WebSocket实现源文件
```makefile
../../../../apps/ling_wl/wss/wss_client_custom.c \
```

### 2. wss_app_util.c修改
**文件**: `apps/ling_wl/app_mode/wss_connected/wss_app_util.c`

#### 2.1 添加头文件
- 添加 `#include "wss/wss_client_custom.h"`

#### 2.2 添加全局变量
- `static ws_client_t g_ws_client_custom = NULL;` - 自定义WebSocket客户端句柄

#### 2.3 添加回调函数
- `wss_custom_recv_callback()` - 接收数据回调，调用 `odmp_rsp_handler()` 和 `odmp_audio_data_handler()`
- `wss_custom_disconnect_callback()` - 断开连接回调，调用 `odmp_disconnect_handler()`

#### 2.4 修改的函数

**wss_check_exit()**
- 改为检查 `g_ws_client_custom` 和连接状态
- 使用 `ws_client_get_state()` 检查连接状态

**wss_connect()**
- 使用 `ws_client_create()` 创建客户端
- 使用 `ws_client_connect()` 连接服务器
- 设置回调函数

**wss_disconnect()**
- 使用 `ws_client_disconnect()` 和 `ws_client_destroy()` 断开连接

**wss_reconnect()**
- 使用新的API重新连接

**send_audio_status()**
- 使用 `ws_client_send_text()` 发送文本消息

**send_audio_format()**
- 使用 `ws_client_send_text()` 发送文本消息

**send_req_message()**
- 使用 `ws_client_send_text()` 发送文本消息

**send_bin_data()**
- 使用 `ws_client_send_binary()` 发送二进制数据
- 移除了TCP队列深度检查（新实现内部处理）

**send_ping()**
- 使用 `ws_client_send_text()` 发送应用层ping消息

## 兼容性说明

为了保持向后兼容，`context->websockets_info` 字段仍然被设置（设置为非NULL标记值），但实际不再使用。这样可以避免其他代码检查 `websockets_info == NULL` 时出现问题。

## 注意事项

1. **测试文件**: `apps/ling_wl/app_mode/wss_connected/test/eend_battery_live_test.c` 中仍使用 `websockets_info`，这些是测试代码，可能需要后续更新。

2. **日志输出**: 所有日志都添加了 `[wss_app_util]` 前缀，便于调试。

3. **错误处理**: 发送失败时会断开连接并清理资源，与原有逻辑保持一致。

## 测试建议

1. 编译项目，确保没有编译错误
2. 测试WebSocket连接和断开
3. 测试文本消息发送和接收
4. 测试二进制数据发送和接收
5. 测试重连功能
6. 观察是否还会出现误判DISCONN的问题

## 回滚方法

如果需要回滚到SDK实现：
1. 从Makefile中移除 `wss_client_custom.c`
2. 恢复 `wss_app_util.c` 的原始版本（使用git恢复）

