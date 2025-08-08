/*
 * @Author: lipu123 3493235509@qq.com
 * @Date: 2025-07-11 12:50:23
 * @LastEditors: lipu123 3493235509@qq.com
 * @LastEditTime: 2025-07-11 16:43:36
 * @FilePath: \kqq_7911-0703m\apps\ling_wl\log\log_mqtt.h
 * @Description: 这是默认设置,请设置`customMade`, 打开koroFileHeader查看配置 进行设置: https://github.com/OBKoro1/koro1FileHeader/wiki/%E9%85%8D%E7%BD%AE
 */
#ifndef LOG_MQTT_H
#define LOG_MQTT_H

#include <stdarg.h>
#include <time.h>
#include "../../include_lib/net/mqtt/MQTTClient.h"

/* 日志级别 */
typedef enum {
    LOG_DEBUG,
    LOG_INFO,
    LOG_WARN,
    LOG_ERROR
} LogLevel;

/* 日志函数 */
void log_mqtt_level(LogLevel level, const char* format, ...);
void log_mqtt(const char* format, ...);
/* MQTT连接管理函数 */
int log_mqtt_start(void);
int mqtt_subscribe_reset();

void log_mqtt_stop(void);

void log_mqtt_battery(const char* sn_param,int battery);
#endif /* LOG_MQTT_H */