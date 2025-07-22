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

/* 避免与项目文件系统冲突，使用printf替代stdio.h */
extern int printf(const char *format, ...);

/* 避免与项目CPU头文件冲突，使用项目自己的bool定义 */

/* 日志级别 */
typedef enum {
    LOG_DEBUG,
    LOG_INFO,
    LOG_WARN,
    LOG_ERROR,
    LOG_FATAL
} LogLevel;

/* 日志函数 */
void log_mqtt(const char* format, ...);

/* MQTT连接管理函数 */
int log_mqtt_init(void);
int log_mqtt_connect(void);
void log_mqtt_disconnect(void);
int log_mqtt_start(void);
void log_mqtt_stop(void);
void log_mqtt_set_config(const char* server, int port, const char* client_id, 
                        const char* username, const char* password, const char* topic);

/* MQTT日志相关函数 */
void log_mqtt_set_client(Client* client);
int log_mqtt_send_message(const char* topic, const char* message);
bool log_mqtt_is_connected(void);

/* 全局MQTT客户端指针 */
extern Client* g_mqtt_log_client;

#endif /* LOG_MQTT_H */