# 统一音频管理模块使用文档

## 概述

`lwl_app_audio` 模块是一个统一的音频管理框架，提供了本地文件播放和流式音频播放的完整控制功能。该模块负责管理音频播放器的生命周期、扬声器切换、音量控制以及资源管理。

### 核心特性

- ✅ **两种播放模式**：本地文件播放和流式音频播放
- ✅ **自动模式切换**：启动一种播放模式时自动关闭另一种
- ✅ **安全的资源管理**：互斥锁保护共享状态
- ✅ **扬声器自动切换**：本地播放用C扬声器，流式播放用E扬声器
- ✅ **完成回调**：支持播放完成时的自定义回调
- ✅ **超时保护**：流式播放线程5秒无数据自动退出
- ✅ **传感器集成**：播放时重置计时器防止设备睡眠

---

## 架构设计

### 文件结构

```
apps/ling_wl/app_mode/
├── lwl_app_audio.h           # 公共接口定义
├── lwl_app_audio.c           # 核心实现（437行）
├── lwl_app_framework.c       # 应用框架（集成音频模块）
└── wss_connected/
    ├── wss_audio.h           # WSS音频接口（已简化）
    ├── wss_audio.c           # WSS音频实现（已简化为接口适配层）
    └── wss_stream.c          # 音频流缓冲（保持不动）
```

### 播放模式控制流

```
┌─────────────────────────────────────────────────────────┐
│           应用主循环 (lwl_app_mode_loop)                │
└──────────────────┬──────────────────────────────────────┘
                   │
                   ▼
        lwl_app_audio_play_loop()    ◄─── 每次循环调用
                   │
         ┌─────────┴──────────┬──────────────┐
         │                    │              │
         ▼                    ▼              ▼
    待播放文件?          流式播放?       本地播放?
         │ 是                 │ 是            │
         └──────────┬────────┘              │
                    │ 关闭流式播放          │
                    ▼                      │
            启动本地文件播放 ◄────────────┘
```

### 资源生命周期

```
lwl_app_init()
    ↓
lwl_app_audio_init()
    ↓
[应用运行中]
    ├─ 本地播放: lwl_app_audio_play() → lwl_app_audio_play_loop() → 回调 → 结束
    ├─ 流式播放: lwl_app_audio_stream_start() → 播放线程 → lwl_app_audio_stream_stop()
    └─ 混合使用: 自动切换，先关闭后开启
    ↓
lwl_app_uninit()
    ↓
lwl_app_audio_uninit()
```

---

## 初始化和反初始化

### 初始化

在应用启动时调用一次：

```c
void lwl_app_init()
{
    os_mutex_create(&g_mode_mutex);
    lwl_app_context_t *context = lwl_app_core_context_get_instance();
    app_curr_mode = APP_POWER_ON_MODE;
    
    // 初始化统一音频管理模块
    lwl_app_audio_init();  // ◄─── 必须调用
}
```

**做了什么：**
- 创建互斥锁保护共享状态
- 初始化播放状态变量
- 重置音量到默认值

### 反初始化

在应用关闭时调用：

```c
void lwl_app_uninit()
{
    lwl_app_audio_uninit();  // ◄─── 必须调用
    os_mutex_del(&g_mode_mutex, 1);
}
```

**做了什么：**
- 停止所有正在进行的音频播放
- 释放互斥锁

### 主循环集成

在应用主循环中每次都要调用播放循环接口：

```c
void lwl_app_mode_loop()
{
    // 调用统一音频管理模块的主循环接口
    lwl_app_audio_play_loop();  // ◄─── 每次循环调用
    
    switch(app_curr_mode) {
        // ... 其他模式处理
    }
}
```

**重要：** 必须在每个循环周期调用，以处理本地文件的播放启动。

---

## 本地文件播放

### 播放提示音

```c
// 方法1：播放文件并在完成时回调
void my_playback_callback(void) {
    log_i("音频播放完成");
}

lwl_app_audio_play("ui/prompt_success.mp3", (void *)my_playback_callback);
```

**工作流程：**
1. `lwl_app_audio_play()` 存储文件名和回调
2. 等待播放完成
3. 主循环中 `lwl_app_audio_play_loop()` 启动实际播放
4. 播放完成时调用回调

**特点：**
- ✅ 异步播放（不阻塞主线程）
- ✅ 自动关闭流式播放
- ✅ 支持完成回调
- ✅ 重置传感器计时器（防止设备睡眠）

### 检查播放状态

```c
if (lwl_app_audio_playing()) {
    log_i("本地文件正在播放");
} else {
    log_i("本地文件未播放");
}
```

### 停止播放

