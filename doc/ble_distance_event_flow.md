# BLE距离检测与nearby_update事件流程文档

## 概述

本文档描述了从BLE扫描、RSSI收集、距离计算到nearby_update事件触发的完整流程。该系统每500ms通过WebSocket发送所有扫描到的E端设备信息。

**核心机制：**
- **触发方式**：基于固定时间周期（500ms），不再依赖距离类型变化
- **距离计算用途**：仅用于提供WebSocket消息中的 `distance_cm` 字段
- **事件处理**：集成在 `wss_application.c` 中，无需额外文件
- **数据发送**：每次发送所有有效设备，不进行距离过滤

---

## 一、整体架构

```
┌─────────────────┐
│  BLE扫描模块     │  ← 持续扫描BLE广播
└────────┬────────┘
         │ 广播数据 + RSSI
         ▼
┌─────────────────┐
│ 广播数据处理任务  │  ← scan_adv_process_task
└────────┬────────┘
         │ 解析SN、记录RSSI、计算距离
         ▼
┌─────────────────┐
│ 距离计算任务     │  ← distance_main (每500ms)
└────────┬────────┘
         │ 触发nearby_update事件
         ▼
┌─────────────────┐
│ 事件通知         │  ← lwl_event_notify
└────────┬────────┘
         │ nearby_update事件
         ▼
┌─────────────────┐
│ 事件调度器       │  ← lwl_simple_event_scheduler
└────────┬────────┘
         │ 分发事件
         ▼
┌─────────────────┐
│ nearby_update处理器 │  ← nearby_update_event_handler (wss_application.c)
└────────┬────────┘
         │ 收集所有设备信息
         ▼
┌─────────────────┐
│ WebSocket发送    │  ← send_nearby_update
└─────────────────┘
```

---

## 二、详细流程

### 阶段1：BLE扫描与广播接收

**触发时机：** BLE扫描器持续运行，接收周围设备的广播数据

**执行位置：** `ble_simple_client` 模块

**流程：**

1. **BLE扫描回调触发**
   - 扫描器收到BLE广播包
   - 调用注册的回调函数 `scan_rssi_record_cb`

2. **广播数据过滤**
   ```c
   // 位置：ble_distance.c:290
   if (!adv_pre_match_impl(report_pt->data, report_pt->length)) {
       return;  // 不匹配的广播直接丢弃
   }
   ```

3. **数据入队**
   ```c
   // 位置：ble_distance.c:305
   my_blocking_queue_try_enqueue(&context->adv_queue, &adv_data);
   ```
   - 将广播数据（包含RSSI、MAC地址、广播数据）放入队列
   - 队列类型：`lwl_cqueue_adv_data_t`

**关键数据结构：**
```c
typedef struct {
    s8 rssi;                    // 信号强度
    u8 address[6];              // MAC地址
    u8 address_type;            // 地址类型
    u8 adv_data[32];            // 广播数据
    u8 adv_data_length;         // 数据长度
} lwl_cqueue_adv_data_t;
```

---

### 阶段2：广播数据处理

**触发时机：** `scan_adv_process_task` 任务从队列中取出数据

**执行位置：** `ble_distance.c:506` - `scan_adv_process_task()`

**流程：**

1. **从队列取出数据**
   ```c
   // 位置：ble_distance.c:513
   my_blocking_queue_dequeue(&temp_ctx->adv_queue, &adv_data);
   ```

2. **解析广播数据**
   ```c
   // 位置：ble_distance.c:529-536
   char sn[16] = "";
   memcpy(sn, adv_data.adv_data + 3, 15);  // 提取设备SN
   if (sn[0] != 'K' || sn[1] != 'P') {
       continue;  // 过滤非E端设备（SN必须以KP开头）
   }
   
   uint8_t bat_level = (buffer[2] >> 4) & 0x0F;      // 电量
   uint8_t adv_state = (buffer[2] >> 0) & 0x0F;      // 广播状态
   ```

3. **查找或创建设备**
   ```c
   // 位置：ble_distance.c:542-552
   lwl_eend_dev_t *dev = find_eend_dev_by_sn(sn);
   if (dev == NULL) {
       dev = add_new_eend_dev(sn);  // 新设备，创建设备结构
       // 发送新设备进入事件
       lwl_event_t e;
       e.event_type = LWL_EVENT_TYPE_EEND_DEV_NEW_IN;
       e.new_in.pdev = dev;
       lwl_event_notify(&e);
   }
   ```

