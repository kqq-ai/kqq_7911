# 充电状态LED显示优化实施计划

## 📋 需求概述

**目标**: 在充电过程中，根据电池电量百分比动态调整LED显示效果：
- **电量 < 80%**: 黄灯闪烁（呼吸效果）
- **电量 ≥ 80%**: 绿灯常亮

---

## 🔍 当前现状分析

### 现有充电LED逻辑
当前代码位于 `default.c`，充电状态只有简单的三种固定颜色：

```c
// 文件: apps/ling_wl/app_mode/default/default.c:48-50
static void set_led_charging(void)  { set_rgb_color(COLOR_YELLOW); }   // 充电中：黄色常亮
static void set_led_charged(void)   { set_rgb_color(COLOR_GREEN); }    // 充满电：绿色常亮
static void set_led_idle(void)      { set_rgb_color(COLOR_WHITE); }    // 空闲：白色
```

### 现有电压阈值定义
```c
// 文件: apps/ling_wl/include/app_cookie.h:25-26
#define LOW_BATT_VOLT 350           // 低电量阈值
#define LOW_BATT_VERY_SHUTDOWN 330  // 极低电量（关机阈值）
```

### 现有呼吸灯API
```c
// 文件: apps/ling_wl/util/led_breath.h
int led_breath_start(uint16_t color1_r, uint16_t color1_g, uint16_t color1_b,
                     uint16_t color2_r, uint16_t color2_g, uint16_t color2_b,
                     uint16_t cycle_count, uint16_t transition_time_ms, uint8_t step_delay_ms);

void led_set_color(uint8_t r, uint8_t g, uint8_t b);  // 停止呼吸并设置固定颜色
```

---

## 📐 电压与电量估算方案

由于没有精确的电量计芯片，采用**电压线性估算**方式：

| 电量百分比 | 估算电压 (单位: 0.01V) | 说明       |
| ---------- | ---------------------- | ---------- |
| 0%         | 330                    | 关机阈值   |
| 20%        | 350                    | 低电量预警 |
| 80%        | 400                    | 接近满电   |
| 100%       | 420                    | 满电       |

**80% 电量对应电压估算**:
```
V_80% = V_min + (V_max - V_min) × 0.8
      = 330 + (420 - 330) × 0.8
      = 330 + 72
      = 402 (约 4.02V)
```

建议定义新常量：
```c
#define BATT_80_PERCENT_VOLT 400  // 80%电量对应电压（简化估算）
```

---

## 🛠️ 修改计划

### 修改点 1️⃣: 添加电量阈值常量

**文件**: `apps/ling_wl/include/app_cookie.h`

**修改内容**:
```c
// 在第26行后添加
#define BATT_80_PERCENT_VOLT 400  // 80%电量对应电压阈值（用于充电显示切换）
```

**原因**: 集中管理电压阈值，便于后续调整和维护。

---

### 修改点 2️⃣: 添加充电状态管理变量

**文件**: `apps/ling_wl/app_mode/default/default.c`

**修改位置**: 文件顶部（约第36行附近）

**新增内容**:
```c
#include "util/led_breath.h"  // 添加呼吸灯头文件

// 充电LED状态追踪（避免重复设置）
typedef enum {
    CHARGE_LED_STATE_UNKNOWN = 0,
    CHARGE_LED_STATE_BREATHING_YELLOW,  // 电量<80%，黄灯呼吸
    CHARGE_LED_STATE_SOLID_GREEN,       // 电量>=80%，绿灯常亮
} charge_led_state_t;

static charge_led_state_t g_charge_led_state = CHARGE_LED_STATE_UNKNOWN;
```

**原因**: 
- 避免在loop中每帧重复调用 `led_breath_start()` 或 `led_set_color()`
- 只在状态真正改变时切换LED效果

---

### 修改点 3️⃣: 新增动态充电LED函数

**文件**: `apps/ling_wl/app_mode/default/default.c`

**在现有 `set_led_charging()` 后添加新函数**:

