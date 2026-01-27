# BLE 配网通信机制文档

## 概述

设备通过 BLE 与小程序进行 WiFi 配网。设备作为 BLE Server，小程序作为 Client。

### 核心特性

1. **自动扫描**：设备 STA 模式启动后自动循环扫描 WiFi，无需小程序触发
2. **缓冲累积**：扫描结果自动累积到设备缓冲（最多 50 个），持续更新
3. **连接即推送**：BLE 连接建立后立即推送当前缓冲的 WiFi 列表
4. **实时更新**：扫描过程中检测到列表变化自动推送，小程序无需轮询

---

## BLE 服务架构

### GATT 特征值

| 特征值   | 方向          | 用途                          |
| -------- | ------------- | ----------------------------- |
| `0xFFF1` | 小程序 → 设备 | 接收 JSON 命令                |
| `0xFFF2` | 设备 → 小程序 | 发送 JSON 响应（通过 Notify） |
| CCC 配置 | 小程序 → 设备 | 启用/禁用通知                 |

**服务 UUID**: `0xAF30`

---

## 通信协议

### 数据包格式

每包 **20 字节**，格式：
```
[序号2B][总数2B][数据16B]
```

示例：`"0105{"cmd_name":"wi..."`

### JSON 格式

**命令格式**（小程序 → 设备）：
```json
{
  "cmd_name": "命令名",
  "cmd_args": { "参数": "值" }
}
```

**响应格式**（设备 → 小程序）：
```json
{
  "cmd_name": "命令名",
  "return_code": 0,
  "return_errstr": "状态描述",
  "数据字段": "数据值"
}
```

---

## 核心流程

### WiFi 自动扫描与缓冲

```
设备启动 → 进入 STA 模式
     ↓
自动开始循环扫描（无需小程序命令）
     ↓
每次扫描完成 → 触发 WIFI_EVENT_STA_SCAN_COMPLETED
     ↓
调用 store_wifi_scan_result() 累积到缓冲
     ↓
- 新 WiFi：添加到缓冲（最多 50 个）
- 已有 WiFi：更新 RSSI、信道
- 按 RSSI 降序排序
     ↓
检测列表变化（新增 / RSSI±10dBm / 信道变化）
     ↓
如果 BLE 已连接且有变化 → 推送 wifi_list_update
```

### BLE 连接后推送流程

```
小程序连接 BLE
     ↓
开启通知（Write CCC = 0x01）
     ↓
设备检测到连接 && 通知已启用
     ↓
立即调用 notify_app_wifi_list_update()
     ↓
推送当前缓冲列表（即使为空也推送）
     ↓
小程序显示 WiFi 选择界面
```

### WiFi 配网流程

```
小程序发送 wifi_connect 命令
     ↓
设备返回 WIFI_CONNECT_CMD_GOT（命令已收到）
     ↓
设备执行 WiFi 连接
     ↓
成功：
  - 推送 wifi_connect 响应（包含 IP 地址）
  - 推送 mcu_notify_app_mqtt（MQTT 连接成功）
  - 断开 BLE 连接
失败：
  - 推送 wifi_connect 响应（包含错误码）
```

---

## 配网时序图

```
小程序                          设备
  |                              |
  |--- 连接 BLE ----------------->|
  |--- 开启通知 (CCC) ----------->|
  |                              |
  |                              | [自动扫描中，缓冲累积]
  |                              |
  |<-- wifi_list_update ---------| 立即推送缓冲列表
  |    {wifi_list}               |
  |                              |
  | [用户选择 WiFi]               |
  |                              |
  |<-- wifi_list_update ---------| 扫描更新时自动推送
  |    {wifi_list}               |
  |                              |
  |--- wifi_connect ------------->|
  |    {ssid, password}          |
  |                              |
  |<-- 命令收到 (CMD_GOT) --------| 
  |                              |
  |                              | [连接 WiFi...]
  |                              |
  |<-- wifi_connect 成功 ---------| 包含 IP 地址
  |                              |
  |                              | [连接 MQTT...]
  |                              |
  |<-- mcu_notify_app_mqtt ------| 
  |                              | [断开 BLE]
```

---

## 主要命令

### 小程序发送的命令

| 命令              | 参数                          | 说明               |
| ----------------- | ----------------------------- | ------------------ |
| `wifi_get_list`   | `details`: 0/1                | 手动获取 WiFi 列表 |
| `wifi_connect`    | `ssid`, `password`, `timeout` | 连接 WiFi          |
| `wifi_disconnect` | -                             | 断开 WiFi          |
| `wifi_info`       | -                             | 获取设备网络信息   |

**注意**：无需发送 `wifi_scan` 命令，设备自动扫描。

### 设备主动推送的通知

| 通知                  | 触发时机                      | 关键字段                 |
| --------------------- | ----------------------------- | ------------------------ |
| `wifi_list_update`    | BLE 连接后 / 扫描列表变化     | `wifi_list` 数组         |
| `wifi_connect`        | WiFi 连接成功                 | `ip_address`             |
| `wifi_connect`        | WiFi 连接失败                 | `return_code: 1`, 错误码 |
| `mcu_notify_app_mqtt` | MQTT 连接成功（随后断开 BLE） | -                        |

---

## WiFi 列表更新通知详解

### 触发条件

