/**
 * @file battery_level.c
 * @brief 电池电量百分比转换模块实现
 * @details 基于3.7V锂电池特性，使用17点查找表和线性插值算法
 */

#include "battery_level.h"

// 电压-电量映射表（从高到低排列，便于查找）
// 电压单位：0.01V（与get_vbat_level()一致）
typedef struct {
  uint16_t voltage; // 电压 (0.01V)
  uint8_t percent;  // 对应电量百分比
} voltage_percent_t;

static const voltage_percent_t voltage_table[] = {
    {420, 100}, // 4.20V = 100%
    {415, 95},  // 4.15V = 95%
    {410, 90},  // 4.10V = 90%
    {405, 85},  // 4.05V = 85%
    {400, 80},  // 4.00V = 80%  ← 关键阈值（充电灯效切换点）
    {395, 75},  // 3.95V = 75%
    {390, 70},  // 3.90V = 70%
    {385, 60},  // 3.85V = 60%
    {380, 50},  // 3.80V = 50%
    {375, 40},  // 3.75V = 40%
    {370, 30},  // 3.70V = 30%
    {365, 25},  // 3.65V = 25%
    {360, 20},  // 3.60V = 20%  ← 关键阈值（低电量警告）
    {355, 15},  // 3.55V = 15%
    {350, 10},  // 3.50V = 10%
    {345, 5},   // 3.45V = 5%
    {330, 0},   // 3.30V = 0%   ← 关机电压
};

#define TABLE_SIZE (sizeof(voltage_table) / sizeof(voltage_table[0]))

/**
 * @brief 根据电压返回电量百分比
 * @param voltage 电压值（0.01V），如350表示3.50V
 * @return 电量百分比 0-100
 */
uint8_t battery_voltage_to_percent(uint16_t voltage) {
  // 超过最大值，返回100%
  if (voltage >= voltage_table[0].voltage) {
    return 100;
  }

  // 低于最小值，返回0%
  if (voltage <= voltage_table[TABLE_SIZE - 1].voltage) {
    return 0;
  }

  // 线性插值查找
  for (int i = 0; i < TABLE_SIZE - 1; i++) {
    if (voltage >= voltage_table[i + 1].voltage) {
      // 在 i 和 i+1 之间进行线性插值
      uint16_t v_high = voltage_table[i].voltage;
      uint16_t v_low = voltage_table[i + 1].voltage;
      uint8_t p_high = voltage_table[i].percent;
      uint8_t p_low = voltage_table[i + 1].percent;

      // 线性插值公式：percent = p_low + (voltage - v_low) * (p_high - p_low) /
      // (v_high - v_low)
      uint8_t percent =
          p_low + ((voltage - v_low) * (p_high - p_low)) / (v_high - v_low);
      return percent;
    }
  }

  return 0; // 兜底
}

/**
 * @brief 获取当前电量百分比
 * @return 电量百分比 0-100
 */
uint8_t get_battery_percent(void) {
  uint16_t voltage = get_vbat_level();
  return battery_voltage_to_percent(voltage);
}

/**
 * @brief 判断电量是否低于20%
 * @return true=低电量(<20%), false=正常
 */
bool is_battery_low(void) {
  uint16_t voltage = get_vbat_level();
  return battery_voltage_to_percent(voltage) < 20;
}

/**
 * @brief 判断电量是否高于80%
 * @return true=高电量(≥80%), false=未充满
 */
bool is_battery_high(void) {
  uint16_t voltage = get_vbat_level();
  return battery_voltage_to_percent(voltage) >= 80;
}
