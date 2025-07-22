/*
 * log_mqtt.c - 修复MQTT连接问题的版本
 */

#include "log_mqtt.h"
#include <string.h>
#include "system/includes.h"  // 杰理芯片系统头文件

/* 全局MQTT客户端指针 */
static Client* g_mqtt_client = NULL;

/* MQTT连接配置结构体 */
typedef struct {
    char* server;           // MQTT服务器地址
    int port;                  // MQTT服务器端口
    char* client_id;        // 客户端ID
    char* username;         // 用户名
    char* password;        // 密码
    char* topic;            // 发布主题
    int keep_alive;            // 保活时间(秒)
    bool use_tls;              // 是否使用TLS
    bool auto_reconnect;       // 是否自动重连
} mqtt_log_config_t;

/* 全局MQTT配置 */
static mqtt_log_config_t g_mqtt_config = {
    .server = "101.126.136.69",
    .port = 1883,
    .client_id = "12345|securemode=3,signmethod=hmacsha1|",
    .username = "ZLCNC5E21XEEEF6",
    .password = "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJkYXRhIjoiS0NDTkM1RjEyQkVFRUo1IiwiaWF0IjoxNzUwNzUyNTQxLCJpc3MiOiJ3b25kZXJsYW5kIiwic3ViIjoiand0LXRva2VuIiwibmJmIjoxNzUwNzUyNTQxfQ.8PTDptxKvY8vSsyzpYQmZkYn_zBrWz1g6YCl11XrRS8",
    .topic = "h-log",
    .keep_alive = 30,
    .use_tls = false,
    .auto_reconnect = true
};

/* MQTT客户端实例 */
static Client mqtt_client;
static Network mqtt_network;
static unsigned char mqtt_send_buf[1024];
static unsigned char mqtt_read_buf[2048];

/* 连接状态 */
static bool g_mqtt_connected = false;

/* 日志级别字符串映射 */
static const char* level_strings[] = {
    "DEBUG", "INFO", "WARN", "ERROR", "FATAL"
};

/* 设置MQTT连接参数 */
void log_mqtt_set_config(const char* server, int port, const char* client_id, 
                        const char* username, const char* password, const char* topic) {
    if (server) strncpy(g_mqtt_config.server, server, sizeof(g_mqtt_config.server) - 1);
    if (port > 0) g_mqtt_config.port = port;
    if (client_id) strncpy(g_mqtt_config.client_id, client_id, sizeof(g_mqtt_config.client_id) - 1);
    if (username) strncpy(g_mqtt_config.username, username, sizeof(g_mqtt_config.username) - 1);
    if (password) strncpy(g_mqtt_config.password, password, sizeof(g_mqtt_config.password) - 1);
    if (topic) strncpy(g_mqtt_config.topic, topic, sizeof(g_mqtt_config.topic) - 1);
}

/* 初始化MQTT网络 */
static int init_mqtt_network(void) {
    // 初始化网络
    NewNetwork(&mqtt_network);
    SetNetworkRecvTimeout(&mqtt_network, 2000);  // 2秒超时
    
    // 连接网络
    int rc = ConnectNetwork(&mqtt_network, g_mqtt_config.server, g_mqtt_config.port);
    if (rc != 0) {
        printf("MQTT network connection failed: %s:%d, error code: %d\n", 
               g_mqtt_config.server, g_mqtt_config.port, rc);
        return rc;
    }
    
    return 0;
}

