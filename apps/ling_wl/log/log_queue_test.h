#ifndef __LOG_QUEUE_TEST_H__
#define __LOG_QUEUE_TEST_H__

#include "../../include_lib/net/mqtt/MQTTClient.h"
#include <stdarg.h>
#include <time.h>
/* 日志函数 */
void log_mqtt(const char* format, ...);
/* MQTT连接管理函数 */
int log_mqtt_batch_start(void);

void log_mqtt_batch_stop(void);

#endif // __LOG_QUEUE_TEST_H__