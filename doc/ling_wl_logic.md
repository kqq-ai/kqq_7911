# Ling WL 项目逻辑文档

## 1. 项目概览
- 方案基于杰理 AC79 WiFi/BLE SoC，定位为语音播放 + BLE 配网 + WebSocket 云端互联的智能终端。
- 工程主体位于 `apps/ling_wl/`，遵循 SDK 提供的 `application` 框架，核心 App 为 `app_cookie`。
- 运行时通过多任务 RTOS（`apps/ling_wl/app_main.c` 中的 `task_info_table`）调度编码、网络、USB、音频等服务，并依赖 `wifi`, `bt_ble`, `audio`, `protocol` 等公共组件。

## 2. 启动链路（从入口开始）
1. `app_main()`（`apps/ling_wl/app_main.c`）
   - 打印标识，初始化 `intent`，指定 `.name = "app_cookie"`, `.action = ACTION_COOKIE_MAIN`。
   - 调用 `start_app(&it)`，交由系统应用管理器加载并运行 `app_cookie`。
2. `REGISTER_APPLICATION(app_cookie)`（`apps/ling_wl/app_cookie.c`）
   - 绑定状态机 `app_cookie_state_machine` 与事件处理器 `app_cookie_event_handler`。
3. `ACTION_COOKIE_MAIN` 触发 `app_cookie_main()`：
   - 初始化 `lwl_app_core`，获取全局上下文 `lwl_app_core_context_get_instance()`。
   - 注册基础事件处理 `base_lwl_event_handler`（负责关机、按键、清 WiFi、同步电量等 UART 指令）。
   - 初始化外设/子系统：UART0（与 7016/LED/传感器交互）、录音机、音乐播放器、BLE 调度器、WiFi Manager、RSSI 测距、LittleFS、SC7A20 加速度计。
   - 创建 `threadpool` 及核心线程 `thread_fork("lwl_app_task", ...)`，该线程循环执行 `lwl_app_mode_loop()`，驱动整机业务状态机。

## 3. LWL 应用核心（`apps/ling_wl/app_mode/lwl_app_core.*`）
- **上下文结构 `lwl_app_context_t`**：集中保存 WiFi 凭证、OTA 信息、MAC、WebSocket 句柄、音频/录音实例、线程池、UART、LittleFS 句柄及 E 端设备映射表。
- **事件系统**：
  - `my_blocking_queue` + `lwl_simple_event_scheduler` 线程负责分发 `lwl_event_t`。
  - 支持的事件类型包含 UART 同步/按键/关机、网络/后端断开、E 端设备距离变更等（见 `lwl_app_core.h` 的 `LWL_EVENT_TYPE` 枚举）。
  - 业务模块通过 `lwl_event_handler_add/remove` 注册回调，统一在 `lwl_event_scheduler` 线程中顺序执行。
- **E 端设备管理**：使用 `tree_map` 保存三台以内的 E 端设备，提供增删查遍历、距离统计、最近设备查询等函数，供 WSS 逻辑调用。

## 4. 模式状态机（`apps/ling_wl/app_mode/lwl_app_framework.*`）
- `lwl_app_task` 驱动以下模式，初始状态为 `APP_POWER_ON_MODE`。`lwl_app_mode_switch_to()` 负责在互斥保护下更新 `app_next_mode`，由 `lwl_app_mode_exit_ok()` 在当前模式收尾后真正切换。
- `lwl_app_mode_play()` 确保语音提示串行播放，并在播放期间拉高扬声器供电引脚。

| 模式枚举 | 主要文件 | 进入条件/入口 | 核心职责 | 退出条件 |
| --- | --- | --- | --- | --- |
| `APP_POWER_ON_MODE` | `power_on/power_on.c` | 上电后默认进入 | 设置蓝灯，播 `poweron.mp3`，立即切到 `NET_CONNECTING`，并响应 UART SYNC | `lwl_app_mode_switch_to()` 请求新模式 |
| `APP_DEFAULT_MODE` | `default/default.c` | 手动退回或无网络时恢复 | 打开 BLE 广播待配网，关闭网络栈，等待按键触发配网流程 | 收到按键后切到 `NET_CONNECTING` |
| `APP_NET_CONNECTING_MODE` | `net_connecting/net_connecting.c` | 正在连接 WiFi 或 BLE 配网 | 调度 WiFi + BLE 共存：WiFi 失败→亮蓝灯并播提示，按键触发 BLE 名称广播；连接成功切 `NET_CONNECTED` | DHCP 成功→切换；长时间失败可切回 `DEFAULT` |
| `APP_NET_CONNECTED_MODE` | `net_connected/net_connected.c` | WiFi 联网成功 | 播放联网成功提示，启动 `log_mqtt`, 检查 OTA（`check_ota_info()`），连接 WebSocket (`wss_connect()`) | WSS 成功→`WSS_CONNECTED`；WiFi 掉线→`NET_CONNECTING`; 检测到 OTA→`UPDATE` |
| `APP_WSS_CONNECTED_MODE` | `wss_connected/wss_connected.c` + `wss_application.*` | WebSocket 建立完成 | 执行云端业务（E 端距离统计、下发任务、BLE 控制等）、定期 dump 设备表、处理掉线重连/语音提示 | 后端或 WiFi 中断触发 `NET_CONNECTED`/`NET_CONNECTING`，手动退出 |
| `APP_UPDATE_MODE` | `update/update.c` | OTA 检测通过 | 播放升级提示，实时按进度切换 LED 颜色，`do_OTA()` 完成下载与烧写 | OTA 完成后 `system_reset()` |
| `APP_DEVICE_CHECK_MODE` | `devices_check/*.c` | 工厂测试指令进入 | 分阶段测试按键、LED、喇叭、MIC、加速度计、WiFi、SN 等，结果通过 UART/提示音告知 | 测试完成切回默认或关机 |

