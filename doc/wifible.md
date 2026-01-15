# 修改说明：Wi‑Fi 失败才开启 BLE 广播（小程序可扫描）

## 目标（达成的业务行为）

- **Wi‑Fi 未失败前**：设备不会开启 BLE 广播，小程序**扫描不到**设备。
- **Wi‑Fi 一次失败即视为失败**：首次检测到 Wi‑Fi 连接失败事件时，设备**立刻开启 BLE 广播**，小程序可扫描并进入配网。
- **Wi‑Fi 成功后的 BLE 行为**：保持原有退出逻辑（模式退出时关闭 BLE 模块/断开连接）。

---

## 核心逻辑调整点（从“init 即广播”改为“失败才广播”）

### 1) 解耦 BLE “初始化”与“开始广播”

**文件**：[apps/ling_wl/bt_ble/server/ble_simple_server.c](cci:7://file:///Users/noir/gitlab/kqq_7911/apps/ling_wl/bt_ble/server/ble_simple_server.c:0:0-0:0)
**函数**：[ble_simple_server_init()](cci:1://file:///Users/noir/gitlab/kqq_7911/apps/ling_wl/bt_ble/server/ble_simple_server.c:892:0-900:1)

#### 修改前

- [ble_simple_server_init()](cci:1://file:///Users/noir/gitlab/kqq_7911/apps/ling_wl/bt_ble/server/ble_simple_server.c:892:0-900:1) 内部调用 [ble_simple_server_name_enable(0)](cci:1://file:///Users/noir/gitlab/kqq_7911/apps/ling_wl/bt_ble/server/ble_simple_server.c:863:0-890:1)
- 而 [ble_simple_server_name_enable()](cci:1://file:///Users/noir/gitlab/kqq_7911/apps/ling_wl/bt_ble/server/ble_simple_server.c:863:0-890:1) 会执行 [ble_simple_server_module_enable(1)](cci:1://file:///Users/noir/gitlab/kqq_7911/apps/ling_wl/bt_ble/server/ble_simple_server.c:834:0-847:1)，从而触发 [bt_ble_adv_enable(1)](cci:1://file:///Users/noir/gitlab/kqq_7911/apps/ling_wl/bt_ble/server/ble_simple_server.c:719:0-722:1) → **启动广播**
- 结果：只要调用了 [ble_simple_server_init()](cci:1://file:///Users/noir/gitlab/kqq_7911/apps/ling_wl/bt_ble/server/ble_simple_server.c:892:0-900:1)，就会导致小程序可扫描到设备（不符合“失败才广播”）

#### 修改后

- [ble_simple_server_init()](cci:1://file:///Users/noir/gitlab/kqq_7911/apps/ling_wl/bt_ble/server/ble_simple_server.c:892:0-900:1) **不再调用** [ble_simple_server_name_enable(0)](cci:1://file:///Users/noir/gitlab/kqq_7911/apps/ling_wl/bt_ble/server/ble_simple_server.c:863:0-890:1)
- 仅做互斥锁准备并将状态置为 `BLE_ST_NULL`（不启动广播）

#### 影响

- 现在 BLE 广播的开启变为**显式行为**：只有调用 [ble_simple_server_name_enable(x)](cci:1://file:///Users/noir/gitlab/kqq_7911/apps/ling_wl/bt_ble/server/ble_simple_server.c:863:0-890:1) / [ble_simple_server_module_enable(1)](cci:1://file:///Users/noir/gitlab/kqq_7911/apps/ling_wl/bt_ble/server/ble_simple_server.c:834:0-847:1) 才会启动广播。

---

## 各业务入口的行为变化

### 2) NET_CONNECTING：Wi‑Fi 首次失败立即广播（不再依赖按键）

**文件**：[apps/ling_wl/app_mode/net_connecting/net_connecting.c](cci:7://file:///Users/noir/gitlab/kqq_7911/apps/ling_wl/app_mode/net_connecting/net_connecting.c:0:0-0:0)
**函数**：[app_net_connecting_loop()](cci:1://file:///Users/noir/gitlab/kqq_7911/apps/ling_wl/app_mode/net_connecting/net_connecting.c:165:0-216:1)

#### 失败判定口径（保持原条件集合）

`is_connection_failed` 为以下任意状态即认为失败：

- `WIFI_STA_CONNECT_TIMEOUT_NOT_FOUND_SSID`
- `WIFI_STA_CONNECT_ASSOCIAT_FAIL`
- `WIFI_STA_DISCONNECT`
- `WIFI_STA_CONNECT_ASSOCIAT_TIMEOUT`
- `WIFI_STA_NETWORK_STACK_DHCP_TIMEOUT`

#### 修改前

- 首次失败时调用 [ble_simple_server_name_enable(0)](cci:1://file:///Users/noir/gitlab/kqq_7911/apps/ling_wl/bt_ble/server/ble_simple_server.c:863:0-890:1)（广播名偏 idle）并进入 `wifi_manager_state`
- 但实际 BLE “忙态广播/连接流程”需要 **按键触发**（通过 `key_press_time_ms` 驱动 [ble_simple_server_name_enable(1)](cci:1://file:///Users/noir/gitlab/kqq_7911/apps/ling_wl/bt_ble/server/ble_simple_server.c:863:0-890:1)）
- 结果：失败后不一定立刻对外可见/可连接，依赖用户动作

#### 修改后

首次失败时（`!wifi_manager_state && is_connection_failed`）直接执行：

- `wifi_manager_state = true;`
- `key_press_time_ms = timer_get_ms();`
- `ble_connecting = true;`
- `ble_connected = false;`
- **[ble_simple_server_name_enable(1);](cci:1://file:///Users/noir/gitlab/kqq_7911/apps/ling_wl/bt_ble/server/ble_simple_server.c:863:0-890:1) → 立刻启动广播（小程序可扫）**

并且 BLE 状态机处理由：

- `if (wifi_manager_state && key_press_time_ms > 0)`改为：
- `if (wifi_manager_state)`
  确保失败后 BLE 连接/超时逻辑持续运行。

#### 同时调整

**函数**：[start_ble_net()](cci:1://file:///Users/noir/gitlab/kqq_7911/apps/ling_wl/app_mode/net_connecting/net_connecting.c:61:0-65:1)

- 修改为不再设置 `key_press_time_ms` 去触发 BLE 广播（避免“失败立刻广播”和“按键触发广播”两条路径互相影响）。

---

### 3) mode7（devices_check）：改为“失败才广播”

**文件**：[apps/ling_wl/app_mode/devices_check/mode_7_wifi.c](cci:7://file:///Users/noir/gitlab/kqq_7911/apps/ling_wl/app_mode/devices_check/mode_7_wifi.c:0:0-0:0)
**函数**：[mode7_init()](cci:1://file:///Users/noir/gitlab/kqq_7911/apps/ling_wl/app_mode/devices_check/mode_7_wifi.c:98:0-109:1) / [process_mode_7()](cci:1://file:///Users/noir/gitlab/kqq_7911/apps/ling_wl/app_mode/devices_check/mode_7_wifi.c:49:0-95:1)

#### 修改前

- [mode7_init()](cci:1://file:///Users/noir/gitlab/kqq_7911/apps/ling_wl/app_mode/devices_check/mode_7_wifi.c:98:0-109:1) 中无条件执行：
  - [ble_simple_server_init();](cci:1://file:///Users/noir/gitlab/kqq_7911/apps/ling_wl/bt_ble/server/ble_simple_server.c:892:0-900:1)
  - [ble_simple_server_name_enable(1);](cci:1://file:///Users/noir/gitlab/kqq_7911/apps/ling_wl/bt_ble/server/ble_simple_server.c:863:0-890:1) → 进入 mode7 就立刻广播

#### 修改后

- [mode7_init()](cci:1://file:///Users/noir/gitlab/kqq_7911/apps/ling_wl/app_mode/devices_check/mode_7_wifi.c:98:0-109:1) 仅保留 [ble_simple_server_init()](cci:1://file:///Users/noir/gitlab/kqq_7911/apps/ling_wl/bt_ble/server/ble_simple_server.c:892:0-900:1)（不广播）
- 在 [process_mode_7()](cci:1://file:///Users/noir/gitlab/kqq_7911/apps/ling_wl/app_mode/devices_check/mode_7_wifi.c:49:0-95:1) 检测到 Wi‑Fi 失败且首次进入失败处理时，执行：
  - **[ble_simple_server_name_enable(1);](cci:1://file:///Users/noir/gitlab/kqq_7911/apps/ling_wl/bt_ble/server/ble_simple_server.c:863:0-890:1)** → 失败后才广播

---

## 不变的逻辑（沿用原有实现）

- **Wi‑Fi 成功后的 BLE 收尾**：[net_connecting_unint()](cci:1://file:///Users/noir/gitlab/kqq_7911/apps/ling_wl/app_mode/net_connecting/net_connecting.c:127:0-135:1) 仍调用 [ble_simple_server_exit()](cci:1://file:///Users/noir/gitlab/kqq_7911/apps/ling_wl/bt_ble/server/ble_simple_server.c:902:0-907:1)，其内部会 [ble_simple_server_module_enable(0)](cci:1://file:///Users/noir/gitlab/kqq_7911/apps/ling_wl/bt_ble/server/ble_simple_server.c:834:0-847:1)，保持原本“退出模式关闭/断开 BLE”的策略。
- **BLE 断连后的重广播策略**：[ble_simple_server.c](cci:7://file:///Users/noir/gitlab/kqq_7911/apps/ling_wl/bt_ble/server/ble_simple_server.c:0:0-0:0) 的断连处理仍保留原逻辑（当 Wi‑Fi 未成功时可触发重新广播的路径），用于配网阶段连接不稳定时保持可发现性。

---

## 总结

本次修改将 BLE 广播从“调用 init 就会对外可见”调整为“**仅在 Wi‑Fi 首次失败后才对外可见**”，从而确保：

- Wi‑Fi 正常可连接的场景下，小程序扫描不到设备（避免误配/干扰）
- Wi‑Fi 连接失败时立刻进入可配网状态（无需再按键触发）

**任务完成状态：已输出修改说明文档。**
