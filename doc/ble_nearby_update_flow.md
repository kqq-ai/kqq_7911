# nearby_update（按扫描即发）流程说明

## 目标

将“距离类型/距离变化事件”链路替换为 **nearby_update** 单事件链路：只要扫描到匹配的E端广播包，就更新 nearby 设备集合，并通过 WebSocket 上报一次 `nearby_update`（全量 devices 列表）。

## 新流程（核心）

### 1) BLE扫描回调 → 广播入队

- **位置**：`apps/ling_wl/bt_ble/client/ble_distance.c` → `scan_rssi_record_cb()`
- **行为**：过滤广播后，把 `adv_data + rssi` 入 `adv_queue`

### 2) 广播处理任务 → 记录RSSI → 计算/平滑距离

- **位置**：`apps/ling_wl/bt_ble/client/ble_distance.c` → `scan_adv_process_task()`
- **行为**：
  - 解析 SN / bat_level / adv_state
  - 更新 `dev->update_time`
  - 记录 `distance_info.rssi_samples[]` 与时间戳
  - 当 1 秒内样本数足够时，执行 `distance_predict()` + `soomth_distance()` 得到“平滑后的最终距离”

### 3) 每个匹配adv包都触发 nearby_update（先更新集合，再投递事件）

- **位置**：`apps/ling_wl/bt_ble/client/ble_distance.c` → `scan_adv_process_task()`
- **顺序**（严格）：
  1. `wss_device_nearby_set_update(dev)`：维护 WSS nearby 设备集合（加入/移除）
  2. `lwl_event_notify(LWL_EVENT_TYPE_NEARBY_UPDATE)`：投递事件（携带 `pdev + scan_time_ms`）

> 注意：这里不再使用 `LWL_EVENT_TYPE_EEND_DEV_DISTANCE_CHANGE` / `distance_type` 相关逻辑。

### 4) 事件调度线程分发 → WSS侧发送WS JSON

- **事件调度**：`apps/ling_wl/app_mode/lwl_app_core.c` → `lwl_simple_event_scheduler()`
- **WSS处理**：`apps/ling_wl/app_mode/wss_connected/wss_device.c` → `nearby_update_event_handler_cb()`
- **WS发送**：`apps/ling_wl/app_mode/wss_connected/wss_app_util.c` → `send_nearby_update()`

## nearby集合维护规则（wss_device_nearby_set_update）

- **位置**：`apps/ling_wl/app_mode/wss_connected/wss_device.c`
- **规则**（中文口径）：
  - 未过期：`dev->update_time + EDEV_EXPIRE_TIME_MS >= now`
  - RSSI有效：`rssi_sample_count > 0`
  - 距离有效且在范围：`0.1m < distance < MAX_DIS(2.5m)`
  - 满足则加入/更新集合；否则移除

## WS消息格式（doc/nearby_update.md 对齐）

- `event`: 固定 `"nearby_update"`
- `msg_id`: `c:<sn_c_id>:nbu_<timer_get_ms()>`（随机/唯一即可）
- `data.scan_time`: `YYYY-MM-DDTHH:MM:SS.mmmZ`
  - 年月日时分秒来自RTC
  - 毫秒使用 `scan_time_ms % 1000`（与“scan_time=update_time(毫秒)”口径对齐）
- `data.devices[]`：全量输出当前 nearby 集合：
  - `sn`: E端SN
  - `rssi`: 最新RSSI样本
  - `distance_cm`: 平滑后的最终距离（厘米）
  - `battery`: 电量（0-100，按 `bat_level(0-15)` 线性映射）

## 与旧逻辑的区别

- **旧**：按 `distance_type` 变化触发 `LWL_EVENT_TYPE_EEND_DEV_DISTANCE_CHANGE`，WSS侧再根据类型 add/remove，并可能触发 `ORMET_NEAR_TRIGGER`
- **新**：按“每个匹配adv包”触发 `nearby_update`，WSS侧仅维护集合并通过WS上报；`ORMET_NEAR_TRIGGER` 已从距离链路移除


