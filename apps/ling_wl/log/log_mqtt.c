#include "util/cqueue.h"

#include "log_mqtt.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>  // 添加可变参数支持
#include <netdb.h> // 添加域名解析支持
#include "cJSON_common/cJSON.h"
#include "../protocol/odm/odm_secure.h"
//#include "../../lib/net/tmallgenie/include/aligenie_os.h"
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
#define LOG_QUEUE_THRESHOLD 10  // 添加新的阈值定义

char temp[LOG_QUEUE_ITEM_MAX_LEN];
// log_buffer 改为在 log_mqtt 函数内使用局部变量，避免多线程竞态
char time_str[32];  // 增加缓冲区大小，确保能容纳完整的时间戳格式 [HH:MM:SS.mmm]（仅在 log_mqtt_battery 中使用）
char mqtt_payload[600];
char token[256] = {0};
char sn[25];
char reset_topic[64];
// json_item_buf 改为在 log_mqtt 函数内使用局部变量，避免多线程竞态
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
static unsigned char mqtt_send_buf[2048];
static unsigned char mqtt_read_buf[2048];
static char json_build_buf[2048];  // 用于构建JSON数组的独立缓冲区

//char temp[LOG_ITEM_MAXLEN];
/* 连接状态 */
static bool g_mqtt_connected = false;

/* 互斥锁 */
static OS_MUTEX g_log_mqtt_mutex;
static bool g_mqtt_start = false;

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

/* 初始化MQTT网络 */
static int init_mqtt_network(void) {
    // 初始化网络
    NewNetwork(&mqtt_network);
    SetNetworkRecvTimeout(&mqtt_network, 2000);  // 2秒超时
    
    // 连接网络
    int rc = ConnectNetwork(&mqtt_network, g_mqtt_config.server, g_mqtt_config.port);
    if (rc != 0) {
        //表示在网络连接端口层面就失败了
        log_i("【MQTT】❌❌❌ 网络连接端口失败: %s:%d, error code: %d\n", 
               g_mqtt_config.server, g_mqtt_config.port, rc);
        
        // 重要：连接失败时，需要断开网络连接，释放socket资源
        // ConnectNetwork失败时可能已经创建了socket，需要清理
        if (mqtt_network.my_socket != NULL) {
            mqtt_network.disconnect(&mqtt_network);
        }
        
        return rc;
    }
    
    return rc;
}
/* 检查MQTT连接状态 */
bool log_mqtt_is_connected(void) {
    return (g_mqtt_client != NULL && g_mqtt_connected);
}

/* 初始化MQTT客户端和连接参数 */
static int init_mqtt_client(const char* token) {
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
        log_i("【MQTT】❌❌❌ 连接失败, error code: %d\n", rc);
        return rc;
    }
    
    return 0;
}

/* 连接MQTT服务器 */
int log_mqtt_connect(void) {
    // 先断开可能存在的旧连接
    log_mqtt_disconnect();
    // 初始化网络
    int rc = init_mqtt_network();
    if (rc != 0) {
        return rc;
    }
    // 获取token,也就是password
    char* token = get_mqtt_token();
    if (token == NULL) {
        log_i("【MQTT】❌❌❌ 获取MQTT token失败\n");
        return -1;
    }
    // 初始化MQTT客户端并连接
    rc = init_mqtt_client(token);
    if (rc != 0) { 
        log_i("【MQTT】❌❌❌ 初始化MQTT客户端并连接失败\n");
        return rc;
    }
    log_i("【MQTT】初始化MQTT客户端并连接成功,rc: %d\n", rc);
    g_mqtt_connected = true;
    return 0;
}

/* MQTT重连函数，持续重连直到成功 */
static void log_mqtt_reconnect_until_success(void) {
    while (!log_mqtt_is_connected() && g_monitor_thread_running) {
        log_i("【MQTT】尝试连接......\n");
        int ret = log_mqtt_connect();
        if (ret == 0) {
            log_i("【MQTT】连接成功\n");
            mqtt_subscribe_reset();
            break;
        } else {
            log_i("【MQTT】连接失败,1秒后重试, error code: %d\n", ret);
            msleep(1000);  // 等待1000ms后重试
        }
    }
}