```c
/**
 * @brief 根据电压动态更新充电LED状态
 * @param voltage 当前电池电压 (单位: 0.01V)
 * 
 * 逻辑:
 *   - 电压 < BATT_80_PERCENT_VOLT (400): 黄灯呼吸 (表示充电中)
 *   - 电压 >= BATT_80_PERCENT_VOLT (400): 绿灯常亮 (表示快充满或已充满)
 */
static void update_charging_led(u16 voltage)
{
    if (voltage < BATT_80_PERCENT_VOLT) {
        // 电量 < 80%: 黄灯呼吸
        if (g_charge_led_state != CHARGE_LED_STATE_BREATHING_YELLOW) {
            g_charge_led_state = CHARGE_LED_STATE_BREATHING_YELLOW;
            // 黄色呼吸: 从黄色渐变到黑色再回来, 无限循环, 每次过渡800ms
            led_breath_start(COLOR_YELLOW, COLOR_BLACK, 0, 800, 50);
            log_i("[default] Charging LED: Yellow breathing (voltage=%d < 400)", voltage);
        }
    } else {
        // 电量 >= 80%: 绿灯常亮
        if (g_charge_led_state != CHARGE_LED_STATE_SOLID_GREEN) {
            g_charge_led_state = CHARGE_LED_STATE_SOLID_GREEN;
            led_set_color(COLOR_GREEN);  // 停止呼吸并设为绿色
            log_i("[default] Charging LED: Green solid (voltage=%d >= 400)", voltage);
        }
    }
}
```

**原因**:
- 使用 `led_breath_start()` 实现黄灯呼吸效果
- 使用 `led_set_color()` 停止呼吸并设置绿灯常亮
- 状态追踪避免重复调用

---

### 修改点 4️⃣: 修改充电事件处理

**文件**: `apps/ling_wl/app_mode/default/default.c`

**修改位置**: `default_input_event_handler_cb()` 函数中的 `LWL_EVENT_TYPE_CHARGE` 分支（约第73-90行）

**修改前**:
```c
case LWL_EVENT_TYPE_CHARGE:
    if (e->charge_cmd.charge_event_type == CHARGE_EVENT_TYPE_CHARGE_IN) {
        set_led_charging();
    }
    else if (e->charge_cmd.charge_event_type == CHARGE_EVENT_TYPE_CHARGE_OUT) {
        set_led_idle();
    }
    else if (e->charge_cmd.charge_event_type == CHARGE_EVENT_TYPE_CHARGE_FULL) {
        set_led_charged();
    }
    break;
```

**修改后**:
```c
case LWL_EVENT_TYPE_CHARGE:
    if (e->charge_cmd.charge_event_type == CHARGE_EVENT_TYPE_CHARGE_IN) {
        // 充电器插入：根据当前电量决定LED状态
        u16 voltage = get_vbat_level();
        g_charge_led_state = CHARGE_LED_STATE_UNKNOWN;  // 重置状态以触发更新
        update_charging_led(voltage);
    }
    else if (e->charge_cmd.charge_event_type == CHARGE_EVENT_TYPE_CHARGE_OUT) {
        // 充电器拔出：停止呼吸，恢复白灯
        g_charge_led_state = CHARGE_LED_STATE_UNKNOWN;
        led_set_color(COLOR_WHITE);
    }
    else if (e->charge_cmd.charge_event_type == CHARGE_EVENT_TYPE_CHARGE_FULL) {
        // 充满电：绿灯常亮
        g_charge_led_state = CHARGE_LED_STATE_SOLID_GREEN;
        led_set_color(COLOR_GREEN);
    }
    break;
```

**原因**:
- 充电器插入时，不再简单显示黄灯，而是根据当前电压动态决定
- 充电器拔出时，使用 `led_set_color()` 确保停止任何正在进行的呼吸效果
- 充满电事件仍保持绿灯常亮

---

### 修改点 5️⃣: 在充电循环中持续更新LED

**文件**: `apps/ling_wl/app_mode/default/default.c`

**修改位置**: `app_default_loop()` 函数（约第135行附近）

**在 `if (charge_in)` 分支中添加电量检测**:

```c
void app_default_loop()
{
    int charge_in = gpio_read(CHARGE_IN_IO);
    u16 voltage = get_vbat_level();
    
    // ... 现有按键处理代码 ...
    
    if (charge_in) {
        g_sensor_reset_alive_timer();
        
        // 【新增】在充电过程中持续检测电压，动态更新LED
        update_charging_led(voltage);
        
    } else {
        // ... 现有超时关机代码 ...
    }
}
```

**原因**:
- 充电过程中电压会逐渐上升
- 需要在loop中持续检测，当电压跨过80%阈值时及时切换LED状态
- 状态追踪机制确保只在阈值跨越时才调用LED函数

---

### 修改点 6️⃣: 修改初始化逻辑