4. **更新设备信息**
   ```c
   // 位置：ble_distance.c:556-562
   dev->bat_level = bat_level;
   dev->adv_state = adv_state;
   dev->update_time = timer_get_ms();
   memcpy(dev->address, adv_data.address, 6);
   ```

5. **记录RSSI样本**
   ```c
   // 位置：ble_distance.c:583-592
   os_mutex_pend(&dev->distance_info.mutex, 0);
   if (dev->distance_info.rssi_sample_count < MAX_SAMPLES) {
       int idx = dev->distance_info.rssi_sample_count;
       dev->distance_info.rssi_samples[idx] = rssi;
       dev->distance_info.rssi_timestamps[idx] = timer_get_ms();
       dev->distance_info.rssi_sample_count++;
   }
   os_mutex_post(&dev->distance_info.mutex);
   ```

**关键点：**
- RSSI样本存储在循环数组中，最多保存 `MAX_SAMPLES` 个样本
- 每个样本包含RSSI值和时间戳
- 使用互斥锁保护RSSI数据的并发访问

---

### 阶段3：距离计算

**触发时机：** `distance_main` 任务周期性执行（每100ms检查一次，有足够RSSI样本时计算距离）

**执行位置：** `ble_distance.c:604-642` - `scan_adv_process_task()`中的距离计算逻辑

**流程：**

1. **距离计算**
   - 使用1秒内的RSSI样本（最多 `MAX_SAMPLES` 个）
   - 提取特征值：最大值、最小值、中位数、次大值、总和
   - 调用 `distance_predict()` 进行线性回归预测
   ```c
   // 位置：ble_distance.c:640
   distance_predict((double[5]){max_sample, min_sample, median, max_2_sample, log(samples_count)}, &possible);
   ```

2. **距离平滑处理**
   ```c
   // 位置：ble_distance.c:642
   soomth_distance(dev, possible);
   ```
   - 限制距离变化速率（最大 `step_max_change` 米/秒）
   - 防止距离值突变

3. **距离类型计算（已废弃）**
   - **重要说明：** 距离类型（DISTANCE_0/1/2/3）的判断逻辑已被注释，不再使用
   - **距离计算的唯一用途：** 为WebSocket消息中的 `distance_cm` 字段提供数值
   - 距离值通过线性回归和平滑处理得到，以厘米为单位发送给后端

---

### 阶段4：nearby_update事件触发

**触发时机：** `distance_main` 任务每500ms触发一次

**执行位置：** `ble_distance.c:326` - `distance_main()`

**触发逻辑：**

```c
// 位置：ble_distance.c:370-377
if (current_time - notify_app_time > NOTIFY_APP_TIME) {  // NOTIFY_APP_TIME = 500ms
    notify_app_time = current_time;

    // 首先清理过期数据
    remove_all_dev_expired_rssi_samples();

    // 每500ms触发nearby_update事件
    lwl_event_t e;
    e.event_type = LWL_EVENT_TYPE_NEARBY_UPDATE;
    lwl_event_notify(&e);

    clean_expired_eend();
}
```

**事件数据结构：**
```c
typedef struct {
    u8 event_type;  // LWL_EVENT_TYPE_NEARBY_UPDATE
    union {
        // ... 其他事件类型
        lwl_event_nearby_update_t nearby_update;
    };
} lwl_event_t;
```

**关键特性：**
- 固定500ms触发周期
- 不依赖距离类型变化
- 每次触发都会发送所有设备信息

---

### 阶段5：事件分发

**触发时机：** 事件被投递到事件队列后

**执行位置：** `lwl_app_core.c:25` - `lwl_simple_event_scheduler()`

**流程：**

1. **事件入队**
   ```c
   // 位置：lwl_app_core.c:455
   void lwl_event_notify(lwl_event_t *event) {
       my_blocking_queue_enqueue(&s_app_context->event_queue, event);
   }
   ```

2. **事件调度器循环**
   ```c
   // 位置：lwl_app_core.c:30-40
   while (!g_event_scheduler_exit) {
       // 从队列取出事件
       my_blocking_queue_dequeue(&s_app_context->event_queue, &event);
       
       // 遍历所有注册的回调函数
       os_mutex_pend(&s_app_context->event_handlers_mutex, 0);
       for (int i = 0; i < LWL_EVENT_HANDLER_MAX_CNT; ++i) {
           if (s_app_context->event_handlers[i] != NULL) {
               s_app_context->event_handlers[i](&event);  // 调用回调
           }
       }
       os_mutex_post(&s_app_context->event_handlers_mutex);
   }
   ```

