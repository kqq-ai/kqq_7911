#include "log_mqtt.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>  // 添加可变参数支持
#include <netdb.h> // 添加域名解析支持
#include "cJSON_common/cJSON.h"
#include "../protocol/odm/odm_secure.h"
#include "os/os_api.h" // 添加互斥锁支持
#include "../util/cqueue.h"
#include "system/os/os_api.h"
#include "app_mode/lwl_app_framework.h"
#include "system/sys_time.h"
#include "driver/device/ioctl_cmds.h"
#include "driver/device/device.h"
#include "generic/jiffies.h"

#define LOG_QUEUE_MAX_COUNT 25    // 最大元素个数（原LOG_QUEUE_SIZE）
#define LOG_QUEUE_ITEM_MAX_LEN 512 // 单个元素最大长度（字节）（原LOG_ITEM_MAXLEN）

#define LOG_UPLOAD_INTERVAL 1000  // 1000ms 上传间隔
#define LOG_QUEUE_THRESHOLD 10     // 队列上传阈值

// MQTT 主题
#define MQTT_TOPIC_LOG "h-log"
#define MQTT_TOPIC_STATUS "h-status"
#define MQTT_TOPIC_RESET_PREFIX "reset-"

// 缓冲区大小
#define MQTT_BUF_SIZE 2048

// 配置常量
#define MQTT_RETRY_COUNT_MAX 10
#define MQTT_RETRY_WAIT_FAST 1000  // ms
#define MQTT_RETRY_WAIT_SLOW 5000  // ms
#define MQTT_NETWORK_TIMEOUT 2000  // ms
#define MQTT_MONITOR_SLEEP 150     // ms

char sn[25];  // 设备序列号
char reset_topic[64];  // 重置主题

// 用于 log_mqtt() 函数的全局缓冲区（解决小栈线程调用问题）
static char g_log_mqtt_log_buffer[LOG_QUEUE_ITEM_MAX_LEN];
static char g_log_mqtt_time_str[32];
static char g_log_mqtt_json_buf[LOG_QUEUE_ITEM_MAX_LEN];
static OS_MUTEX g_log_mqtt_buffer_mutex;  // 保护缓冲区访问
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
};

/* MQTT客户端实例 */
static Client mqtt_client;
static Network mqtt_network;
static unsigned char mqtt_send_buf[MQTT_BUF_SIZE];
static unsigned char mqtt_read_buf[MQTT_BUF_SIZE];
static char json_build_buf[MQTT_BUF_SIZE];  // 用于构建JSON数组的独立缓冲区
static char upload_temp[LOG_QUEUE_ITEM_MAX_LEN];  // upload_all_logs 使用的临时缓冲区

/* 连接状态 */
static bool g_mqtt_connected = false;

/* 互斥锁 */
static OS_MUTEX g_log_mqtt_mutex;
static bool g_mqtt_start = false;
static bool g_mqtt_init = false;            // MQTT基础设施(队列/RTC/互斥锁)是否已初始化
static bool g_mqtt_thread_running = false;  // MQTT上传线程是否在运行（防止重复创建）

/* RTC设备句柄（在整个MQTT线程生命周期内保持打开，避免频繁打开/关闭） */
static void *g_rtc_device = NULL;
static OS_MUTEX g_rtc_mutex;  // 保护RTC设备访问（多线程读取需要）

void log_mqtt_disconnect();
void log_mqtt_battery(const char* sn_param,int battery);
static MyBlockingQueue log_queue;
static char log_queue_data[LOG_QUEUE_MAX_COUNT * LOG_QUEUE_ITEM_MAX_LEN];

static int monitor_thread_pid = 0;  // 线程PID

static bool g_monitor_thread_running = false;  // 线程运行标志
static uint32_t last_upload_time = 0;  // 上次上传时间（用于超时判断）

static bool g_battery_update_flag = false;
static int g_current_battery_level = 0;
static char g_sn_battery[25];

/* 缓存的MQTT token（只在初始化时获取一次） */
static char* g_mqtt_token = NULL;

// 获取队列错误消息
static const char* get_queue_error_message(int error_code) {
    switch (error_code) {
        case MYERROR_Null_Pointer: return "空指针错误";
        case MYERROR_Malloc_Failed: return "内存分配失败";
        case MYERROR_OS_Api_Error: return "操作系统API错误";
        case MYERROR_Is_Full: return "队列已满";
        case MYERROR_Is_Empty: return "队列为空（异常状态）";
        case MYERROR_Not_Found: return "未找到（异常状态）";
        default: return "未知错误";
    }
}