/* 断开MQTT连接 */
void log_mqtt_disconnect(void) {
    if (g_mqtt_client != NULL) {
        MQTTDisconnect(g_mqtt_client);
        g_mqtt_client = NULL;
        mqtt_network.disconnect(&mqtt_network);
        g_mqtt_connected = false;
        log_i("MQTT connection disconnected\n");
    }
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
            log_i("【MQTT】重连失败\n");
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

// 批量上传队列中的所有日志
static void upload_all_logs(void) {
    os_mutex_pend(&g_log_mqtt_mutex, 0);
    
    if (log_queue.size == 0) {
        os_mutex_post(&g_log_mqtt_mutex);
        return;
    }
    
    uint32_t start_time = timer_get_ms();
    const int MAX_SIZE = sizeof(mqtt_send_buf) - 20;
    int count = 0;
    
    // 使用独立缓冲区构建JSON数组
    char *ptr = json_build_buf;
    int current_len = 0;
    
    // 写入开始的 '['
    current_len = snprintf(ptr, sizeof(json_build_buf), "[");
    ptr += current_len;
    
    while (log_queue.size > 0 && my_blocking_queue_try_dequeue(&log_queue, temp) == MYERROR_Success) {
        int item_len = strlen(temp);
        
        // 计算添加这条后的总长度: 当前长度 + 逗号(如果不是第一条) + 日志内容 + ']'
        int need_len = current_len + (count > 0 ? 1 : 0) + item_len + 1;
        
        // 判断累积是否超限
        if (need_len > MAX_SIZE) {
            // 封装当前缓冲区
            snprintf(ptr, sizeof(json_build_buf) - current_len, "]");
            
            // 把当前取出的放回队列头
            if (my_blocking_queue_push_front(&log_queue, temp) != MYERROR_Success) {
                log_i("【MQTT】放回队列失败，日志丢失: %s\n", temp);
            }
            int send_bytes = strlen(json_build_buf);
            // 发送当前批次
            int result = log_mqtt_send_message("h-log", json_build_buf);
            if (result == 0) {
                uint32_t end_time = timer_get_ms();
                log_i("【MQTT】批量发送完成（超限）- %d字节, 耗时 %u ms\n", send_bytes, end_time - start_time);
                os_mutex_post(&g_log_mqtt_mutex);
                return;
            }
            result = log_mqtt_send_message("h-log", json_build_buf);
            if (result != 0) {
                log_i("【MQTT】❌❌❌ 批量日志发送失败, error code: %d\n", result);
                os_mutex_post(&g_log_mqtt_mutex);
                return;
            }
            uint32_t end_time = timer_get_ms();
            log_i("【MQTT】超限批量日志发送完成(重试) - %d字节, 耗时 %u ms\n", send_bytes, end_time - start_time);
            os_mutex_post(&g_log_mqtt_mutex);
            return;
        }
        
        // 未超限，追加到缓冲区
        int len = snprintf(ptr, sizeof(json_build_buf) - current_len, "%s%s", 
                          count > 0 ? "," : "", temp);
        ptr += len;
        current_len += len;
        count++;
    }
    
    // 收尾处理
    if (count > 0) {
        // 封装并发送
        snprintf(ptr, sizeof(json_build_buf) - current_len, "]");
        int send_bytes = strlen(json_build_buf);
        
        int result = log_mqtt_send_message("h-log", json_build_buf);
        if (result == 0) {
            uint32_t end_time = timer_get_ms();
            log_i("【MQTT】批量发送完成 - %d字节, 耗时 %u ms\n", send_bytes, end_time - start_time);
            os_mutex_post(&g_log_mqtt_mutex);
            return;
        }
        result = log_mqtt_send_message("h-log", json_build_buf);
        if (result != 0) {
            log_i("【MQTT】❌❌❌ 批量日志发送失败, error code: %d\n", result);
        }
        uint32_t end_time = timer_get_ms();
        log_i("【MQTT】批量日志发送完成(重试) - %d字节, 耗时 %u ms\n", send_bytes, end_time - start_time);
    } else {
        log_i("【MQTT】无数据发送\n");
    }
    
    os_mutex_post(&g_log_mqtt_mutex);
}


// 常驻上传线程：检测队列阈值和超时，并上传日志
static void log_monitor_thread(void* arg) {
    // 启动MQTT日志上传线程
    log_i("【MQTT】日志上传线程启动\n");

    g_monitor_thread_running = true;
    
    // 初次连接，失败则持续重连
    log_mqtt_reconnect_until_success();

    last_upload_time = timer_get_ms();  // 初始化上传时间
    while (g_monitor_thread_running) {
        // 检查连接状态，断线则重连
        if (!log_mqtt_is_connected()) {
            log_mqtt_reconnect_until_success();
        }
        if (!log_mqtt_is_connected()) {
            break;  // 无法连接则退出循环
        }
        
        // 定期调用 MQTTYield 处理心跳和接收消息
        if (g_mqtt_client != NULL && log_mqtt_is_connected()) {
            MQTTYield(g_mqtt_client, 50);  // 50ms超时，处理心跳和消息
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
        msleep(150);
    }
    
    my_blocking_queue_destroy_static(&log_queue);// 释放队列资源

    log_mqtt_disconnect();// 断开MQTT连接
    
    // 在线程退出时关闭RTC设备（备份清理，确保资源释放）
    if (g_rtc_device != NULL) {
        dev_close(g_rtc_device);
        g_rtc_device = NULL;
        log_i("【MQTT】RTC设备已关闭（线程退出时）\n");
    }

    g_mqtt_start = false;
    
    log_i("【MQTT】上传线程退出 Log monitor thread exited\n");
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
    
    // 从系统时间戳获取时间
    get_system_timestamp(time_str, sizeof(time_str));

    // 构造JSON
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "event", "batt");
    cJSON_AddStringToObject(root, "time", time_str);
    cJSON_AddStringToObject(root, "sn", sn_param);
    cJSON_AddNumberToObject(root, "batt", battery); // 直接添加整数

    char *json_str = cJSON_PrintUnformatted(root);

    // 打印到控制台
    log_i("【MQTT】battery: %s %d\n", sn_param, battery);

    // 发送到MQTT
    if (json_str != NULL) {
        int result = log_mqtt_send_message("h-status", json_str);
        if (result == 0) {
            log_i("【MQTT】电池状态日志发送成功\n");
        } else {
            result = log_mqtt_send_message("h-status", json_str);
            if (result != 0) {
                log_i("【MQTT】❌❌❌ 电池状态日志发送失败, error code: %d\n", result);
            }
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

/**
 * @brief 发送MQTT消息
 * @param format 格式化字符串
 * @note 注意，使用该函数不可以在字符串中包含\n换行符
 */
void log_mqtt(const char* format, ...) {
    //检查队列是否已经初始化
    if (!g_mqtt_start) {
        log_i("【MQTT】队列未初始化，无法发送消息");
        return;
    }
    // 使用栈上的局部变量，每个线程有自己的副本，避免竞态条件
    char log_buffer[LOG_QUEUE_ITEM_MAX_LEN];
    char time_str[32];
    char json_item_buf[LOG_QUEUE_ITEM_MAX_LEN];
    
    // 格式化日志消息
    __builtin_va_list args;
    va_start(args, format);
    int msg_len = vsnprintf(log_buffer, sizeof(log_buffer), format, args);
    va_end(args);
    
    // 打印到控制台
    log_i("【MQTT】message: %s", log_buffer);

    // 获取时间戳（耗时操作，在锁外执行）
    get_system_timestamp(time_str, sizeof(time_str));

    // 构建JSON字符串（快速操作，在锁外执行）
    int json_len = snprintf(json_item_buf, sizeof(json_item_buf), 
                            "{\"time\":\"%s\",\"sn\":\"%s\",\"msg\":\"%s\"}", 
                            time_str, sn, log_buffer);
    
    // 检查JSON构建是否成功
    if (json_len < 0 || json_len >= sizeof(json_item_buf)) {
        log_i("【MQTT】JSON构建失败或截断（长度:%d）\n", json_len);
        return;  // JSON构建失败，无法入队
    }
    
    // 入队操作：队列内部已经有锁保护（my_blocking_queue_try_enqueue内部有pthread_mutex_lock）
    int ret = my_blocking_queue_try_enqueue(&log_queue, json_item_buf);
    if (ret != MYERROR_Success) {
        switch (ret) {
            case MYERROR_Null_Pointer:
                log_i("【MQTT】❌❌❌ 日志入队失败：空指针错误\n");
                break;
            case MYERROR_Malloc_Failed:
                log_i("【MQTT】❌❌❌ 日志入队失败：内存分配失败\n");
                break;
            case MYERROR_OS_Api_Error:
                log_i("【MQTT】❌❌❌ 日志入队失败：操作系统API错误\n");
                break;
            case MYERROR_Is_Full:
                log_i("【MQTT】❌❌❌ 日志入队失败：队列已满\n");
                break;
            case MYERROR_Is_Empty:
                log_i("【MQTT】❌❌❌ 日志入队失败：队列为空（异常状态）\n");
                break;
            case MYERROR_Not_Found:
                log_i("【MQTT】❌❌❌ 日志入队失败：未找到（异常状态）\n");
                break;
            default:
                log_i("【MQTT】❌❌❌ 日志入队失败：未知错误，错误码: %d\n", ret);
                break;
        }
    }
}
//实现一个同步连接mqtt并发送一条信息的函数
int log_mqtt_send_message_sync(const char* topic, const int progress) {
    // 获取时间戳和构建消息在锁外进行，减少锁持有时间
    char time_str[32];
    if (get_system_timestamp(time_str, sizeof(time_str)) != 0) {
        log_i("【MQTT】ota获取时间戳失败\n");
        return -1;
    }

    // 简化消息格式，去除特殊字符
    char message[128];
    int msg_len = snprintf(message, sizeof(message),
                          "{\"time\":\"%s\",\"sn\":\"%s\",\"msg\":\"OTA更新进度：%d /100\"}",
                          time_str, sn, progress);

    if (msg_len < 0 || msg_len >= sizeof(message)) {
        log_i("【MQTT】ota JSON构建失败或截断（长度:%d）\n", msg_len);
        return -1;
    }

    log_i("【MQTT】ota开始同步发送进度: %d\n", progress);

    // 每次未连接时都尝试重连
    if (!log_mqtt_is_connected()) {
        log_i("【MQTT】ota未连接，尝试重连\n");
        os_mutex_pend(&g_log_mqtt_mutex, 0);
        int ret = log_mqtt_connect();
        os_mutex_post(&g_log_mqtt_mutex);

        if (ret != 0) {
            log_i("【MQTT】ota重连失败\n");
            return -1;
        }
        log_i("【MQTT】ota重连成功，继续发送\n");
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
        log_i("【MQTT】ota发送进度失败, error code: %d\n", rc);
        g_mqtt_connected = false; // 标记连接断开
    } else {
        log_i("【MQTT】ota发送进度成功\n");
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
    }
    
    // 初始化RTC设备互斥锁
    if (!os_mutex_valid(&g_rtc_mutex)) {
        os_mutex_create(&g_rtc_mutex);
    }
    
    // 打开RTC设备（在整个MQTT线程生命周期内保持打开，避免频繁打开/关闭，性能优化）
    if (g_rtc_device == NULL) {
        g_rtc_device = dev_open("rtc", NULL);
        if (g_rtc_device == NULL) {
            log_i("【MQTT】⚠️ RTC设备打开失败，时间戳功能将使用回退方法（性能较低）\n");
            // 不返回错误，允许继续运行（使用回退方法）
        } else {
            log_i("【MQTT】✓ RTC设备已打开，时间戳性能优化启用（保持设备打开）\n");
        }
    }
    
    // 获取SN
    get_SN(sn);
    // 初始化队列用于缓存MQTT日志消息
    my_blocking_queue_init_static(&log_queue, log_queue_data, LOG_QUEUE_ITEM_MAX_LEN, LOG_QUEUE_MAX_COUNT);
    // 设置启动标志
    g_mqtt_start = true;
    // 启动线程用于监控队列阈值和超时
    int ret = thread_fork("log_mqtt_monitor", 15, 2048 * 4, 0, &monitor_thread_pid, log_monitor_thread, NULL);
    if (ret != 0) {
        log_i("【MQTT】❌❌❌ Failed to create monitor thread, error: %d\n", ret);
        // 清理：关闭RTC设备（如果线程创建失败）
        if (g_rtc_device != NULL) {
            dev_close(g_rtc_device);
            g_rtc_device = NULL;
        }
        return ret;
    }
    return 0;
}

// 添加停止函数
void log_mqtt_stop(void) {
    if (!g_mqtt_start) {
        log_i("【MQTT】队列未初始化，无法停止");
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

    // 关闭RTC设备（释放资源）
    if (g_rtc_device != NULL) {
        dev_close(g_rtc_device);
        g_rtc_device = NULL;
        log_i("【MQTT】RTC设备已关闭\n");
    }

    // 确保最后的消息都发送出去
    // upload_all_logs();
}