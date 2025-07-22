/*
 * log_mqtt.c - 修复MQTT连接问题的版本
 */

#include "log_mqtt.h"
#include <string.h>
#include <netdb.h> // 添加域名解析支持
#include "system/includes.h"  // 杰理芯片系统头文件
#include "../protocol/odm/odm_secure.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "cJSON_common/cJSON.h"
#include "lwip/sockets.h"  // 或你实际用的 socket 头文件
#include "os/os_api.h" // 添加互斥锁支持

/* 全局MQTT客户端指针 */
static Client* g_mqtt_client = NULL;


char log_buffer[512];
char time_str[20];
char mqtt_payload[600];
char sn[25];
char token[256] = {0};

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
    .username = "",
    .password = "",
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

/* 互斥锁 */
static OS_MUTEX g_log_mqtt_mutex;

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

int fetch_mqtt_token(const char* sn, char* out_token, int out_token_size) {
    const char* host = "test.coolqq.com";
    int port = 80;
    const char* api = "/api/jwt/permanentEncrypt";
    
    // 1. 动态构造请求体
    char body[128];
    int body_len = snprintf(body, sizeof(body), "{\"sn\":\"%s\"}", sn);
    if (body_len < 0 || body_len >= sizeof(body)) {
        printf("[fetch_mqtt_token] body construction failed\n");
        return -1;
    }

    // 2. 动态构造请求头
    char header[512];
    int header_len = snprintf(header, sizeof(header),
        "POST %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %d\r\n"
        "Connection: close\r\n\r\n",
        api, host, body_len);
    if (header_len < 0 || header_len >= sizeof(header)) {
        printf("[fetch_mqtt_token] header construction failed\n");
        return -1;
    }

    // 3. 域名解析
    struct hostent *he = gethostbyname(host);
    if (!he) {
        printf("[fetch_mqtt_token] DNS resolution failed\n");
        return -2;
    }
    struct in_addr **addr_list = (struct in_addr **)he->h_addr_list;
    if (addr_list[0] == NULL) {
        printf("[fetch_mqtt_token] no IP address found\n");
        return -2;
    }

    // 4. 创建socket并连接
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        printf("[fetch_mqtt_token] socket create failed\n");
        return -3;
    }

    struct sockaddr_in serv_addr;
    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(port);
    serv_addr.sin_addr = *addr_list[0];

    // 修复点1: 移除多余的右括号
    if (connect(sockfd, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) < 0) {
        printf("[fetch_mqtt_token] connect failed, errno=%d\n", errno);
        close(sockfd);
        return -4;
    }

    // 5. 发送请求
    if (send(sockfd, header, header_len, 0) != header_len ||
        send(sockfd, body, body_len, 0) != body_len) {
        printf("[fetch_mqtt_token] send failed\n");
        close(sockfd);
        return -5;
    }

    // 6. 接收响应
    char recv_buf[4096]; // 必须在 while 前声明
    int total = 0, ret;
    
    // 修复点3: 使用lwip_recv避免宏冲突
    while ((ret = lwip_recv(sockfd, recv_buf + total, sizeof(recv_buf) - 1 - total, 0)) > 0) {
        total += ret;
        if (total >= sizeof(recv_buf) - 1) break;
    }
    close(sockfd);
    recv_buf[total] = '\0';

    // 7. 验证HTTP状态码
    if (strstr(recv_buf, "HTTP/1.1 200 OK") == NULL) {
        printf("[fetch_mqtt_token] non-200 response: %.*s\n", 200, recv_buf);
        return -6;
    }

    // 8. 安全提取JSON主体
    char *body_start = strstr(recv_buf, "\r\n\r\n");
    if (!body_start) {
        printf("[fetch_mqtt_token] malformed HTTP response\n");
        return -7;
    }
    body_start += 4;

    // 9. 解析JSON
    cJSON *root = cJSON_Parse(body_start);
    if (!root) {
        printf("[fetch_mqtt_token] JSON parse error\n");
        return -8;
    }

    // 10. 安全提取token
    cJSON *data = cJSON_GetObjectItem(root, "data");
    cJSON *token = data ? cJSON_GetObjectItem(data, "token") : NULL;
    if (!token || !cJSON_IsString(token)) {
        cJSON_Delete(root);
        printf("[fetch_mqtt_token] invalid token format\n");
        return -9;
    }

    strncpy(out_token, token->valuestring, out_token_size - 1);
    out_token[out_token_size - 1] = '\0';
    cJSON_Delete(root);
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

    // 获取token,也就是password
    printf("SN: %s\n", sn);
    rc = fetch_mqtt_token(sn, token, sizeof(token));

    printf("++++++++++++++++++++ %s +++++++++++++++++++++++++\n",token);
    if (rc != 0) {
        printf("获取MQTT token失败\n");
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
    connect_data.username.cstring = sn;
    
    connect_data.password.cstring = token;
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
        g_mqtt_client = NULL;
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
void log_mqtt_level(LogLevel level, const char* format, ...) {
    os_mutex_pend(&g_log_mqtt_mutex, 0);
    uint32_t total_ms = timer_get_ms();
    uint32_t seconds = total_ms / 1000;
    uint32_t milliseconds = total_ms % 1000;
    uint32_t minutes = seconds / 60;
    seconds %= 60;
    uint32_t hours = minutes / 60;
    minutes %= 60;
    // 格式化时间戳 [hh:mm:ss.mmm]
    snprintf(time_str, sizeof(time_str), "[%02u:%02u:%02u.%03u]", hours, minutes, seconds, milliseconds);
    // 格式化日志内容
    va_list args;
    va_start(args, format);
    vsnprintf(log_buffer, sizeof(log_buffer), format, args);
    va_end(args);
    // 拼接最终MQTT日志格式: [时间] [级别] SN 日志内容
    const char* level_str;
    switch (level) {
        case LOG_DEBUG: level_str = "DEBUG"; break;
        case LOG_INFO:  level_str = "INFO";  break;
        case LOG_WARN:  level_str = "WARN";  break;
        case LOG_ERROR: level_str = "ERROR"; break;
        default:        level_str = "INFO";  break;
    }
    snprintf(mqtt_payload, sizeof(mqtt_payload), "%s [%s] %s %s", time_str, level_str, sn, log_buffer);
    // 打印到控制台
    printf("+++++%s\n", mqtt_payload);
    // 发送到MQTT
    if (log_mqtt_is_connected()) {
        int result = log_mqtt_send_message(g_mqtt_config.topic, mqtt_payload);
        if (result != 0) {
            printf("MQTT log sending failed, error code: %d\n", result);
        }
    }

    os_mutex_post(&g_log_mqtt_mutex);
}

void log_mqtt(const char* format, ...) {
    os_mutex_pend(&g_log_mqtt_mutex, 0);
    uint32_t total_ms = timer_get_ms();
    uint32_t seconds = total_ms / 1000;
    uint32_t milliseconds = total_ms % 1000;
    uint32_t minutes = seconds / 60;
    seconds %= 60;
    uint32_t hours = minutes / 60;
    minutes %= 60;
    // 格式化时间戳 [hh:mm:ss.mmm]
    snprintf(time_str, sizeof(time_str), "[%02u:%02u:%02u.%03u]", hours, minutes, seconds, milliseconds);
    // 格式化日志内容
    va_list args;
    va_start(args, format);
    vsnprintf(log_buffer, sizeof(log_buffer), format, args);
    va_end(args);
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

    os_mutex_post(&g_log_mqtt_mutex);
}

/* 启动MQTT日志系统 */
int log_mqtt_start(void) {
    os_mutex_create(&g_log_mqtt_mutex);
    get_SN(&sn);
    return log_mqtt_connect();
}

/* 停止MQTT日志系统 */
void log_mqtt_stop(void) {
    log_mqtt_disconnect();
    os_mutex_del(&g_log_mqtt_mutex, 1);
}