**关键点：**
- 事件调度器运行在独立线程中
- 所有注册的回调函数都会被调用
- 使用互斥锁保护回调函数列表

---

### 阶段6：nearby_update事件处理

**触发时机：** 事件调度器调用已注册的回调函数

**执行位置：** `wss_application.c:115` - `nearby_update_event_handler()`

**流程：**

1. **事件类型检查**
   ```c
   if (e->event_type != LWL_EVENT_TYPE_NEARBY_UPDATE) {
       return;  // 只处理nearby_update事件
   }
   ```

2. **统计有效设备**
   ```c
   // 第一次遍历dev_map，统计有RSSI样本的设备数量
   MapIterator it = map_begin(ctx->dev_map);
   MapIterator end = map_end(ctx->dev_map);
   
   for (; !map_iterator_equal(it, end); map_iterator_next(&it)) {
       lwl_eend_dev_t *dev = (lwl_eend_dev_t *)map_iterator_value(it);
       if (dev && dev->distance_info.rssi_sample_count > 0) {
           valid_device_count++;
       }
   }
   map_iterator_unlock(it);
   ```

3. **收集设备信息**
   ```c
   // 分配设备指针数组
   void **devices = (void **)malloc(valid_device_count * sizeof(void *));
   
   // 第二次遍历，收集有效设备
   for (; !map_iterator_equal(it, end); map_iterator_next(&it)) {
       lwl_eend_dev_t *dev = (lwl_eend_dev_t *)map_iterator_value(it);
       if (dev && dev->distance_info.rssi_sample_count > 0) {
           devices[idx++] = (void *)dev;
       }
   }
   map_iterator_unlock(it);
   ```

4. **生成并发送消息**
   ```c
   // 调用send_nearby_update发送WebSocket消息
   int ret = send_nearby_update(devices, idx);
   
   // 释放临时资源
   free(devices);
   ```

**关键说明：**
- 事件处理逻辑直接集成在 `wss_application.c` 中，无需额外文件
- 简化了模块依赖关系，提高了代码内聚性

---

### 阶段7：WebSocket消息发送

**执行位置：** `wss_app_util.c:378` - `send_nearby_update()`

**消息格式：**

```json
{
  "msg_id": "c:WL-C8K2512000891:nbu_20251225165803472",
  "event": "nearby_update",
  "data": {
    "scan_time": "2025-12-25T16:58:02.819Z",
    "devices": [
      {
        "sn": "E9K2512003147",
        "rssi": -49,
        "distance_cm": 45,
        "battery": 88
      }
    ]
  }
}
```

**字段说明：**
- `msg_id`: 格式为 `c:{c_id}:nbu_{timestamp}`
- `scan_time`: ISO8601格式的时间戳
- `sn`: 设备序列号
- `rssi`: 最新RSSI值（取rssi_samples数组最后一个）
- `distance_cm`: 距离（米转厘米，四舍五入）
- `battery`: 电量百分比（bat_level * 100 / 15）

---

## 三、初始化流程

### 1. 距离检测模块初始化

```c
// 位置：app_cookie.c:240
monitor_init();  // 初始化距离检测模块
```

**执行步骤：**
1. 创建广播数据处理任务 `scan_adv_process_task`
2. 清空所有设备的RSSI样本
3. 注册BLE扫描回调 `scan_rssi_record_cb`
4. 创建距离计算任务 `distance_main`
5. 发送初始化消息到距离任务

### 2. 启动距离检测

```c
// 位置：app_cookie.c:243
monitor_start();  // 启动距离检测
```

**执行步骤：**
1. 发送启动消息到距离任务
2. 距离任务进入 `DISTANCE_TASK_START` 状态
3. 开始周期性触发nearby_update事件（每500ms）

### 3. nearby_update处理器初始化

```c
// 位置：wss_application.c:518
nearby_update_handler_init();  // 初始化nearby_update处理器
```

**执行步骤：**
1. 注册事件回调 `lwl_event_handler_add(nearby_update_event_handler)`
2. 该函数在 `wss_application.c` 中实现，确保事件处理逻辑与应用层紧密集成

---

## 四、关键时序图