**文件**: `apps/ling_wl/app_mode/default/default.c`

**修改位置**: `default_init()` 函数中的充电检测部分（约第107-113行）

**修改前**:
```c
if (charge_in) {
    if (charge_full) {
        set_led_charged();
    } else {
        set_led_charging();
    }
    play_system_music("Charging.mp3");
}
```

**修改后**:
```c
if (charge_in) {
    g_charge_led_state = CHARGE_LED_STATE_UNKNOWN;  // 重置状态
    if (charge_full) {
        g_charge_led_state = CHARGE_LED_STATE_SOLID_GREEN;
        led_set_color(COLOR_GREEN);
    } else {
        update_charging_led(voltage);  // 根据电压动态设置
    }
    play_system_music("Charging.mp3");
}
```

**原因**: 初始化时也需要根据当前电压正确设置LED状态。

---

## ⚠️ 注意事项

### 1. 线程安全
- `led_breath_start()` 内部使用互斥锁保护，可以安全地从事件回调和主循环调用
- 状态变量 `g_charge_led_state` 只在单一线程(`lwl_app_task`)中访问，无需额外加锁

### 2. 电压读取精度
- `get_vbat_level()` 返回的电压值受ADC采样噪声影响
- 建议在阈值附近增加**滞后区间**（hysteresis），避免LED频繁切换：
  ```c
  #define BATT_80_PERCENT_VOLT_HIGH 405  // 从呼吸切到常亮
  #define BATT_80_PERCENT_VOLT_LOW  395  // 从常亮切到呼吸（如果电压下降）
  ```
- 当前简化方案不加滞后，后续观察是否有闪烁问题再优化

### 3. 呼吸效果参数选择
```c
led_breath_start(COLOR_YELLOW, COLOR_BLACK, 0, 800, 50);
//               |起始颜色    |目标颜色    |无限循环|800ms过渡|50ms步进
```
- **过渡时间800ms**: 一个完整的呼吸周期约 1.6s (渐暗800ms + 渐亮800ms)
- **步进50ms**: 每50ms更新一次颜色，足够平滑
- 如需调整呼吸速度，修改 `transition_time_ms` 参数

### 4. 充满电判断
- 硬件 `CHARGE_FULL_IO` 信号优先级最高
- 即使电压达到80%以上，也可能尚未完全充满
- 保留 `CHARGE_EVENT_TYPE_CHARGE_FULL` 事件处理

### 5. 退出DEFAULT模式时的清理
在 `default_unint()` 中重置状态：
```c
static int default_unint(void)
{
    lwl_event_handler_remove(default_input_event_handler_cb);
    g_charge_led_state = CHARGE_LED_STATE_UNKNOWN;  // 重置充电LED状态
    return 0;
}
```

---

## 📁 涉及文件清单

| 文件路径                                  | 修改类型 | 说明                                       |
| ----------------------------------------- | -------- | ------------------------------------------ |
| `apps/ling_wl/include/app_cookie.h`       | 新增常量 | 添加 `BATT_80_PERCENT_VOLT`                |
| `apps/ling_wl/app_mode/default/default.c` | 主要修改 | 添加状态管理、新增函数、修改事件处理和循环 |

---

## 🧪 测试用例

| 测试场景                     | 预期结果                |
| ---------------------------- | ----------------------- |
| 低电量开始充电 (电压 < 400)  | 黄灯呼吸                |
| 充电过程中电压上升超过400    | 自动切换为绿灯常亮      |
| 高电量开始充电 (电压 >= 400) | 直接绿灯常亮            |
| 充满电信号 (CHARGE_FULL_IO)  | 绿灯常亮                |
| 拔出充电器                   | 停止呼吸，恢复白灯      |
| 反复插拔充电器               | LED状态正确切换，无异常 |

---

## 📝 实施顺序

1. ✅ 在 `app_cookie.h` 添加 `BATT_80_PERCENT_VOLT` 常量
2. ✅ 在 `default.c` 添加头文件引用和状态变量
3. ✅ 在 `default.c` 实现 `update_charging_led()` 函数
4. ✅ 修改 `default_init()` 初始化逻辑
5. ✅ 修改 `default_input_event_handler_cb()` 事件处理
6. ✅ 修改 `app_default_loop()` 循环检测
7. ✅ 修改 `default_unint()` 清理逻辑
8. 🧪 编译测试
9. 🧪 硬件验证

---

*文档创建时间: 2026-01-28*
*项目: kqq_7911*