/* 连接MQTT服务器 */
int log_mqtt_connect(void) {
    if (g_mqtt_connected) {
        return 0; // 已经连接
    }
    
    // 先断开可能存在的旧连接
    if (g_mqtt_client != NULL) {
        log_mqtt_disconnect();
    }
    
    // 初始化网络
    int rc = init_mqtt_network();
    if (rc != 0) {
        return rc;
    }
    
    // 初始化MQTT客户端
    MQTTClient(&mqtt_client, &mqtt_network, 5000, 
               mqtt_send_buf, sizeof(mqtt_send_buf),
               mqtt_read_buf, sizeof(mqtt_read_buf));
    
    g_mqtt_client = &mqtt_client;
    
    // 配置MQTT连接参数
    MQTTPacket_connectData connect_data = MQTTPacket_connectData_initializer;
    connect_data.MQTTVersion = 3;
    connect_data.clientID.cstring = g_mqtt_config.client_id;
    connect_data.username.cstring = g_mqtt_config.username;
    connect_data.password.cstring = g_mqtt_config.password;
    connect_data.keepAliveInterval = g_mqtt_config.keep_alive;
    connect_data.cleansession = 1;
    connect_data.willFlag = 0;
    
    // 连接MQTT服务器
    rc = MQTTConnect(g_mqtt_client, &connect_data);
    if (rc != 0) {
        printf("MQTT connection failed, error code: %d\n", rc);
        log_mqtt_disconnect();  // 连接失败，清理资源
        return rc;
    }
    
    g_mqtt_connected = true;
    printf("======MQTT connection successful: %s:%d\n", g_mqtt_config.server, g_mqtt_config.port);
    return 0;
}

/* 断开MQTT连接 */
void log_mqtt_disconnect(void) {
    if (g_mqtt_client != NULL && g_mqtt_connected) {
        MQTTDisconnect(g_mqtt_client);
        mqtt_network.disconnect(&mqtt_network);
        g_mqtt_connected = false;
        printf("MQTT connection disconnected\n");
    }
}

/* 检查MQTT连接状态 */
bool log_mqtt_is_connected(void) {
    return g_mqtt_client != NULL && g_mqtt_connected;
}

/* 通过MQTT发送消息 */
int log_mqtt_send_message(const char* topic, const char* message) {
    if (!log_mqtt_is_connected()) {
        // 尝试重连
        if (g_mqtt_config.auto_reconnect) {
            log_mqtt_disconnect();  // 先确保断开旧连接
            if (log_mqtt_connect() != 0) {
                return -1;
            }
        } else {
            return -1;
        }
    }
    
    MQTTMessage mqtt_msg;
    mqtt_msg.qos = QOS1;
    mqtt_msg.retained = 0;
    mqtt_msg.payload = (void*)message;
    mqtt_msg.payloadlen = strlen(message);  // 修正：不包含'\0'
    
    int rc = MQTTPublish(g_mqtt_client, topic, &mqtt_msg);
    if (rc != 0) {
        printf("MQTT publish failed, error code: %d\n", rc);
        g_mqtt_connected = false;  // 发布失败，标记连接断开
    }
    
    return rc;
}

/* 日志函数实现 */
void log_mqtt(const char* format, ...) {
    char log_buffer[512];
    char time_str[20];
    char mqtt_payload[600];
    // char sn[20] = {0}; // 不再需要单独获取SN
    const char* sn = g_mqtt_config.username; // 直接用username作为sn
    uint32_t total_ms = timer_get_ms();
    uint32_t seconds = total_ms / 1000;
    uint32_t milliseconds = total_ms % 1000;
    uint32_t minutes = seconds / 60;
    seconds %= 60;
    uint32_t hours = minutes / 60;
    minutes %= 60;
    // 格式化时间戳 [hh:mm:ss.mmm]
    snprintf(time_str, sizeof(time_str), "[%02u:%02u:%02u.%03u]", hours, minutes, seconds, milliseconds);
    // 获取SN号
    // get_SN(sn); // 注释掉原有的获取SN
    // 格式化日志内容
    va_list args;
    va_start(args, format);
    vsnprintf(log_buffer, sizeof(log_buffer), format, args);
    va_end(args);
    // 拼接最终MQTT日志格式: [时间] SN 日志内容
    snprintf(mqtt_payload, sizeof(mqtt_payload), "%s %s %s", time_str, sn, log_buffer);
    // 打印到控制台
    printf("+++++%s\n", mqtt_payload);
    // 发送到MQTT
    if (log_mqtt_is_connected()) {
        int result = log_mqtt_send_message(g_mqtt_config.topic, mqtt_payload);
        if (result != 0) {
            printf("MQTT log sending failed, error code: %d\n", result);
        }
    }
}

/* 启动MQTT日志系统 */
int log_mqtt_start(void) {
    return log_mqtt_connect();
}

/* 停止MQTT日志系统 */
void log_mqtt_stop(void) {
    log_mqtt_disconnect();
}