```
时间轴 →

BLE扫描器
  │
  ├─> 收到广播 ──┐
  │              │
  └─> 收到广播 ──┤
                 │
                 ▼
广播处理任务
  │
  ├─> 解析SN、记录RSSI
  │
  ├─> 计算距离(distance_cm)
  │
  └─> 更新设备信息
                 
                 
距离计算任务（每500ms）
  │
  ├─> 清理过期RSSI
  │
  └─> 触发nearby_update事件
         │
         ▼
事件通知
  │
  └─> lwl_event_notify()
         │
         ▼
事件队列
  │
  └─> 事件调度器取出
         │
         ▼
调用所有注册的回调
  │
  └─> nearby_update_event_handler()
         │
         ├─> 遍历所有设备
         │
         ├─> 收集有效设备信息
         │
         └─> 生成JSON并发送WebSocket
```

---

## 五、关键参数配置

| 参数 | 值 | 说明 |
|------|-----|------|
| `MAX_SAMPLES` | 64 | RSSI样本最大数量 |
| `RSSI_EXPIRE_TIME_MS` | 2000ms | RSSI样本过期时间 |
| `EDEV_EXPIRE_TIME_MS` | 10000ms | 设备超时时间 |
| `LWL_EDEV_EXPIRE_TIME_MS` | 15000ms | 设备清理超时时间 |
| `step_max_change` | 0.3米/秒 | 距离变化最大速率 |
| `NOTIFY_APP_TIME` | 500ms | nearby_update触发周期 |

---

## 六、异常处理

### 1. RSSI样本溢出
- **现象：** 样本数量超过 `MAX_SAMPLES`
- **处理：** 在存储时检查边界，防止数组越界

### 2. 设备超时
- **现象：** 设备长时间未更新（超过 `EDEV_EXPIRE_TIME_MS`）
- **处理：** 距离类型设为 `DISTANCE_3`，设备仍会包含在nearby_update中

### 3. 事件队列满
- **现象：** 事件投递失败
- **处理：** 广播数据队列满时记录错误日志，丢弃数据

### 4. WebSocket断开
- **现象：** 发送nearby_update失败
- **处理：** 记录错误日志，等待下次500ms周期重试

---

## 七、性能优化点

1. **RSSI样本过期清理**
   - 定期清理过期样本，减少计算量
   - 使用时间戳判断，避免无效数据参与计算

2. **距离平滑处理**
   - 限制距离变化速率，避免突变
   - 提供更稳定的distance_cm值

3. **设备信息批量发送**
   - 每500ms统一发送所有设备
   - 减少WebSocket通信频率

4. **两次遍历优化**
   - 第一次统计数量，精确分配内存
   - 第二次收集数据，避免重复扩容

---

## 八、调试与日志

### 关键日志点

1. **RSSI记录**
   ```
   [RSSI_STORE] KP123456789012345: Stored RSSI=-65
   ```

2. **距离计算**
   ```
   [Distance_Filter] sn: KP123456789012345 distance: 1.85
   ```

3. **nearby_update触发**
   ```
   [nearby_update] Event triggered, collecting device info...
   [nearby_update] Found 3 valid devices
   ```

4. **WebSocket发送**
   ```
   [nearby_update] wbs send msg : {"msg_id":"c:WL-C8K2512000891:nbu_1234567890",...}
   [nearby_update] Successfully sent nearby_update with 3 devices
   ```

---

## 九、总结

整个流程实现了从BLE扫描到WebSocket发送的完整闭环：

1. **数据采集**：BLE扫描 → RSSI记录
2. **距离计算**：RSSI样本 → 距离预测 → 平滑处理（仅用于提供distance_cm字段值）
3. **定时触发**：每500ms触发nearby_update事件（不依赖距离类型变化）
4. **批量发送**：收集所有设备 → 生成JSON → WebSocket发送

该设计具有以下优点：
- **实时性**：每500ms固定发送，不会遗漏设备
- **全面性**：发送所有扫描到的设备，不基于距离过滤
- **稳定性**：距离平滑处理，提供可靠的distance_cm值
- **高效性**：批量发送，减少通信开销
- **简洁性**：事件处理逻辑直接集成在应用层，无需额外文件
- **可维护性**：事件驱动架构，模块解耦

---

**文档版本：** v2.0 (nearby_update重构版)  
**最后更新：** 2025年1月  
**维护者：** 开发团队