/* 初始化MQTT网络 */
static int init_mqtt_network(void) {
    int rc = 0;
    
    // 先清理前一次的网络资源，确保不会泄漏TCP_PCB
    if (mqtt_network.my_socket != NULL) {
        mqtt_network.disconnect(&mqtt_network);
        mqtt_network.my_socket = NULL;
    }
    
    // 初始化网络
    NewNetwork(&mqtt_network);
    SetNetworkRecvTimeout(&mqtt_network, MQTT_NETWORK_TIMEOUT);
    
    // 连接网络
    rc = ConnectNetwork(&mqtt_network, g_mqtt_config.server, g_mqtt_config.port);
    if (rc != 0) {
        log_i("[ERROR][MQTT]❌❌❌ 网络连接端口失败: %s:%d, error code: %d\n", 
               g_mqtt_config.server, g_mqtt_config.port, rc);
        goto cleanup;
    }
    
    return 0;
    
cleanup:
    log_mqtt_disconnect();
    return rc;
}
/* 检查MQTT连接状态 */
bool log_mqtt_is_connected(void) {
    return (g_mqtt_client != NULL && g_mqtt_connected);
}

/* 初始化MQTT客户端和连接参数 */
static int init_mqtt_client(const char* token) {
    if (token == NULL) {
        log_i("[ERROR][MQTT]token为空，无法连接MQTT\n");
        return -1;
    }
    // 初始化MQTT客户端，超时时间设置成2000ms
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
    int rc = MQTTConnect(g_mqtt_client, &connect_data);
    if (rc != 0) {
        log_i("[ERROR][MQTT]❌❌❌ 连接失败, error code: %d\n", rc);
        return rc;
    }
    
    return 0;
}

/* 连接MQTT服务器 */
int log_mqtt_connect(void) {
    if (!g_mqtt_init) {
        log_i("[ERROR][MQTT]基础设施未初始化，无法连接\n");
        return -1;
    }
    if (g_mqtt_token == NULL) {
        log_i("[ERROR][MQTT]token未就绪，无法连接\n");
        return -1;
    }
    // 先断开可能存在的旧连接
    log_mqtt_disconnect();
    // 初始化网络
    int rc = init_mqtt_network();
    if (rc != 0) {
        // init_mqtt_network内部已经处理了资源释放，这里直接返回
        return rc;
    }   
    // 使用缓存的token连接（不再每次获取）
    rc = init_mqtt_client(g_mqtt_token);
    if (rc != 0) { 
        log_i("[ERROR][MQTT]❌❌❌ 初始化MQTT客户端并连接失败\n");
        // 统一使用log_mqtt_disconnect释放资源（包括已建立的TCP连接）
        log_mqtt_disconnect();
        return rc;
    }
    log_i("[MQTT]初始化MQTT客户端并连接成功,rc: %d\n", rc);
    g_mqtt_connected = true;
    return 0;
}

/* 断开MQTT连接 */
void log_mqtt_disconnect(void) {
    // 断开MQTT客户端连接（如果已建立）
    if (g_mqtt_client != NULL) {
        MQTTDisconnect(g_mqtt_client);
        g_mqtt_client = NULL;
    }
    
    // 释放网络连接资源（TCP socket等），无论MQTT客户端是否存在
    // 这很重要：因为可能在网络连接建立后，MQTT连接失败，此时需要释放TCP资源
    if (mqtt_network.my_socket != NULL) {
        mqtt_network.disconnect(&mqtt_network);
    }
    
    // 重置Network结构体，确保所有状态都被清理
    NewNetwork(&mqtt_network);
    
    // 重置连接状态标志
    g_mqtt_connected = false;
    
    log_i("[MQTT] disconnected\n");
}

