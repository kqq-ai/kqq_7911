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
#define LOG_QUEUE_ITEM_MAX_LEN 256 // 单个元素最大长度（字节）（原LOG_ITEM_MAXLEN）

#define LOG_UPLOAD_INTERVAL 1000  // 1000ms 上传间隔
#define LOG_QUEUE_THRESHOLD 10  // 添加新的阈值定义

char temp[LOG_QUEUE_ITEM_MAX_LEN];
char log_buffer[LOG_QUEUE_ITEM_MAX_LEN];
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
static bool g_mqtt_start = false;

void log_mqtt_disconnect();
void log_mqtt_battery(const char* sn_param,int battery);
static MyBlockingQueue log_queue;
static char log_queue_data[LOG_QUEUE_MAX_COUNT * LOG_QUEUE_ITEM_MAX_LEN];

static pthread_t monitor_thread;  // 声明监控线程
pthread_attr_t attr;
size_t stack_size = 1024 * 8;  // 设置栈大小为8KB

static bool g_monitor_thread_running = false;  // 线程运行标志
static uint32_t last_upload_time = 0;  // 上次上传时间（用于超时判断）

static bool g_battery_update_flag = false;
static int g_current_battery_level = 0;
static char g_sn_battery[25];

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

/* 连接MQTT服务器 */
int log_mqtt_connect(void) {
    os_mutex_pend(&g_log_mqtt_mutex, 0);
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
        os_mutex_post(&g_log_mqtt_mutex);
        return rc;
    }

    // 获取token,也就是password
    char* token = get_mqtt_token();

    if (token == NULL) {
        log_i("获取MQTT token失败\n");
        os_mutex_post(&g_log_mqtt_mutex);
        return -1;
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
        os_mutex_post(&g_log_mqtt_mutex);
        return rc;
    }
    
    g_mqtt_connected = true;
    os_mutex_post(&g_log_mqtt_mutex);
    return 0;
}

/* 检查MQTT连接状态 */
bool log_mqtt_is_connected(void) {
    if (!os_mutex_valid(&g_log_mqtt_mutex) || !g_mqtt_connected) return false;
    os_mutex_pend(&g_log_mqtt_mutex, 0);
    bool ret = (g_mqtt_client != NULL && g_mqtt_connected);
    os_mutex_post(&g_log_mqtt_mutex);
    return ret;
}

/* 断开MQTT连接 */
void log_mqtt_disconnect(void) {
    os_mutex_pend(&g_log_mqtt_mutex, 0);
    if (g_mqtt_client!=NULL) {
        MQTTDisconnect(g_mqtt_client);
        g_mqtt_client = NULL;
        mqtt_network.disconnect(&mqtt_network);
        g_mqtt_connected = false;
        log_i("MQTT connection disconnected\n");
    }
    os_mutex_post(&g_log_mqtt_mutex);
}

/* 通过MQTT发送消息 */
int log_mqtt_send_message(const char* topic, const char* message) {
    os_mutex_pend(&g_log_mqtt_mutex, 0);  // 添加锁
    if (!log_mqtt_is_connected()) {
        // 尝试重连
        if (g_mqtt_config.auto_reconnect) {
            log_mqtt_connect();
        }
    }
    if (!log_mqtt_is_connected()) {
        os_mutex_post(&g_log_mqtt_mutex);  // 释放锁
        return -1;
    }
    
    MQTTMessage mqtt_msg;
    mqtt_msg.qos = QOS1;
    mqtt_msg.retained = 0;
    mqtt_msg.payload = (void*)message;
    mqtt_msg.payloadlen = strlen(message);  // 修正：不包含'\0'

    int rc = MQTTPublish(g_mqtt_client, topic, &mqtt_msg);
    if (rc != 0) {
        log_i("MQTT publish failed, error code: %d\n", rc);
        g_mqtt_connected = false;  // 发布失败，标记连接断开
    }
    
    os_mutex_post(&g_log_mqtt_mutex);  // 释放锁
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
    int size = log_queue.size;
    while (log_queue.size > 0) {
        if (my_blocking_queue_try_dequeue(&log_queue, temp) == MYERROR_Success) {

            cJSON *item = cJSON_Parse(temp);
            if (item) {
                cJSON_AddItemToArray(array, item);
            } else {
                log_i("Failed to parse log item to JSON: %s\n", temp);
            }
        }
    }
    
    // 一次性转换成JSON字符串
    char *batch_json = cJSON_PrintUnformatted(array);
    if (batch_json != NULL) {
        int result = log_mqtt_send_message("h-log", batch_json);
        if (result != 0) {
            log_i("MQTT message log sending failed, error code: %d\n", result);
        }
    }

    // 清理资源
    if (batch_json) {
        free(batch_json);
    }
    cJSON_Delete(array);
    
    uint32_t end_time = timer_get_ms();
    log_i("[MQTT upload]Batch send messages took %u ms %d\n", end_time - start_time, size);
    os_mutex_post(&g_log_mqtt_mutex);  // 释放锁
}


/* 常驻监控线程：检测队列阈值和超时 */
static void* log_monitor_thread(void* arg) {
    log_i("[MQTT log_monitor]Log monitor thread started\n");
    
    if(!log_mqtt_is_connected()) {
        int ret=log_mqtt_connect(); // 内部调用log_mqtt_connect
        if (ret==0) {
            log_i("MQTT connected successfully\n");
            ret = mqtt_subscribe_reset();
            g_monitor_thread_running = true;
        }
    }

    last_upload_time = timer_get_ms();  // 初始化上传时间
    while (g_monitor_thread_running) {
        os_mutex_pend(&g_log_mqtt_mutex, 0);
        uint32_t current_time = timer_get_ms();
        bool need_upload = false;

        // 检查是否需要上传电池状态
        if (g_battery_update_flag) {
            log_mqtt_battery(g_sn_battery, g_current_battery_level);
            g_battery_update_flag = false;
        }

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
        os_mutex_post(&g_log_mqtt_mutex);
        // 休眠100ms，避免CPU空转（可根据需求调整）
        os_time_dly(100);
    }
    
    log_i("[MQTT log_monitor]Log monitor thread exited\n");
    return NULL;
}