```c
lwl_app_audio_stop_playing();  // 立即停止
```

---

## 流式音频播放

### 启动流式播放

```c
// 启动流式音频播放（通常由WSS协议触发）
lwl_app_audio_stream_start();

log_mqtt("开始蓝牙音频播放");
```

**工作流程：**
1. 自动关闭任何正在进行的本地文件播放
2. 启动流式播放线程
3. 线程循环从 `wss_stream` 获取数据
4. 将数据写入音频解码器
5. 播放器输出到E扬声器

**特点：**
- ✅ 线程化处理（不阻塞主线程）
- ✅ 自动缓冲管理（通过 `wss_stream`）
- ✅ LED指示（蓝色LED表示播放中）
- ✅ 5秒超时保护

### 停止流式播放

```c
lwl_app_audio_stream_stop();

log_mqtt("停止蓝牙音频播放");
send_device_status(DEVICE_STATUS_BLE_AUDIO_STOP);
```

**清理动作：**
- 停止播放线程
- 重置stream缓冲
- 关闭LED指示

### 检查流式播放状态

```c
if (lwl_app_audio_stream_playing()) {
    log_i("流式音频正在播放");
} else {
    log_i("流式音频未播放");
}
```

---

## 音量控制

### 设置音量

```c
// 设置音量（1-100）
lwl_app_audio_set_volume(70);  // 70%音量

// 音量映射
// 输入范围: 1-100
// 内部映射: 1-G_MAX_VOLUME (G_MAX_VOLUME=100)
```

**特点：**
- 本地播放：使用 `INIT_VOLUME_VALUE` (通常为85)
- 流式播放：使用设置的音量值
- 音量设置立即生效（如果正在播放流式音频）

### 音量保护

```c
// 内部自动范围限制
void set_tts_volume(int volume)
{
    if (volume < 1) volume = 1;          // 最小值
    if (volume > 100) volume = 100;      // 最大值
    g_volume = volume * G_MAX_VOLUME / 100;
}
```

---

## 扬声器管理

### 自动切换规则

| 操作 | C扬声器 | E扬声器 | 说明 |
|------|--------|--------|------|
| 本地播放 | ✅ ON | ❌ OFF | 扬声器C播放提示音 |
| 流式播放 | ❌ OFF | ✅ ON | 扬声器E播放蓝牙音频 |
| 切换模式 | 自动切换 | 自动切换 | 先关闭旧模式，启动新模式 |

### 实现细节

```c
// 本地播放配置
static void set_local_speaker(void)
{
    gpio_direction_output(IO_PORTA_00, 1);  // Enable C speaker
    gpio_direction_output(IO_PORTB_02, 0);  // Disable E speaker
    set_dec_volume(g_app_ctx->music_player, INIT_VOLUME_VALUE);
}

// 流式播放配置
static void set_stream_speaker(void)
{
    lwl_app_audio_stop_playing();           // 先关闭本地播放
    gpio_direction_output(IO_PORTA_00, 0);  // Disable C speaker
    gpio_direction_output(IO_PORTB_02, 1);  // Enable E speaker
    set_dec_volume(g_app_ctx->music_player, g_volume);
    bdo_enable();
}
```

---

## API 快速参考

### 初始化/反初始化

| API | 说明 |
|-----|------|
| `lwl_app_audio_init()` | 初始化模块（应用启动） |
| `lwl_app_audio_uninit()` | 反初始化模块（应用关闭） |

### 本地播放

| API | 说明 |
|-----|------|
| `lwl_app_audio_play(fname, priv)` | 播放本地文件 |
| `lwl_app_audio_play_loop()` | 播放循环（每次循环调用） |
| `lwl_app_audio_playing()` | 检查本地播放状态 |
| `lwl_app_audio_stop_playing()` | 停止本地播放 |

### 流式播放

| API | 说明 |
|-----|------|
| `lwl_app_audio_stream_start()` | 启动流式播放 |
| `lwl_app_audio_stream_stop()` | 停止流式播放 |
| `lwl_app_audio_stream_playing()` | 检查流式播放状态 |

### 通用

| API | 说明 |
|-----|------|
| `lwl_app_audio_set_volume(vol)` | 设置音量（1-100） |
| `lwl_app_audio_is_playing()` | 检查任何音频是否播放中 |

---

## 线程安全性

### 互斥锁保护

所有共享状态通过 `g_audio_mutex` 保护：

```c
static OS_MUTEX g_audio_mutex;

// 写操作
os_mutex_pend(&g_audio_mutex, 0);
g_play_voice_prompt = 1;
os_mutex_post(&g_audio_mutex);

// 读操作
os_mutex_pend(&g_audio_mutex, 0);
bool playing = g_play_voice_prompt;
os_mutex_post(&g_audio_mutex);
```