/* 通过MQTT发送消息 */
int log_mqtt_send_message(const char* topic, const char* message) {
    MQTTMessage mqtt_msg;
    mqtt_msg.qos = QOS1;
    mqtt_msg.retained = 0;
    mqtt_msg.payload = (void*)message;
    mqtt_msg.payloadlen = strlen(message);  // 修正：不包含'\0'
    if (!log_mqtt_is_connected()) {
        //重连并且发送
        int ret = log_mqtt_connect();
        if (ret != 0) {
            log_i("[ERROR][MQTT]重连失败(发送阶段)\n");
            return -1;
        }
    }
    int rc = MQTTPublish(g_mqtt_client, topic, &mqtt_msg);
    if (rc != 0) {
        g_mqtt_connected = false;  // 发布失败，标记连接断开
    }
    
    return rc;
}

// 获取与log_i相同格式的系统时间戳（优化版本：使用已打开的RTC设备）
static int get_system_timestamp(char *time_str, int str_len) {
    struct sys_time t;
    int ret;
    
    // 如果设备已打开，使用已打开的设备（性能优化，避免频繁打开/关闭）
    if (g_rtc_device != NULL) {
        // 使用已打开的设备，需要加锁保护（多线程安全）
        os_mutex_pend(&g_rtc_mutex, 0);
        ret = dev_ioctl(g_rtc_device, IOCTL_GET_SYS_TIME, (u32)&t);
        os_mutex_post(&g_rtc_mutex);
        
        if (ret != 0) {
            return -1;  // 设备操作失败
        }
    } else {
        // 设备未打开，回退到原始方法（兼容性，避免启动前调用失败）
        void *dev = dev_open("rtc", NULL);
        if (!dev) {
            return -1;  // 无法打开RTC设备
        }
        
        ret = dev_ioctl(dev, IOCTL_GET_SYS_TIME, (u32)&t);
        dev_close(dev);
        
        if (ret != 0) {
            return -1;  // 获取时间失败
        }
    }
    
    // 获取毫秒部分（使用更高精度的jiffies_half_msec）
    // jiffies_half_msec返回半毫秒单位，除以2得到毫秒，取模1000得到0-999的毫秒数
    uint32_t half_ms = jiffies_half_msec();
    uint32_t milliseconds = (half_ms / 2) % 1000;
    
    // 格式化为与log_i相同的时间戳格式 [HH:MM:SS.mmm]
    // 使用RTC的时分秒 + 运行时间的毫秒部分
    snprintf(time_str, str_len, "[%02u:%02u:%02u.%03u]", t.hour, t.min, t.sec, milliseconds);
    return 0;
}

// 发送消息，失败时自动重试一次
static int send_with_retry(const char* topic, const char* message, const char* log_prefix, uint32_t start_time) {
    int result = log_mqtt_send_message(topic, message);
    if (result == 0) {
        uint32_t end_time = timer_get_ms();
        log_i("[PERF][MQTT]%s成功 - %d字节, 耗时 %u ms\n", log_prefix, (int)strlen(message), end_time - start_time);
        return 0;
    }
    
    // 重试一次
    result = log_mqtt_send_message(topic, message);
    if (result != 0) {
        log_i("[ERROR][MQTT]%s失败(重试后), error code: %d\n", log_prefix, result);
    } else {
        uint32_t end_time = timer_get_ms();
        log_i("[PERF][MQTT]%s成功(重试) - %d字节, 耗时 %u ms\n", log_prefix, (int)strlen(message), end_time - start_time);
    }
    return result;
}