1. **BLE 连接后立即推送**（首次）
   - 连接建立 && CCC 已启用
   - 推送当前缓冲列表（可能为空）

2. **扫描过程中自动推送**（后续）
   - BLE 已连接
   - 检测到列表变化：
     - 新增 WiFi
     - RSSI 变化 ≥ 10dBm
     - 信道变化

### 通知内容

```json
{
  "cmd_name": "wifi_list_update",
  "return_code": 0,
  "return_errstr": "WIFI_LIST_UPDATE_CODE_SUCCESS",
  "wifi_list": [
    {
      "ssid": "WiFi名称",
      "rssi": -45,
      "channel": 6
    }
  ]
}
```

**错误码**：
- `WIFI_LIST_UPDATE_CODE_SUCCESS` - 列表有内容
- `WIFI_LIST_UPDATE_CODE_EMPTY` - 列表为空

---

## WiFi 连接结果通知详解

### 成功通知

```json
{
  "cmd_name": "wifi_connect",
  "return_code": 0,
  "return_errstr": "WIFI_CONNECT_CODE_SUCCESS",
  "ip_address": "192.168.1.100"
}
```

### 失败通知

```json
{
  "cmd_name": "wifi_connect",
  "return_code": 1,
  "return_errstr": "错误描述"
}
```

**常见错误码**：
- `WIFI_CONNECT_CODE_PASSWORD_ERROR` - 密码错误
- `WIFI_CONNECT_CODE_TIMEOUT` - 连接超时
- `WIFI_CONNECT_CODE_NO_SSID` - 找不到该 SSID
- `WIFI_CONNECT_CODE_DHCP_TIMEOUT` - DHCP 超时

---

## 核心函数

### 数据接收处理链路

```
att_write_callback()           BLE 写数据回调
    ↓
parse_and_assemble()           组装分片数据
    ↓
parse_and_dispatch_json()      解析 JSON 并分发
    ↓
handle_xxx()                   各命令处理函数
```

### 数据发送处理链路

```
notify_xxx() / handle_xxx()    生成响应数据
    ↓
send_return_json_data()        分片打包
    ↓
app_send_user_data()           BLE 发送
    ↓
小程序接收并组装
```

### 关键函数列表

| 函数                            | 功能                         |
| ------------------------------- | ---------------------------- |
| `store_wifi_scan_result()`      | 累积扫描结果到缓冲，检测变化 |
| `notify_app_wifi_list_update()` | 推送 WiFi 列表更新           |
| `notify_app_wifi_ip_succ()`     | 推送 WiFi 连接成功（含 IP）  |
| `notify_app_wifi_mqtt_succ()`   | 推送 MQTT 连接成功           |
| `handle_wifi_connect()`         | 处理 WiFi 连接命令           |
| `handle_wifi_get_list()`        | 处理获取列表命令             |
| `send_return_json_data()`       | 分片发送 JSON 数据           |

---

## 技术要点

### 1. 缓冲机制
- **容量**：最多 50 个 WiFi（`STORE_WIFI_NUM_MAX`）
- **策略**：
  - 新 WiFi：添加（未满时）
  - 已有 WiFi：更新 RSSI/信道
  - 缓冲已满：忽略新 WiFi
- **排序**：按 RSSI 降序
- **持久性**：设备启动后持续更新，不依赖 BLE 连接

### 2. 变化检测
- **去抖动**：RSSI 变化阈值 ±10dBm
- **触发条件**：
  - 数量变化
  - RSSI 变化 ≥ 10dBm
  - 信道变化

### 3. 分片传输
- **每包大小**：20 字节
- **有效数据**：16 字节/包
- **最大容量**：99 包 × 16 字节 = 1584 字节
- **包头**：序号（2B）+ 总数（2B）

### 4. 线程安全
- 互斥锁保护 BLE 状态
- WiFi 扫描在独立线程池执行
- 避免阻塞 BLE 事件处理

---

## 小程序开发注意事项

1. **分包重组**：通过包头序号（如 `01`, `02`）正确拼接 JSON 字符串
2. **编码处理**：SSID 可能包含特殊字符或中文
3. **时序依赖**：
   - 必须先开启 Notify (Write CCC) 才能接收数据
   - MQTT 通知后设备会断开 BLE，应提示配网成功
4. **列表更新**：监听 `wifi_list_update` 通知实时更新 UI，无需轮询
5. **空列表处理**：连接后可能收到空列表，应显示"扫描中"提示

---

## 常见问题

| 问题              | 排查方向                                  |
| ----------------- | ----------------------------------------- |
| 收不到数据        | 检查 CCC 是否已配置（通知是否启用）       |
| JSON 解析失败     | 检查分片组装是否完整                      |
| WiFi 列表始终为空 | 检查设备是否已进入 STA 模式，扫描是否启动 |
| 连接后立即断开    | 正常情况，MQTT 连接成功后会主动断开 BLE   |

---

## 总结

BLE 配网通信采用 **自动扫描 + 缓冲累积 + 主动推送** 的架构：

- **小程序端**：连接 BLE 后即可收到 WiFi 列表，发送配网命令，监听结果通知
- **设备端**：自动扫描并缓冲 WiFi，BLE 连接后主动推送，配网成功后断开

整个流程对小程序端透明度高，用户体验流畅。