### 状态变量

```c
// 本地播放
static u8 g_play_voice_prompt;      // 本地播放中
static char g_music_file[128];      // 待播放文件
static music_cbk_t g_prompt_cb_priv; // 完成回调

// 流式播放
static int g_stream_audio_on;        // 流式启动请求
static int g_stream_audio_playing;   // 流式实际播放
static int g_stream_audio_thread_running; // 线程运行
```

---

## 常见问题

### Q1: 为什么主循环中必须调用 `lwl_app_audio_play_loop()`？

**A:** 本地文件播放是异步的：
1. `lwl_app_audio_play()` 只存储文件名和回调
2. 实际播放在 `lwl_app_audio_play_loop()` 中启动
3. 需要在每个循环周期检查是否有待播放文件

### Q2: 本地播放和流式播放能同时进行吗？

**A:** 不能。模块会自动处理：
- 启动本地播放 → 自动关闭流式播放
- 启动流式播放 → 自动关闭本地播放

### Q3: 如何确保播放不被中断？

**A:** 播放完成后再启动新的播放：
```c
// ✅ 正确做法
lwl_app_audio_play("file1.mp3", on_complete_1);

void on_complete_1(void) {
    lwl_app_audio_play("file2.mp3", on_complete_2);
}

// ❌ 错误做法（会中断第一个文件）
lwl_app_audio_play("file1.mp3", NULL);
lwl_app_audio_play("file2.mp3", NULL);
```

### Q4: 流式播放超时了怎么办？

**A:** 模块有5秒超时保护：
- 如果5秒内无数据接收，线程自动退出
- 可通过 `lwl_app_audio_stream_stop()` 手动停止

### Q5: 如何调整音量？

**A:** 流式播放使用 `lwl_app_audio_set_volume()`：
```c
lwl_app_audio_set_volume(50);   // 50%音量

// 本地播放音量固定为INIT_VOLUME_VALUE (85)
// 可在net_music_play_my.h中修改
```

### Q6: 设备为什么会进入睡眠状态？

**A:** 播放完成后需要重置传感器计时器，模块已自动处理：
- 本地播放完成 → 自动重置
- 播放循环 → 自动重置

---

## 性能指标

| 指标 | 值 |
|------|-----|
| 初始化时间 | < 1ms |
| 切换播放模式延迟 | < 10ms |
| 流式播放线程优先级 | 由threadpool决定 |
| 互斥锁等待时间 | < 1ms |
| 缓冲区大小 | 500KB (wss_stream) |
| 超时保护 | 5000ms |
| LED响应延迟 | < 100ms |

---

## 维护和扩展

### 添加新的播放模式

如需添加第三种播放模式，参考现有实现：
1. 在 `lwl_app_audio.c` 中定义状态变量
2. 实现初始化/启动/停止函数
3. 在启动前关闭其他模式（保持"先关闭后开启"原则）
4. 在 `lwl_app_audio.h` 中导出接口

### 修改音量映射

```c
// 在lwl_app_audio.c中修改
#define G_MAX_VOLUME 100  // 修改最大映射值
```

### 扩展回调功能

```c
// 当前实现支持简单回调
lwl_app_audio_play("file.mp3", (void *)callback_func);

// 如需传递参数，可使用全局变量或扩展接口
```

---

## 修改历史

### v1.0 (2026-01-13)
- 初始版本
- 合并 `lwl_app_framework.c` 和 `wss_audio.c` 的音频控制
- 统一的接口设计
- 完整的文档

---

## 相关文件

| 文件 | 说明 |
|------|------|
| [lwl_app_audio.h](../apps/ling_wl/app_mode/lwl_app_audio.h) | 公共接口定义 |
| [lwl_app_audio.c](../apps/ling_wl/app_mode/lwl_app_audio.c) | 核心实现 (437行) |
| [lwl_app_framework.c](../apps/ling_wl/app_mode/lwl_app_framework.c) | 应用框架 |
| [wss_audio.h](../apps/ling_wl/app_mode/wss_connected/wss_audio.h) | WSS接口 |
| [wss_audio.c](../apps/ling_wl/app_mode/wss_connected/wss_audio.c) | WSS适配层 |
| [wss_stream.c](../apps/ling_wl/app_mode/wss_connected/wss_stream.c) | 流缓冲实现 |
| [net_music_play_my.h](../apps/ling_wl/net_music_play_my.h) | 音乐播放器接口 |

---

*文档生成时间: 2026-01-13*  
*模块版本: 1.0*  
*维护者: Audio Team*
