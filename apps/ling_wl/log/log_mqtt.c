#include "util/cqueue.h"

#include "log_mqtt.h"

#include "system/includes.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <netdb.h> // 添加域名解析支持
#include "cJSON_common/cJSON.h"
#include "../protocol/odm/odm_secure.h"
//#include "../../lib/net/tmallgenie/include/aligenie_os.h"
#include "os/os_api.h" // 添加互斥锁支持
#include "../util/cqueue.h"
#include "system/os/os_api.h"
#include "app_mode/lwl_app_framework.h"

#define LOG_QUEUE_MAX_COUNT 15    // 最大元素个数（原LOG_QUEUE_SIZE）
#define LOG_QUEUE_ITEM_MAX_LEN 120 // 单个元素最大长度（字节）（原LOG_ITEM_MAXLEN）

#define LOG_UPLOAD_INTERVAL 1000  // 1000ms 上传间隔
#define LOG_QUEUE_THRESHOLD 10  // 添加新的阈值定义

char temp[LOG_QUEUE_ITEM_MAX_LEN];
char log_buffer[512];
char time_str[20];
char mqtt_payload[600];
char token[256] = {0};
char sn[25];
char reset_topic[64];
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

//char temp[LOG_ITEM_MAXLEN];
/* 连接状态 */
static bool g_mqtt_connected = false;

/* 互斥锁 */
static OS_MUTEX g_log_mqtt_mutex;

void log_mqtt_disconnect();

static MyBlockingQueue log_queue;
static char log_queue_data[LOG_QUEUE_MAX_COUNT * LOG_QUEUE_ITEM_MAX_LEN];

static pthread_t monitor_thread;  // 声明监控线程
static bool g_monitor_thread_running = false;  // 线程运行标志
static uint32_t last_upload_time = 0;  // 上次上传时间（用于超时判断）

/* 初始化MQTT网络 */
static int init_mqtt_network(void) {
    // 初始化网络
    NewNetwork(&mqtt_network);
    SetNetworkRecvTimeout(&mqtt_network, 2000);  // 2秒超时
    
    // 连接网络
    int rc = ConnectNetwork(&mqtt_network, g_mqtt_config.server, g_mqtt_config.port);
    if (rc != 0) {
        log_i("MQTT network connection failed: %s:%d, error code: %d\n", 
               g_mqtt_config.server, g_mqtt_config.port, rc);
        return rc;
    }
    
    return 0;
}

int fetch_mqtt_token(const char* sn, char* out_token, int out_token_size) {
    const char* host = SERVER_DOMAIN;
    int port = 80;
    const char* api = "/api/jwt/permanentEncrypt";
    
    // 1. 动态构造请求体
    char body[128];
    int body_len = snprintf(body, sizeof(body), "{\"sn\":\"%s\"}", sn);
    if (body_len < 0 || body_len >= sizeof(body)) {
        log_i("[fetch_mqtt_token] body construction failed\n");
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
        log_i("[fetch_mqtt_token] header construction failed\n");
        return -1;
    }

    // 3. 域名解析
    struct hostent *he = gethostbyname(host);
    if (!he) {
        log_i("[fetch_mqtt_token] DNS resolution failed\n");
        return -2;
    }
    struct in_addr **addr_list = (struct in_addr **)he->h_addr_list;
    if (addr_list[0] == NULL) {
        log_i("[fetch_mqtt_token] no IP address found\n");
        return -2;
    }

    // 4. 创建socket并连接
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        log_i("[fetch_mqtt_token] socket create failed\n");
        return -3;
    }

    struct sockaddr_in serv_addr;
    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(port);
    serv_addr.sin_addr = *addr_list[0];

    // 修复点1: 移除多余的右括号
    if (connect(sockfd, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) < 0) {
        log_i("[fetch_mqtt_token] connect failed, errno=%d\n", errno);
        close(sockfd);
        return -4;
    }

    // 5. 发送请求
    if (send(sockfd, header, header_len, 0) != header_len ||
        send(sockfd, body, body_len, 0) != body_len) {
        log_i("[fetch_mqtt_token] send failed\n");
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
        log_i("[fetch_mqtt_token] non-200 response: %.*s\n", 200, recv_buf);
        return -6;
    }

    // 8. 安全提取JSON主体
    char *body_start = strstr(recv_buf, "\r\n\r\n");
    if (!body_start) {
        log_i("[fetch_mqtt_token] malformed HTTP response\n");
        return -7;
    }
    body_start += 4;

    // 9. 解析JSON
    cJSON *root = cJSON_Parse(body_start);
    if (!root) {
        log_i("[fetch_mqtt_token] JSON parse error\n");
        return -8;
    }

    // 10. 安全提取token
    cJSON *data = cJSON_GetObjectItem(root, "data");
    cJSON *token = data ? cJSON_GetObjectItem(data, "token") : NULL;
    if (!token || !cJSON_IsString(token)) {
        cJSON_Delete(root);
        log_i("[fetch_mqtt_token] invalid token format\n");
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
    rc = fetch_mqtt_token(sn, token, sizeof(token));

    if (rc != 0) {
        log_i("获取MQTT token失败\n");
        return rc;
    }

    //Mqtt超时时间设置成2000ms
    // 初始化MQTT客户端
    MQTTClient(&mqtt_client, &mqtt_network, 2000, 
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
        log_i("MQTT connection failed, error code: %d\n", rc);
        log_mqtt_disconnect();  // 连接失败，清理资源
        return rc;
    }
    
    g_mqtt_connected = true;
    return 0;
}