// 批量上传队列中的所有日志
static void upload_all_logs(void) {
    os_mutex_pend(&g_log_mqtt_mutex, 0);
    
    if (log_queue.size == 0) {
        os_mutex_post(&g_log_mqtt_mutex);
        return;
    }
    
    uint32_t start_time = timer_get_ms();
    // 计算最大长度，留出20字节空间用于封装
    const int MAX_SIZE = sizeof(mqtt_send_buf) - 20;
    int count = 0;
    
    // 使用独立缓冲区构建JSON数组
    char *ptr = json_build_buf;
    int current_len = 0;
    
    // 写入开始的 '['
    current_len = snprintf(ptr, sizeof(json_build_buf), "[");
    ptr += current_len;
    
    // 遍历队列中的所有日志
    while (log_queue.size > 0 && my_blocking_queue_try_dequeue(&log_queue, upload_temp) == MYERROR_Success) {
        int item_len = strlen(upload_temp);
        
        // 计算添加这条后的总长度: 当前长度 + 逗号(如果不是第一条) + 日志内容 + ']'
        int need_len = current_len + (count > 0 ? 1 : 0) + item_len + 1;
        
        // 判断累积是否超限
        if (need_len > MAX_SIZE) {
            // 封装当前缓冲区
            snprintf(ptr, sizeof(json_build_buf) - current_len, "]");
            
            // 把当前取出的放回队列头
            if (my_blocking_queue_push_front(&log_queue, upload_temp) != MYERROR_Success) {
                log_i("[ERROR][MQTT]放回队列失败，日志丢失: %s\n", upload_temp);
            }
            // 发送当前批次
            send_with_retry(MQTT_TOPIC_LOG, json_build_buf, "批量发送完成（超限）", start_time);
            os_mutex_post(&g_log_mqtt_mutex);
            return;
        }
        
        // 未超限，追加到缓冲区
        int len = snprintf(ptr, sizeof(json_build_buf) - current_len, "%s%s", 
                          count > 0 ? "," : "", upload_temp);
        ptr += len;
        current_len += len;
        count++;
    }
    
    // 收尾处理
    if (count > 0) {
        // 封装并发送
        snprintf(ptr, sizeof(json_build_buf) - current_len, "]");
        send_with_retry(MQTT_TOPIC_LOG, json_build_buf, "批量发送完成", start_time);
    } else {
        log_i("[MQTT]无数据发送\n");
    }
    
    os_mutex_post(&g_log_mqtt_mutex);
}


// 常驻上传线程：检测队列阈值和超时，并上传日志
static void log_monitor_thread(void* arg) {
    // 启动MQTT日志上传线程
    log_i("[MQTT]日志上传线程启动\n");

    g_monitor_thread_running = true;

    // 初次连接（token 已在 log_mqtt_start() 获取并缓存）
    int retry_count = 0;
    while (g_monitor_thread_running && !log_mqtt_is_connected()) {
        int ret = log_mqtt_connect();
        if (ret == 0) {
            mqtt_subscribe_reset();
            retry_count = 0;
            break;
        }

        retry_count++;
        uint32_t wait_time = (retry_count <= MQTT_RETRY_COUNT_MAX) ? MQTT_RETRY_WAIT_FAST : MQTT_RETRY_WAIT_SLOW;
        log_i("[ERROR][MQTT]连接失败，%u ms后重试 (第 %d 次), error=%d\n", wait_time, retry_count, ret);
        msleep(wait_time);
    }

    last_upload_time = timer_get_ms();  // 初始化上传时间
    while (g_monitor_thread_running) {
        // 检查连接状态，断线则重连
        if (!log_mqtt_is_connected()) {
            int ret = log_mqtt_connect();
            if (ret == 0) {
                mqtt_subscribe_reset();
            } else {
                msleep(MQTT_RETRY_WAIT_FAST);
                continue;
            }
        }
        
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
            last_upload_time = current_time;  // 更新上传时间
        }
        
        // 休眠100ms，避免CPU空转（可根据需求调整）
        msleep(MQTT_MONITOR_SLEEP);
    }
    
    // 线程退出：只断开连接，不销毁队列/不关闭RTC（基础设施常驻）
    log_mqtt_disconnect();
    g_mqtt_thread_running = false;
    g_mqtt_start = false;
    
    log_i("[MQTT]上传线程退出 Log monitor thread exited\n");
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
    os_mutex_pend(&g_log_mqtt_mutex, 0);
    
    // 使用局部变量，避免使用全局 time_str
    char local_time_str[32];
    get_system_timestamp(local_time_str, sizeof(local_time_str));

    // 构造JSON
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "event", "batt");
    cJSON_AddStringToObject(root, "time", local_time_str);
    cJSON_AddStringToObject(root, "sn", sn_param);
    cJSON_AddNumberToObject(root, "batt", battery);

    char *json_str = cJSON_PrintUnformatted(root);

    // 打印到控制台
    log_i("[MQTT]battery: %s %d\n", sn_param, battery);

    // 发送到MQTT
    if (json_str != NULL) {
        send_with_retry(MQTT_TOPIC_STATUS, json_str, "电池状态日志发送", timer_get_ms());
    }

    cJSON_Delete(root);
    if (json_str != NULL) {
        free(json_str);
    }

    os_mutex_post(&g_log_mqtt_mutex);
}

/**
 * @brief 发送MQTT消息
 * @param format 格式化字符串
 * @note 注意，使用该函数不可以在字符串中包含\n换行符
 */