## 5. 系统事件与子系统协作
- **系统事件分发**（`app_cookie_event_handler`）：
  - `SYS_KEY_EVENT` → 本地音频控制（播放流/音量/停止等）。
  - `SYS_NET_EVENT` → WiFi 状态提示、RF 共存表切换、通知 LWL 模式。
  - `SYS_BT_EVENT` → 透传给 BLE demo。
- **UART 7016 通道**（`app_mode/7911_uart_7016`）：
  - 提供封装 `set_rgb_color`、`uart_response_synch_status` 等接口，负责 LED、触控、普通按键、Gseneor 电源等；`base_lwl_event_handler` 接收关机/清配置命令，并可触发 `system_reset()` 或 `do_shutdown()`。
- **音频子系统**：
  - `music_player_init()` 创建本地 + 网络播放能力。
  - 用于提示音（`CONFIG_CP_VOICE_FILE_PATH` 下的 mp3）和远端播放 (`app_music_play_net_music_with_callback`)。
  - `recorder_mode_init()` 配合 `SC7A20`、BLE 距离任务，可支持语音/环境数据采集。
- **网络 & BLE**：
  - WiFi：`wifi_manage_init`、`wifi_get_sta_connect_state()` 监测状态；`app_net_connecting_loop` 根据状态切 LED/播放提示。
  - BLE：`ble_simple_server_init()` + `ble_simple_server_name_enable()` 在配网阶段广播；`ble_dev_task_scheduler_init()` 启动后台 BLE 任务；`ble distance` 模块用于 RSSI 定位。
- **云端互联**：
  - `log_mqtt_start()` 上报日志。
  - `wss_application` 完成 WebSocket 会话、设备同步、命令执行，并与 `lwl_app_core` 的设备表交互。
- **文件与配置**：
  - `app_lfs_init("mnt/sdfile/app/exif")` 初始化 LittleFS，`get_flash_app_cfg_data()` 加载配置/资源。
  - `syscfg_read/write` 存取 MAC、音量、WiFi 等持久化数据。
- **传感器与监控**：
  - `app_sc7a20_init()` 管理加速度计，配合 UART 命令可关闭。
  - `monitor_init/monitor_start` 负责 BLE RSSI 测距，数据写入 `dev_map`。

## 6. 关机与异常路径
- `base_lwl_event_handler` 接收 `LWL_EVENT_TYPE_UART_SHUTDOWN` → `do_shutdown()`。
- `do_shutdown()` 强制切回默认模式，尝试播放 `PowerOff.mp3`，完成回调 `power_off_cb()` 才真正调用 `power_set_soft_poweroff()` 并关灯，确保提示音完整播出。
- 低电量处理：UART SYNC 带回电量后通过 MQTT 报告，电量 < 30% 执行 `set_led_red_low_power()`。
- WiFi/后端异常：`NET_EVENT_DISCONNECTED` → 投递 `LWL_EVENT_TYPE_NET_DISCONNECTED`，由各模式自行决定是否重连或退回配网。

## 7. 开发调试建议
- 入口调试：可在 `app_main()` 的 `start_app()` 之前添加日志或自定义 `intent`，但需保证 `app_cookie` 注册名一致。
- 模式扩展：在 `app_mode/lwl_app_framework.c` 中添加新的枚举及 `case`，并在对应目录实现初始化/循环/退出模式，注意在 `lwl_app_mode_switch_check/exit_check` 中维护进入/退出条件。
- 事件使用：新增模块需调用 `lwl_event_handler_add()` 注册回调，并通过 `lwl_event_notify()` 投递事件，避免在中断中直接访问共享状态。
- 音频提示：所有提示音放置在 `CONFIG_CP_VOICE_FILE_PATH` 指定目录，调用 `play_system_music()`/`lwl_app_mode_play()` 自动处理音量与扬声器控制。
- OTA/云端：`check_ota_info()` 依据 LittleFS 中的配置与服务器握手结果决定是否进入 `APP_UPDATE_MODE`，确保远程版本信息写入 `context`。
- UART 协议：与 7016 协处理器的协议在 `protocol/cp/c_uart_p.*`，扩展按键/LED/测试项时需同步修改协议栈及事件回调。

---
以上文档覆盖了从 `app_main` 入口到各业务模式、事件系统及关键子系统的逻辑，可作为二次开发或调试时的参考蓝图。欢迎在对应模块文件头部添加更细粒度的流程注释以保持与本逻辑文档一致。