/* 断开MQTT连接 */
void log_mqtt_disconnect(void) {
    if (g_mqtt_client != NULL && g_mqtt_connected) {
        MQTTDisconnect(g_mqtt_client);
        g_mqtt_client = NULL;
        mqtt_network.disconnect(&mqtt_network);
        g_mqtt_connected = false;
        log_i("MQTT connection disconnected\n");
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
        log_i("MQTT publish failed, error code: %d\n", rc);
        //g_mqtt_connected = false;  // 发布失败，标记连接断开
    }
    
    return rc;
}

// 批量上传队列中的所有日志
static void upload_all_logs(void) {
    os_mutex_pend(&g_log_mqtt_mutex, 0);  // 添加锁
    if (log_queue.size == 0) {
        os_mutex_post(&g_log_mqtt_mutex);
        return;
    }

    uint32_t start_time = timer_get_ms();
    
    // 创建JSON数组
    cJSON *array = cJSON_CreateArray();
    if (!array) {
        return;
    }

    while (log_queue.size > 0) {
        if (my_blocking_queue_try_dequeue(&log_queue, temp) == MYERROR_Success) {

            cJSON *item = cJSON_Parse(temp);
            if (item) {
                cJSON_AddItemToArray(array, item);
            }
        }
    }
    
    // 一次性转换成JSON字符串
    char *batch_json = cJSON_PrintUnformatted(array);

    // 一次性发送所有消息
    if (log_mqtt_is_connected() && batch_json) {
        int result = log_mqtt_send_message("test-log", batch_json);
        if (result != 0) {
            log_i("Batch MQTT send failed, error code: %d\n", result);
        }
    }
    
    // 清理资源
    if (batch_json) {
        free(batch_json);
    }
    cJSON_Delete(array);
    
    uint32_t end_time = timer_get_ms();
    log_i("[MQTT upload]Batch send messages took %u ms\n", end_time - start_time);
    os_mutex_post(&g_log_mqtt_mutex);  // 释放锁
}

/* 常驻监控线程：检测队列阈值和超时 */
static void* log_monitor_thread(void* arg) {
    log_i("[MQTT log_monitor]Log monitor thread started\n");
    last_upload_time = timer_get_ms();  // 初始化上传时间

    while (g_monitor_thread_running) {
        uint32_t current_time = timer_get_ms();
        bool need_upload = false;

        // 条件1：队列长度达到阈值
        if (log_queue.size >= LOG_QUEUE_THRESHOLD) {
            need_upload = true;
        }
        // 条件2：未达阈值但超过上传间隔（超时兜底）
        else if (log_queue.size > 0 && 
                 (current_time - last_upload_time) >= LOG_UPLOAD_INTERVAL) {
            need_upload = true;
        }

        if (need_upload) {
            upload_all_logs();  // 触发上传
        }
        // 休眠10ms，避免CPU空转（可根据需求调整）
        os_time_dly(100);
    }
    g_monitor_thread_running = false;

    log_i("[MQTT log_monitor]Log monitor thread exited\n");
    return NULL;
}
// 初始化队列和定时器
void log_mqtt_queue_init(void) {
    my_blocking_queue_init_static(&log_queue, log_queue_data, LOG_QUEUE_ITEM_MAX_LEN, LOG_QUEUE_MAX_COUNT);
    //创建线程
    // 启动监控线程
    if (g_monitor_thread_running) return;
    g_monitor_thread_running = true;
    int ret = pthread_create(&monitor_thread, NULL, log_monitor_thread, NULL);
    if (ret != 0) {
        log_i("Failed to create monitor thread, error: %d\n", ret);
        g_monitor_thread_running = false;
    }
}