void log_mqtt(const char* format, ...) {
    // 检查基础设施是否已经初始化（队列/互斥锁/RTC等）
    if (!g_mqtt_init) {
        log_i("[ERROR][MQTT]基础设施未初始化，无法入队\n");
        return;
    }

    // 使用全局缓冲区，解决小栈线程（512字节）调用时的栈溢出问题
    __builtin_va_list args;
    va_start(args, format);

    os_mutex_pend(&g_log_mqtt_buffer_mutex, 0);
    int msg_len = vsnprintf(g_log_mqtt_log_buffer, sizeof(g_log_mqtt_log_buffer), format, args);
    va_end(args);

    // 打印到控制台（锁内）
    log_i("[MQTT]message: %s", g_log_mqtt_log_buffer);

    // 获取时间戳（锁内）
    get_system_timestamp(g_log_mqtt_time_str, sizeof(g_log_mqtt_time_str));

    // 构建JSON字符串（锁内）
    int json_len = snprintf(g_log_mqtt_json_buf, sizeof(g_log_mqtt_json_buf),
                            "{\"time\":\"%s\",\"sn\":\"%s\",\"msg\":\"%s\"}",
                            g_log_mqtt_time_str, sn, g_log_mqtt_log_buffer);

    os_mutex_post(&g_log_mqtt_buffer_mutex);

    // 检查JSON构建是否成功
    if (json_len < 0 || json_len >= sizeof(g_log_mqtt_json_buf)) {
        log_i("[ERROR][MQTT]JSON构建失败或截断（长度:%d）\n", json_len);
        return;  // JSON构建失败，无法入队
    }

    // 入队操作：队列内部已经有锁保护（my_blocking_queue_try_enqueue内部有pthread_mutex_lock）
    int ret = my_blocking_queue_try_enqueue(&log_queue, g_log_mqtt_json_buf);
    if (ret != MYERROR_Success) {
        log_i("[ERROR][MQTT]❌❌❌ 日志入队失败：%s (错误码:%d)\n", 
               get_queue_error_message(ret), ret);
    }
}
//实现一个同步连接mqtt并发送一条信息的函数
int log_mqtt_send_message_sync(const char* topic, const int progress) {
    // 获取时间戳和构建消息在锁外进行，减少锁持有时间
    char time_str[32];
    if (get_system_timestamp(time_str, sizeof(time_str)) != 0) {
        log_i("[ERROR][MQTT]ota获取时间戳失败\n");
        return -1;
    }

    // 简化消息格式，去除特殊字符
    char message[128];
    int msg_len = snprintf(message, sizeof(message),
                          "{\"time\":\"%s\",\"sn\":\"%s\",\"msg\":\"OTA更新进度：%d /100\"}",
                          time_str, sn, progress);

    if (msg_len < 0 || msg_len >= sizeof(message)) {
        log_i("[ERROR][MQTT]ota JSON构建失败或截断（长度:%d）\n", msg_len);
        return -1;
    }

    log_i("[MQTT]ota开始同步发送进度: %d\n", progress);

    // 每次未连接时都尝试重连
    if (!log_mqtt_is_connected()) {
        log_i("[MQTT]ota未连接，尝试重连\n");
        os_mutex_pend(&g_log_mqtt_mutex, 0);
        int ret = log_mqtt_connect();
        os_mutex_post(&g_log_mqtt_mutex);

        if (ret != 0) {
            log_i("[ERROR][MQTT]ota重连失败\n");
            return -1;
        }
        log_i("[MQTT]ota重连成功，继续发送\n");
    }

    int rc = -1;
    os_mutex_pend(&g_log_mqtt_mutex, 0);

    MQTTMessage mqtt_msg;
    mqtt_msg.qos = QOS0; // 降低QoS级别，提高发送成功率
    mqtt_msg.retained = 0;
    mqtt_msg.payload = (void*)message;
    mqtt_msg.payloadlen = strlen(message);

    rc = MQTTPublish(g_mqtt_client, topic, &mqtt_msg);
    if (rc != 0) {
        log_i("[ERROR][MQTT]ota发送进度失败, error code: %d\n", rc);
        g_mqtt_connected = false; // 标记连接断开
    } else {
        log_i("[MQTT]ota发送进度成功\n");
    }

    os_mutex_post(&g_log_mqtt_mutex);
    return rc;
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
        log_i("[ERROR][MQTT]MQTT not connected, cannot subscribe\n");
        return -1;
    }

    snprintf(reset_topic, sizeof(reset_topic), MQTT_TOPIC_RESET_PREFIX "%s", sn);
    int rc = MQTTSubscribe(g_mqtt_client, reset_topic, QOS0, mqtt_reset_callback);

    if (rc != 0) {
        log_i("[ERROR][MQTT]MQTT subscribe failed, error code: %d\n", rc);
    } else {
        log_i("Subscribed to topic: %s\n", reset_topic);
    }
    return rc;
}

