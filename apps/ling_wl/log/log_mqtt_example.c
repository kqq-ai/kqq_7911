#include "log_mqtt.h"
#include "system/includes.h"
#include <string.h>

// 示例：如何使用MQTT日志系统
void log_mqtt_usage_example(void) {
    // 1. 配置MQTT连接参数（可选，如果不配置则使用默认值）
    log_mqtt_set_config(
        "101.126.136.69",           // MQTT服务器地址
        1883,                       // 端口
        "ling_wl_log_client_001",   // 客户端ID
        "ZLCNC5E21XEEEF6",          // 用户名
        "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJkYXRhIjoiS0NDTkM1RjEyQkVFRUo1IiwiaWF0IjoxNzUwNzUyNTQxLCJpc3MiOiJ3b25kZXJsYW5kIiwic3ViIjoiand0LXRva2VuIiwibmJmIjoxNzUwNzUyNTQxfQ.8PTDptxKvY8vSsyzpYQmZkYn_zBrWz1g6YCl11XrRS8", // 密码
        "h-log"                     // 发布主题
    );
    
    // 2. 启动MQTT日志系统
    int result = log_mqtt_start();
    if (result != 0) {
        printf("MQTT日志系统启动失败\n");
        return;
    }
    
    // 3. 使用日志函数（会自动发送到MQTT服务器）
    log_mqtt(LOG_INFO, "系统启动成功");
    log_mqtt(LOG_DEBUG, "调试信息: 温度=%d, 湿度=%d", 25, 60);
    log_mqtt(LOG_WARN, "警告: 电池电量低，剩余%d%%", 15);
    log_mqtt(LOG_ERROR, "错误: 传感器读取失败，错误码=%d", -123);
    
    // 4. 检查连接状态
    if (log_mqtt_is_connected()) {
        printf("MQTT连接正常\n");
    } else {
        printf("MQTT连接异常\n");
    }
    
    // 5. 手动发送消息
    log_mqtt_send_message("h-log", "手动发送的测试消息");
    
    // 6. 发送日志文件（如果有的话）
    // log_mqtt_send_file("h-log", "/log/system.log");
    
    // 7. 停止MQTT日志系统
    // log_mqtt_stop();
}

// 示例：在应用初始化时启动MQTT日志
int app_init_with_mqtt_log(void) {
    // 初始化系统
    printf("应用初始化开始...\n");
    
    // 启动MQTT日志系统
    if (log_mqtt_start() == 0) {
        log_mqtt(LOG_INFO, "应用初始化完成，MQTT日志系统已启动");
        return 0;
    } else {
        printf("MQTT日志系统启动失败，但应用继续运行\n");
        return -1;
    }
}

// 示例：在应用退出时停止MQTT日志
void app_exit_with_mqtt_log(void) {
    log_mqtt(LOG_INFO, "应用即将退出");
    log_mqtt_stop();
    printf("应用已退出\n");
}

// 示例：定期检查MQTT连接状态
void mqtt_connection_monitor(void) {
    static int check_count = 0;
    check_count++;
    
    if (check_count % 100 == 0) { // 每100次检查一次
        if (!log_mqtt_is_connected()) {
            log_mqtt(LOG_WARN, "MQTT连接断开，尝试重连...");
            log_mqtt_connect();
        } else {
            log_mqtt(LOG_DEBUG, "MQTT连接状态正常");
        }
    }
}

// 示例：使用不同的日志级别
void log_level_example(void) {
    // 调试信息
    log_mqtt(LOG_DEBUG, "这是调试信息，通常用于开发阶段");
    
    // 一般信息
    log_mqtt(LOG_INFO, "系统正常运行中");
    
    // 警告信息
    log_mqtt(LOG_WARN, "内存使用率较高: %d%%", 85);
    
    // 错误信息
    log_mqtt(LOG_ERROR, "网络连接超时，重试次数: %d", 3);
    
    // 致命错误
    log_mqtt(LOG_FATAL, "系统严重错误，需要重启");
}

// 示例：发送结构化数据
void log_structured_data_example(void) {
    // 发送JSON格式的数据
    log_mqtt_send_message("h-log", 
        "{\"timestamp\":\"2025-01-11 12:30:45\","
        "\"level\":\"INFO\","
        "\"module\":\"sensor\","
        "\"data\":{\"temperature\":25.5,\"humidity\":60.2,\"pressure\":1013.25}}");
    
    // 发送CSV格式的数据
    log_mqtt_send_message("h-log", 
        "2025-01-11 12:30:45,INFO,sensor,25.5,60.2,1013.25");
}

// 示例：批量发送日志
void batch_log_example(void) {
    for (int i = 0; i < 10; i++) {
        log_mqtt(LOG_INFO, "批量日志消息 #%d", i + 1);
        os_time_dly(1); // 延时10ms，避免发送过快
    }
} 