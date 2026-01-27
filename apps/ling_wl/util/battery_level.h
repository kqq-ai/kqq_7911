#ifndef __BATTERY_LEVEL_H__
#define __BATTERY_LEVEL_H__

#include <stdbool.h>
#include <stdint.h>

/**
 * @file battery_level.h
 * @brief 电池电量百分比转换模块
 * @details 提供电压到电量百分比的转换功能，支持80%/20%阈值判断
 */

// 外部电压获取函数（已存在于系统中）
extern uint16_t get_vbat_level(void);

/**
 * @brief 根据电压返回电量百分比
 * @param voltage 电压值（单位：0.01V），如350表示3.50V
 * @return 电量百分比 0-100
 */
uint8_t battery_voltage_to_percent(uint16_t voltage);

/**
 * @brief 获取当前电量百分比
 * @return 电量百分比 0-100
 */
uint8_t get_battery_percent(void);

/**
 * @brief 判断电量是否低于20%
 * @return true=低电量(<20%), false=正常
 */
bool is_battery_low(void);

/**
 * @brief 判断电量是否高于80%
 * @return true=高电量(≥80%), false=未充满
 */
bool is_battery_high(void);

// 关键电压阈值定义（替代原app_cookie.h中的定义）
#define VBAT_FULL 420       // 4.20V = 100%
#define VBAT_80_PERCENT 400 // 4.00V = 80%（充电灯效切换点）
#define VBAT_20_PERCENT 360 // 3.60V = 20%（低电量警告）
#define VBAT_SHUTDOWN 330   // 3.30V = 0%（强制关机）

// 向后兼容旧代码的宏定义
#define LOW_BATT_VOLT 360          // 对应20%（原350改为360）
#define LOW_BATT_VERY_SHUTDOWN 330 // 保持不变

#endif // __BATTERY_LEVEL_H__