// 设置需要更新电量上传消息的SN和电量参数，SN为NULL时使用默认SN,即C的SN，电量范围0-100
void log_mqtt_set_battery_level(char *sn_battery, int battery_level) {
    if (!log_mqtt_is_connected()) return;
    os_mutex_pend(&g_log_mqtt_mutex, 0);
    g_current_battery_level = battery_level;
    g_battery_update_flag = true;
    if (sn_battery == NULL) {
        strncpy(g_sn_battery, sn, sizeof(g_sn_battery) - 1);
    } else {
        strncpy(g_sn_battery, sn_battery, sizeof(g_sn_battery) - 1);
    }
    os_mutex_post(&g_log_mqtt_mutex);
}

// 发送电池状态日志
void log_mqtt_battery(const char* sn_param,int battery) {
    if (!log_mqtt_is_connected()) return;
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
    log_i("[MQTT battery]%s %d\n", sn_param, battery);

    // 发送到MQTT
    if (json_str != NULL) {
        int result = log_mqtt_send_message("h-status", json_str);
        ///////////int result = log_mqtt_send_message("reset-KCCNC5L05BEEEH1", json_str);
        if (result != 0) {
            log_i("MQTT battery log sending failed, error code: %d\n", result);
        }
        else{
            ///////////log_i("MQTT battery log GOOD. or  RESET   RESET   RESET   RESET   RESET   \n" );
        }
    }

    cJSON_Delete(root);
    free(json_str);

    os_mutex_post(&g_log_mqtt_mutex);
}


void log_mqtt_enqueue(const char *msg) {
    // os_mutex_pend(&g_log_mqtt_mutex, 0);
    my_blocking_queue_try_enqueue(&log_queue, (void *)msg);
    // os_mutex_post(&g_log_mqtt_mutex);
}

/* 日志发送函数 */
void log_mqtt(const char* format, ...) {
    va_list args;
    va_start(args, format);
    vsnprintf(log_buffer, sizeof(log_buffer), format, args);
    va_end(args);
    
    // 打印到控制台
    log_i("[MQTT message] %s", log_buffer);

    if (!log_mqtt_is_connected()) return;

    os_mutex_pend(&g_log_mqtt_mutex, 0);
    
    uint32_t total_ms = timer_get_ms();
    uint32_t seconds = total_ms / 1000;
    uint32_t milliseconds = total_ms % 1000;
    uint32_t minutes = seconds / 60;
    seconds %= 60;
    uint32_t hours = minutes / 60;
    minutes %= 60;
    
    snprintf(time_str, sizeof(time_str), "[%02u:%02u:%02u.%03u]", hours, minutes, seconds, milliseconds);


    // 构造JSON
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "time", time_str);
    cJSON_AddStringToObject(root, "sn", sn);
    cJSON_AddStringToObject(root, "msg", log_buffer);

    char *json_str = cJSON_PrintUnformatted(root);

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
    
    app_del_stored_wifi_ssid();/////////////
    printf("delete all   delete all   delete all  wifi info\r\n");
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

int log_mqtt_start(void) {
    if (!os_mutex_valid(&g_log_mqtt_mutex)) {
        os_mutex_create(&g_log_mqtt_mutex);
        get_SN(sn);
    }
    os_mutex_pend(&g_log_mqtt_mutex, 0);
    if (g_monitor_thread_running) {
        os_mutex_post(&g_log_mqtt_mutex);
        return 0;
    }
    if (g_mqtt_start) log_mqtt_stop();
    g_mqtt_start = true;

    my_blocking_queue_init_static(&log_queue, log_queue_data, LOG_QUEUE_ITEM_MAX_LEN, LOG_QUEUE_MAX_COUNT);
    //创建线程
    // 启动监控线程

    // 初始化线程属性并设置栈大小
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, stack_size);
    
    int ret = pthread_create(&monitor_thread, &attr, log_monitor_thread, NULL);
    if (ret != 0) {
        log_i("Failed to create monitor thread, error: %d\n", ret);
    }
    os_mutex_post(&g_log_mqtt_mutex);
    return 0;
}

// 添加停止函数
void log_mqtt_stop(void) {
    if (!os_mutex_valid(&g_log_mqtt_mutex)) {
        return;
    }
    os_mutex_pend(&g_log_mqtt_mutex, 0);
    if (!g_mqtt_start) {
        os_mutex_post(&g_log_mqtt_mutex);
        return;
    }
    // 停止监控线程
    g_monitor_thread_running = false;
    
    log_i("MQTT pthread_join\n");
    os_mutex_post(&g_log_mqtt_mutex);
    if (monitor_thread) {
        pthread_join(monitor_thread, NULL);  // 等待线程退出
        pthread_attr_destroy(&attr);  // 清理线程属性
        monitor_thread = 0;
    }
    os_mutex_pend(&g_log_mqtt_mutex, 0);

    // 确保最后的消息都发送出去
    upload_all_logs();
    
    my_blocking_queue_destroy_static(&log_queue);// 释放队列资源

    log_mqtt_disconnect();// 断开MQTT连接

    g_mqtt_start = false;
    os_mutex_post(&g_log_mqtt_mutex);
}