/**
 * @brief 初始化 MQTT 基础设施（队列、RTC、互斥锁）
 * @note 在设备启动时调用一次，与网络状态无关
 */
int log_mqtt_init(void) {
    if (g_mqtt_init) {
        return 0;
    }

    if (!os_mutex_valid(&g_log_mqtt_mutex)) {
        os_mutex_create(&g_log_mqtt_mutex);
    }
    if (!os_mutex_valid(&g_log_mqtt_buffer_mutex)) {
        os_mutex_create(&g_log_mqtt_buffer_mutex);
    }
    if (!os_mutex_valid(&g_rtc_mutex)) {
        os_mutex_create(&g_rtc_mutex);
    }

    // 打开RTC设备（常驻保持打开；失败时允许回退逻辑继续工作）
    if (g_rtc_device == NULL) {
        g_rtc_device = dev_open("rtc", NULL);
        if (g_rtc_device == NULL) {
            log_i("[ERROR][MQTT]⚠️ RTC设备打开失败，时间戳将使用回退方法\n");
        } else {
            log_i("[MQTT]✓ RTC设备已打开（常驻）\n");
        }
    }

    // 获取SN（用于连接参数/日志字段）
    get_SN(sn);

    // 初始化队列用于缓存MQTT日志消息（常驻）
    my_blocking_queue_init_static(&log_queue, log_queue_data, LOG_QUEUE_ITEM_MAX_LEN, LOG_QUEUE_MAX_COUNT);

    g_mqtt_init = true;
    log_i("[MQTT]✓ 基础设施初始化完成\n");
    return 0;
}

int log_mqtt_start(void) {
    if (!g_mqtt_init) {
        log_i("[ERROR][MQTT]基础设施未初始化，请先调用log_mqtt_init()\n");
        return -1;
    }

    if (g_mqtt_thread_running || monitor_thread_pid) {
        log_i("[MQTT]线程已在运行，跳过\n");
        return 0;
    }

    // 获取token（首次获取，后续复用）
    if (g_mqtt_token == NULL) {
        log_i("[MQTT]开始获取token...\n");
        g_mqtt_token = get_mqtt_token();
        if (g_mqtt_token == NULL) {
            log_i("[ERROR][MQTT]获取token失败\n");
            return -1;
        }
        log_i("[MQTT]✓ Token获取成功，已缓存\n");
    }

    // 设置启动标志
    g_mqtt_start = true;
    // 启动线程用于监控队列阈值和超时
    int ret = thread_fork("log_mqtt_monitor", 15, 2048 * 4, 0, &monitor_thread_pid, log_monitor_thread, NULL);
    if (ret != 0) {
        log_i("[ERROR][MQTT]❌❌❌ Failed to create monitor thread, error: %d\n", ret);
        g_mqtt_start = false;
        return ret;
    }
    g_mqtt_thread_running = true;
    return 0;
}

// 添加停止函数
void log_mqtt_stop(void) {
    if (!g_mqtt_thread_running && !monitor_thread_pid) {
        log_i("[MQTT]线程未运行，跳过\n");
        return;
    }
    g_mqtt_start = false;
    // 停止监控线程
    g_monitor_thread_running = false;
    
    log_i("MQTT pthread_join %d\n", monitor_thread_pid);
    if (monitor_thread_pid) {
        thread_kill(&monitor_thread_pid, KILL_WAIT);  // 等待线程退出
        log_i("MQTT monitor thread killed\n");
        monitor_thread_pid = 0;
    }
    g_mqtt_thread_running = false;

    // 停止网络侧：断开连接；基础设施(队列/RTC/token)保持
    log_mqtt_disconnect();
    // 确保最后的消息都发送出去
    // upload_all_logs();
}