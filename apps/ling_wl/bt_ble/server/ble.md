# 小程序蓝牙通信整体逻辑文档

## 目录
1. [概述](#概述)
2. [BLE服务架构](#ble服务架构)
3. [通信协议](#通信协议)
4. [数据流向](#数据流向)
5. [核心函数说明](#核心函数说明)
6. [命令处理流程](#命令处理流程)
7. [主动通知机制](#主动通知机制)
8. [典型配网流程](#典型配网流程)

---

## 概述

本文档描述了设备通过BLE（低功耗蓝牙）与手机小程序进行网络配网的完整通信机制。设备作为BLE Server，小程序作为BLE Client，通过GATT（通用属性配置文件）进行双向数据通信。

### 主要功能
- WiFi网络自动扫描（STA模式启动时自动开始，持续循环扫描）
- WiFi列表缓冲累积（最多50个WiFi，自动累积更新）
- WiFi列表获取
- WiFi连接配置
- 设备状态查询
- 配网结果主动通知

### 自动扫描机制
- **触发时机**：设备进入STA模式时自动开始WiFi扫描
- **扫描方式**：持续循环扫描，每次扫描完成触发`WIFI_EVENT_STA_SCAN_COMPLETED`事件
- **结果累积**：扫描结果自动累积到缓冲列表（`g_all_wifi_info[]`），最多保存50个WiFi
- **缓冲机制**：缓冲列表在设备启动后持续更新，不依赖小程序连接
- **小程序无需触发**：小程序不再需要发送`wifi_scan`命令，设备自动扫描

---

## BLE服务架构

### GATT特征值定义

设备提供以下BLE特征值用于通信：

| 特征值UUID | 名称 | 方向 | 用途 |
|-----------|------|------|------|
| `0xFFF1` | Write Characteristic | 小程序 → 设备 | 接收小程序发送的JSON命令 |
| `0xFFF2` | Notify Characteristic | 设备 → 小程序 | 发送JSON响应数据给小程序 |
| `0xFFF2` CCC | Client Configuration | 小程序 → 设备 | 配置通知使能（启用/禁用） |

### 服务UUID
- 服务UUID: `0xAF30`（在广播数据中可扫描到）

---

## 通信协议

### 数据包格式

由于BLE单次传输数据量有限，JSON数据需要分片传输。

#### 1. 发送数据包格式（小程序 → 设备）

每个数据包大小为 **20字节**，格式如下：

```
[0-1]  : 当前包序号（2字节ASCII，如 "01", "02"）
[2-3]  : 总包数（2字节ASCII，如 "05", "10"）
[4-19] : JSON数据负载（16字节有效数据）
```

**示例：**
```
包1: "0105{"cmd_name":"wifi_co"
包2: "0205nnect","cmd_args":{"s"
包3: "0305sid":"WiFi","password"
包4: "0405":"123456"}}00000000"
包5: "050500000000000000000000"
```

#### 2. 接收数据包格式（设备 → 小程序）

格式与发送相同，通过BLE通知（Notification）发送。

```
[0-1]  : 当前包序号（2字节ASCII）
[2-3]  : 总包数（2字节ASCII）
[4-19] : JSON响应数据负载（16字节有效数据）
```

### JSON命令格式

小程序发送的命令采用JSON格式：

```json
{
    "cmd_name": "命令名称",
    "cmd_args": {
        "参数名1": "参数值1",
        "参数名2": "参数值2"
    }
}
```

### JSON响应格式

设备返回的响应采用JSON格式：

```json
{
    "cmd_name": "命令名称",
    "return_code": 0,
    "return_errstr": "错误字符串",
    "数据字段": "数据值"
}
```

---

## 数据流向

### 1. 小程序 → 设备（命令下发）

```
小程序
  ↓
分片打包（20字节/包）
  ↓
BLE Write → 特征值 0xFFF1
  ↓
att_write_callback()         [ble_simple_server.c:440]
  ↓
parse_and_assemble()          [ble_data.c:61] - 组装分片数据
  ↓
parse_and_dispatch_json()     [ble_data.c:1038] - 解析JSON并分发命令
  ↓
handle_xxx()                  各种命令处理函数 [ble_data.c]
```

### 2. 设备 → 小程序（响应发送）

```
命令处理函数 / 主动通知函数
  ↓
生成JSON响应数据
  ↓
send_return_json_data()       [ble_simple_server.c:511] - 分片打包
  ↓
app_send_user_data_do()       [ble_simple_server.c:209]
  ↓
app_send_user_data()          [ble_simple_server.c:485]
  ↓
BLE Notify → 特征值 0xFFF2
  ↓
小程序接收并组装数据
```

### 3. 自动扫描与缓冲累积流程

```
STA模式启动
  ↓
WiFi协议栈自动开始扫描
  ↓
每次扫描完成 → WIFI_EVENT_STA_SCAN_COMPLETED 事件
  ↓
store_wifi_scan_result()      [app_wifi_manage.c:367] - 累积到缓冲
  ↓
检测列表变化（新增、RSSI更新、信道更新等）
  ↓
如果BLE已连接且列表有变化
  ↓
notify_app_wifi_list_update() → send_return_json_data()
  ↓
BLE Notify → 特征值 0xFFF2（主动推送）
  ↓
小程序接收并更新UI
```

### 4. 蓝牙连接后立即推送流程

```
BLE连接建立
  ↓
HCI_SUBEVENT_LE_CONNECTION_COMPLETE 事件
  ↓
server_cbk_packet_handler()   [ble_simple_server.c:244] - 处理连接事件
  ↓
通知已启用（CCC已配置）
  ↓
notify_app_wifi_list_update() [ble_simple_server.c:998] - 立即推送缓冲列表
  ↓
send_return_json_data()       [ble_simple_server.c:511] - 分片打包
  ↓
BLE Notify → 特征值 0xFFF2（首次推送）
  ↓
小程序接收初始列表，显示选择界面
```

**注意**：连接成功后立即推送当前缓冲列表，即使列表为空也会推送，让小程序知道当前状态。

---

## 核心函数说明

### 接收侧函数

#### `att_write_callback()` 
**位置**: `ble_simple_server.c:440`
**功能**: BLE写数据回调，接收小程序发送的数据
- 检查特征值Handle（0xFFF1）
- 调用`parse_and_assemble()`组装分片数据
- 调用`parse_and_dispatch_json()`解析并处理命令
- 调用`send_return_json_data()`发送响应

#### `parse_and_assemble()`
**位置**: `ble_data.c:61`
**功能**: 组装分片数据包，还原完整JSON
- 解析包序号和总包数
- 将多包数据组装成完整JSON字符串
- 返回0表示组装完成

#### `parse_and_dispatch_json()`
**位置**: `ble_data.c:1038`
**功能**: 解析JSON命令并分发到对应的处理函数
- 解析JSON获取`cmd_name`和`cmd_args`
- 根据`cmd_name`调用相应的`handle_xxx()`函数
- 将处理结果写入`return_json_data`

### 发送侧函数

#### `send_return_json_data()`
**位置**: `ble_simple_server.c:511`
**功能**: 将JSON响应数据分片打包并通过BLE发送
- 计算需要分包的个数（每包16字节有效数据）
- 为每个包添加序号和总包数（前4字节）
- 调用`app_send_user_data_do()`逐包发送

#### `app_send_user_data()`
**位置**: `ble_simple_server.c:485`
**功能**: BLE底层数据发送函数
- 检查连接状态和CCC配置
- 调用协议栈`ble_op_multi_att_send_data()`发送数据

### 命令处理函数

所有命令处理函数位于`ble_data.c`：

| 函数名 | 命令名称 | 功能 |
|--------|----------|------|
| `handle_wifi_get_list()` | `wifi_get_list` | 获取WiFi列表 |
| `handle_wifi_list_ready()` | `wifi_list_ready` | 检查WiFi列表是否就绪 |
| `handle_wifi_stop_scan()` | `wifi_stop_scan` | 停止WiFi扫描 |
| `handle_wifi_connect()` | `wifi_connect` | 连接WiFi网络 |
| `handle_wifi_disconnect()` | `wifi_disconnect` | 断开WiFi连接 |
| `handle_wifi_reconnect()` | `wifi_reconnect` | 重连WiFi网络 |
| `handle_wifi_get_status()` | `wifi_get_status` | 获取WiFi连接状态 |
| `handle_wifi_info()` | `wifi_info` | 获取WiFi详细信息 |

**注意**：`wifi_scan`命令已移除，WiFi扫描由STA模式自动启动，无需小程序触发。

### 主动通知函数

#### `notify_app_wifi_ip_succ()`
**位置**: `ble_simple_server.c:905`
**功能**: WiFi获取IP成功后主动通知小程序
- 封装`wifi_connect`命令响应
- 包含IP地址信息
- 通过`send_return_json_data()`发送

#### `notify_app_wifi_mqtt_succ()`
**位置**: `ble_simple_server.c:953`
**功能**: MQTT连接成功后主动通知小程序
- 封装`mcu_notify_app_mqtt`命令响应
- 发送完成后主动断开BLE连接

### BLE事件处理

#### `server_cbk_packet_handler()`
**位置**: `ble_simple_server.c:244`
**功能**: 处理BLE协议栈事件
- `HCI_SUBEVENT_LE_CONNECTION_COMPLETE`: 连接建立事件
  - 连接建立后，如果通知已启用，立即调用`notify_app_wifi_list_update()`推送缓冲列表
- `HCI_EVENT_DISCONNECTION_COMPLETE`: 断开连接事件
- `ATT_EVENT_CAN_SEND_NOW`: 可以发送数据事件
- `ATT_EVENT_MTU_EXCHANGE_COMPLETE`: MTU协商完成事件

#### `notify_app_wifi_list_update()`
**位置**: `ble_simple_server.c:998`
**功能**: 推送WiFi列表更新通知给小程序
- 获取当前缓冲列表（`g_all_wifi_info[]`）
- 封装为`wifi_list_update`命令格式
- 通过`send_return_json_data()`发送
- **触发时机**：
  1. BLE连接建立且通知已启用后立即推送（首次推送）
  2. 扫描过程中检测到列表变化时推送（后续更新）

---

## 命令处理流程

### WiFi自动扫描与缓冲机制

```
1. STA模式启动（设备启动时）:
   - WiFi协议栈自动开始循环扫描
   - 无需小程序命令触发
   ↓
2. 每次扫描完成:
   - 触发 WIFI_EVENT_STA_SCAN_COMPLETED 事件
   - 调用 store_wifi_scan_result() 累积WiFi列表到缓冲
   ↓
3. 缓冲列表累积机制:
   - 新扫描到的WiFi：如果SSID不存在，添加到缓冲（最多50个）
   - 已存在的WiFi：更新RSSI、信道等信息
   - 缓冲已满时：忽略新WiFi（不添加）
   - 所有WiFi按RSSI降序排序
   ↓
4. 蓝牙连接成功后:
   - 设备立即推送当前缓冲列表（即使为空也推送）
   - 小程序收到初始列表，显示选择界面
   ↓
5. 扫描过程中缓冲更新:
   - 检测列表变化（新增、RSSI更新、信道更新等）
   - 如果BLE已连接且列表有变化，主动推送 wifi_list_update 通知
   - 小程序实时接收更新，无需轮询
   ↓
6. （可选）小程序手动刷新:
   - 小程序发送: {"cmd_name":"wifi_get_list","cmd_args":{"details":0}}
   - handle_wifi_get_list() 返回当前缓冲中的完整列表
```

### WiFi连接流程

```
1. 小程序发送: {"cmd_name":"wifi_connect","cmd_args":{"ssid":"xxx","pwd":"xxx"}}
   ↓
2. handle_wifi_connect() 处理:
   - 验证参数
   - 调用 wifi_connect_block() 连接WiFi
   - 返回连接状态响应
   ↓
3. WiFi连接结果:
   - 成功: 触发 notify_app_wifi_ip_succ() 主动通知
   - 失败: 返回错误码和错误信息
```

---

## 主动通知机制

设备在特定事件发生时主动通知小程序，无需小程序主动查询。

### 1. WiFi列表更新通知

**触发时机**:
1. **蓝牙连接成功后立即推送**：BLE连接建立且通知已启用后，立即推送当前缓冲列表（即使为空也推送）
2. **扫描过程中缓冲更新**：WiFi扫描结果有变化时（新增WiFi、RSSI更新、信道更新等）

**触发条件**:
- BLE已连接（`get_ble_app_disconnect_flag() == 0`）
- 通知已启用（CCC已配置）
- 满足以下任一条件：
  - 蓝牙连接成功后立即推送（首次推送）
  - WiFi扫描正在进行中（命令状态为 `WIFI_CMD_SCANNING`，或扫描状态为 `WIFI_SCAN_RUNNING`）
  - 检测到列表有变化（数量变化或内容更新）

**通知内容**:
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
        },
        {
            "ssid": "另一个WiFi",
            "rssi": -60,
            "channel": 11
        }
    ]
}
```

**错误码**:
- `WIFI_LIST_UPDATE_CODE_SUCCESS` - 列表有内容，推送成功
- `WIFI_LIST_UPDATE_CODE_EMPTY` - 列表为空（也会推送，让小程序知道当前状态）

**实现**: 
- **连接后立即推送**：
  - `HCI_SUBEVENT_LE_CONNECTION_COMPLETE`事件 → `server_cbk_packet_handler()` → 检查通知已启用 → `notify_app_wifi_list_update()` → `send_return_json_data()`
- **扫描更新推送**：
  - `WIFI_EVENT_STA_SCAN_COMPLETED`事件 → `store_wifi_scan_result()` → 检测列表变化 → `notify_app_wifi_list_update()` → `send_return_json_data()`

**缓冲列表机制**:
- **容量限制**：最多保存50个WiFi（`STORE_WIFI_NUM_MAX = 50`）
- **累积方式**：每次扫描结果与现有列表合并
  - 新SSID：添加到缓冲（如果未满）
  - 已存在SSID：更新RSSI、信道等信息
- **缓冲已满处理**：忽略新WiFi（不添加）
- **排序**：所有WiFi按RSSI降序排序
- **持久性**：缓冲列表在设备启动后持续更新，不依赖小程序连接

**状态检查机制**:
- `store_wifi_scan_result()` 中检查：同时检查 `cmd_status` 和 `scan_status`
- `notify_app_wifi_list_update()` 中再次检查：确保状态一致性
- 如果任一状态表明正在扫描，则允许推送通知

**变化检测机制**:
- 记录更新前的WiFi数量（`prev_count`）
- 检测新增WiFi（数量增加，`has_new = true`）
- 检测已有WiFi的信息更新（RSSI变化、信道变化等，`has_update = true`）
- 任何变化（`has_changed = true`）都会触发推送

### 2. WiFi IP获取成功通知

**触发时机**: WiFi连接成功并获取到IP地址后

**通知内容**:
```json
{
    "cmd_name": "wifi_connect",
    "return_code": 0,
    "return_errstr": "WIFI_CONNECT_CODE_SUCCESS",
    "ip_address": "192.168.1.100"
}
```

**实现**: `notify_app_wifi_ip_succ()` → `send_return_json_data()`

### 3. MQTT连接成功通知

**触发时机**: MQTT服务器连接成功并完成初始化后

**通知内容**:
```json
{
    "cmd_name": "mcu_notify_app_mqtt",
    "return_errstr": "MCU_WIFI_TASK_SUCC"
}
```

**实现**: `notify_app_wifi_mqtt_succ()` → `send_return_json_data()` → `server_disconnect()`

**注意**: MQTT通知发送完成后会主动断开BLE连接。

---

## 典型配网流程

### 完整配网时序图

```
小程序                          设备
  |                              |
  |--- [1. 建立BLE连接] ---------->|
  |   (连接、获取服务、开启通知)   |
  |                              |
  |                              | [STA模式已启动，自动扫描中...]
  |                              | 扫描结果持续累积到缓冲
  |                              |
  |<-- [2. wifi_list_update] -----| 连接成功后立即推送缓冲列表
  |   {wifi_list}                | notify_app_wifi_list_update()
  |   (当前缓冲中的WiFi列表)      | (即使为空也推送)
  |                              |
  | [显示选择界面]                |
  |                              |
  |                              | [继续自动扫描...]
  |                              | 每次扫描完成:
  |                              | - 累积到缓冲
  |                              | - 检测变化
  |<-- [3. wifi_list_update] -----| 主动推送（列表有变化时）
  |   {wifi_list}                | notify_app_wifi_list_update()
  |                              |
  |<-- [4. wifi_list_update] -----| 继续推送更新
  |   {wifi_list}                | (RSSI更新、新增WiFi等)
  |                              |
  |--- [5. wifi_connect] -------->|
  |   {ssid, pwd}                | 连接WiFi网络
  |                              |
  |                              | [WiFi连接中...]
  |                              |
  |                              | WiFi连接成功，获取IP
  |<-- [6. wifi_connect响应] -----| notify_app_wifi_ip_succ()
  |   {ip_address}               |
  |                              |
  |                              | [MQTT连接中...]
  |                              |
  |                              | MQTT连接成功
  |<-- [7. mcu_notify_app_mqtt] --| notify_app_wifi_mqtt_succ()
  |                              | [断开BLE连接]
  |                              |
```

### 关键状态标志

#### `g_wifi_scan_flag`
- **位置**: `ble_data.c:27`
- **用途**: 标识WiFi缓冲列表是否有数据
- **设置时机**:
  - 缓冲列表有数据时设置为1
  - `handle_wifi_get_list()` 获取列表后保持为1（可继续获取）

#### WiFi状态管理
- `WIFI_CMD_IDLE`: 空闲状态
- `WIFI_CMD_SCANNING`: 扫描中（由STA模式自动触发）
- `WIFI_CMD_CONNECTING`: 连接中
- `WIFI_CMD_RECONNECTING`: 重连中

### 自动扫描与缓冲累积机制

**自动扫描机制**：
- **触发时机**：设备进入STA模式时自动开始WiFi扫描
- **扫描方式**：持续循环扫描，每次扫描完成触发`WIFI_EVENT_STA_SCAN_COMPLETED`事件
- **无需命令**：小程序不再需要发送`wifi_scan`命令，设备自动扫描

**缓冲累积机制**：
- **缓冲容量**：`g_all_wifi_info[]`数组，最多保存50个WiFi（`STORE_WIFI_NUM_MAX = 50`）
- **累积方式**：每次单次扫描完成，调用`store_wifi_scan_result()`
  - 新扫描到的WiFi：如果SSID不存在且缓冲未满，添加到缓冲
  - 已存在的WiFi：更新信息（RSSI、信道等）
  - 缓冲已满时：忽略新WiFi（不添加）
- **排序**：所有WiFi按RSSI降序排序
- **持久性**：缓冲列表在设备启动后持续更新，不依赖小程序连接

**主动推送机制**：
- **连接后立即推送**：BLE连接建立且通知已启用后，立即推送当前缓冲列表
- **扫描更新推送**：
  - 检测列表变化（数量变化、RSSI更新、信道更新等）
  - **状态检查**：同时检查命令状态（`WIFI_CMD_SCANNING`）和扫描状态（`WIFI_SCAN_RUNNING`）
  - **去抖动机制**：RSSI变化≥10dBm或信道变化时才标记为更新，避免微小变化导致的频繁推送
  - 如果检测到变化且正在扫描中，自动调用`notify_app_wifi_list_update()`推送
  - 小程序无需轮询，实时接收更新的WiFi列表
- **手动获取**：小程序仍可随时调用`wifi_get_list`手动获取完整列表（用于刷新）

**性能优化**：
- **哈希表查找**：使用链式哈希表实现O(1)平均查找复杂度，将整体处理时间从O(n²)降至O(n)
- **去抖动**：RSSI变化阈值设置为±10dBm，减少不必要的推送，降低BLE通道压力

**注意**: 
- 状态检查采用双重验证机制，确保即使命令状态被重置，只要扫描还在运行，仍能正常推送通知
- 缓冲列表在设备启动后就开始累积，即使小程序未连接也会持续更新

---

## 技术要点

### 1. 分片组装机制
- **发送端**: JSON数据按16字节分片，添加序号和总包数
- **接收端**: 根据序号和总包数组装完整JSON
- **最大数据量**: 99包 × 16字节 = 1584字节

### 2. 线程安全
- 使用互斥锁保护BLE状态操作
- WiFi扫描在独立线程池中执行
- 避免阻塞BLE事件处理

### 3. 连接管理
- 自动处理连接/断开事件
- 断开后自动重新广播（如果WiFi未连接）
- CCC配置检查确保通知已启用

### 4. 错误处理
- 命令解析错误返回`return_code: 1`
- 状态检查不满足返回相应错误码
- 数据发送失败记录日志

---

## 性能优化方向

### 当前性能瓶颈分析

#### 1. BLE数据传输性能

**瓶颈点**:
- **单包数据量小**: 每包仅16字节有效数据（总包20字节，前4字节为包头）
- **分包数量多**: WiFi列表较大时可能达到几十包
- **串行发送**: 分包需要逐包发送，存在等待时间
- **MTU限制**: 当前MTU为200字节，但实际每包只传输16字节

**影响**:
- 传输一个包含50个WiFi的列表（约1500字节）需要约94包，传输时间较长
- 频繁推送更新时，BLE通道可能拥塞

**剩余优化方向**:
1. **MTU协商优化**（高优先级，待实施）
   - 协商更大的MTU（如512字节），减少分包数量
   - 需要双方支持，小程序端也需要支持MTU协商
   - 可减少约50%的分包数量

#### 2. WiFi扫描处理性能

**已优化**:
- ✅ **哈希表查找**: 使用链式哈希表实现O(1)平均查找复杂度，将整体处理时间从O(n²)降至O(n)
- ✅ **去抖动机制**: RSSI变化阈值设置为±10dBm，减少不必要的推送

**剩余瓶颈**:
- **排序开销**: 每次更新后都要重新排序整个列表（`qsort`）
- **内存拷贝**: 多次`memcpy`操作

#### 3. 推送频率控制

**已优化**:
- ✅ **去抖动机制**: RSSI变化阈值设置为±10dBm，只有变化≥10dBm或信道变化时才推送

---

## 调试建议

### 关键日志点
1. `att_write_callback()`: 记录接收到的原始数据
2. `parse_and_assemble()`: 记录分片组装过程
3. `parse_and_dispatch_json()`: 记录命令解析和分发
4. `send_return_json_data()`: 记录分包发送过程
5. `handle_xxx()`: 各命令处理函数的执行结果

### 常见问题
1. **数据发送失败**: 检查CCC是否已配置（通知是否启用）
2. **JSON解析失败**: 检查分片组装是否正确完成
3. **WiFi列表为空**: 检查`g_wifi_scan_flag`标志和扫描状态
4. **连接断开**: 检查`server_con_handle`是否有效

---

## 文件结构

```
apps/ling_wl/bt_ble/
├── server/
│   ├── ble_simple_server.c      # BLE服务器主文件
│   ├── ble_simple_server.h      # BLE服务器头文件
│   ├── ble_simple_server_gatt_profile.inl  # GATT配置文件
│   └── ble.md                   # 本文档
├── ble_data.c                   # 命令处理和JSON解析
├── ble_data.h                   # 命令处理头文件
└── app_wifi_manage.c            # WiFi管理（扫描、连接）
```

---

## 总结

小程序蓝牙通信采用**分片传输 + JSON命令**的架构，实现了完整的WiFi配网功能。核心特点：

1. **双向通信**: 支持小程序下发命令和设备主动通知
2. **分片传输**: 解决BLE单次传输数据量限制
3. **自动扫描**: STA模式启动时自动开始WiFi扫描，无需小程序触发
4. **缓冲累积**: WiFi列表自动累积到缓冲（最多50个），设备启动后持续更新
5. **连接即推送**: 蓝牙连接成功后立即推送缓冲列表，无需等待
6. **主动通知**: 缓冲更新和WiFi/MQTT连接成功后主动通知小程序

整个通信机制设计合理，能够满足配网场景的各种需求。设备端自动扫描和缓冲机制大大简化了小程序端的操作流程。