// 发送电池状态日志
void log_mqtt_battery(const char* sn_param,int battery) {
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

    // 构造JSON
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "event", "batt");
    cJSON_AddStringToObject(root, "time", time_str);
    cJSON_AddStringToObject(root, "sn", sn_param);
    cJSON_AddNumberToObject(root, "batt", battery); // 直接添加整数

    char *json_str = cJSON_PrintUnformatted(root);

    // 打印到控制台
    log_i("+++++battery json %d\n", battery);

    // 发送到MQTT
    if (log_mqtt_is_connected()) {
        int result = log_mqtt_send_message("h-status", json_str);
        if (result != 0) {
            log_i("MQTT battery log sending failed, error code: %d\n", result);
        }
    }

    cJSON_Delete(root);
    free(json_str);

    os_mutex_post(&g_log_mqtt_mutex);
}

void log_mqtt_enqueue(const char *msg) {
    os_mutex_pend(&g_log_mqtt_mutex, 0);
    my_blocking_queue_enqueue(&log_queue, (void *)msg);
    os_mutex_post(&g_log_mqtt_mutex);
}

/* 日志发送函数 */
void log_mqtt(const char* format, ...) {
    os_mutex_pend(&g_log_mqtt_mutex, 0);
    
    uint32_t total_ms = timer_get_ms();
    uint32_t seconds = total_ms / 1000;
    uint32_t milliseconds = total_ms % 1000;
    uint32_t minutes = seconds / 60;
    seconds %= 60;
    uint32_t hours = minutes / 60;
    minutes %= 60;
    
    snprintf(time_str, sizeof(time_str), "[%02u:%02u:%02u.%03u]", hours, minutes, seconds, milliseconds);
    
    va_list args;
    va_start(args, format);
    vsnprintf(log_buffer, sizeof(log_buffer), format, args);
    va_end(args);

    // 构造JSON
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "time", time_str);
    cJSON_AddStringToObject(root, "sn", sn);
    cJSON_AddStringToObject(root, "msg", log_buffer);

    char *json_str = cJSON_PrintUnformatted(root);
    
    // 打印到控制台
    log_i("[MQTT message] %s", log_buffer);

    // 发送到队列
    log_mqtt_enqueue(json_str);

    // 清理资源
    cJSON_Delete(root);
    free(json_str);

    os_mutex_post(&g_log_mqtt_mutex);
}

// 回调函数实现
void mqtt_reset_callback(MessageData* data)
{
    log_i("+++++++++++++++MQTT reset callback triggered++++++++++++++++++\n");
    //清除wifi配置，并且重启设备
    log_i("收到MQTT reset消息:\n");
    log_i("- topic: %.*s\n", data->topicName->lenstring.len, data->topicName->lenstring.data);
    log_i("- msg: %.*s\n", data->message->payloadlen, (char*)data->message->payload);
    log_i("- QoS: %d\n", data->message->qos);
    
    os_mutex_pend(&g_log_mqtt_mutex, 0);
    
    app_del_stored_wifi_ssid();
    printf("delete all wifi info\r\n");
    lwl_app_mode_switch_to(APP_DEFAULT_MODE);
    os_mutex_post(&g_log_mqtt_mutex);
    return ;
}

/* 订阅主题*/
int mqtt_subscribe_reset() {
    if (!log_mqtt_is_connected()) {
        log_i("MQTT not connected, cannot subscribe\n");
        return -1;
    }

    snprintf(reset_topic, sizeof(reset_topic), "reset-%s", sn);
    int rc = MQTTSubscribe(g_mqtt_client, reset_topic, QOS0, mqtt_reset_callback);

    if (rc != 0) {
        log_i("MQTT subscribe failed, error code: %d\n", rc);
    } else {
        log_i("Subscribed to topic: %s\n", reset_topic);
    }
    return rc;
}


// 在 log_mqtt_start() 里加上队列初始化
int log_mqtt_start(void) {
    os_mutex_create(&g_log_mqtt_mutex);
    get_SN(&sn);
    log_mqtt_queue_init();//初始化定时器和日志队列
    return log_mqtt_connect();
}



// 添加停止函数
void log_mqtt_stop(void) {

    // 停止监控线程
    g_monitor_thread_running = false;
    pthread_join(monitor_thread, NULL);  // 等待线程退出

    // 确保最后的消息都发送出去
    upload_all_logs();
    
    my_blocking_queue_destroy_static(&log_queue);// 释放队列资源

    log_mqtt_disconnect();// 断开MQTT连接

    os_mutex_del(&g_log_mqtt_mutex, 1);//删除互斥锁
}