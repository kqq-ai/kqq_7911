/*
 * @Author: lipu123 3493235509@qq.com
 * @Date: 2025-07-11 12:50:23
 * @LastEditors: lipu123 3493235509@qq.com
 * @LastEditTime: 2025-07-11 16:43:36
 * @FilePath: \kqq_7911-0703m\apps\cookie\log\log_mqtt.h
 * @Description: 这是默认设置,请设置`customMade`, 打开koroFileHeader查看配置 进行设置: https://github.com/OBKoro1/koro1FileHeader/wiki/%E9%85%8D%E7%BD%AE
 */
#ifndef LOG_MQTT_H
#define LOG_MQTT_H

#include <stdarg.h>
#include <time.h>
#include "../../include_lib/net/mqtt/MQTTClient.h"

/**
 * @brief 打印MQTT日志
 * @param format 格式化字符串
 * @note 注意，使用该函数不可以在字符串中包含\n换行符
 */
void log_mqtt(const char* format, ...);
void log_mqtt_set_battery_level(char *sn_battery, int battery_level);
int log_mqtt_send_message_sync(const char* topic, const int progress);

/* MQTT连接管理函数 */
int log_mqtt_init(void);
int log_mqtt_start(void);
void log_mqtt_stop(void);

#endif /* LOG_MQTT_H */