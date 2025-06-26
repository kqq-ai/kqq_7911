/**
 * @author chuxuanhuang
 * @brief 
 *   +----> IDLE
 *   |
 * POWER_ON -------> NET_CONNECTING <-------> NET_CONNECTED
 *   |                   ^    ^                     ^
 *   |                   |    |                     |
 *   +----> DEFAULT <----+    +---- WSS_CONNECTED <-+
 * 
 * POWER 上电模式
 * IDLE 空闲模式（无任务不代表低功耗）
 * DEFAULT 默认模式：
 *     默认开启BLE ADV，提供配网功能
 * NET_CONNECTING 连接模式
 *     尝试连接WIFI
 * NET_CONNECTED 已连接WIFI的模式
 *     尝试连接后端服务器
 * WSS_CONNECTED 后端服务器连接成功模式
 *     可以进行应用逻辑处理
 */
#include "net_connected.h"
#include "../lwl_app_framework.h"
#include "../lwl_app_core.h"
#include "../wss_connected/wss_application.h"
#include "../wss_connected/test/eend_battery_live_test.h"
#include "log.h"
#include "ota/http_ota.h"

// #include "protocol/odm/odmp.h"
// #include "protocol/odm/odm_util.h"

#include "protocol/lingos/loswssp.h"

#include "wss/wss_client.h"

#include "protocol/odm/odm_secure.h"
#include "mqtt/MQTTClient.h"

#include "lfs.h"

// #define TEST_LOCOL_MODE

///这里不用def，直接使用文件名 #define APP_BIG_LOG_FILENAME_B "app_big_log_B.txt"

#define COMMAND_TIMEOUT_MS 30000        //命令超时时间
#define MQTT_TIMEOUT_MS 10000           //接收阻塞时间
#define MQTT_KEEPALIVE_TIME 30000       //心跳时间
#define SEND_BUF_SIZE 1024              //发送buf大小
#define READ_BUF_SIZE 1024              //接收buf大小
static char send_buf[SEND_BUF_SIZE];    //发送buf
static char read_buf[READ_BUF_SIZE];    //接收buf

// 获取文件系统的全局句柄
extern lfs_t g_lfs;
//接收回调，当订阅的主题有信息下发时，在这里接收
static void messageArrived(MessageData *data)
{
    char temp[128] = {0};

    strncpy(temp, data->topicName->lenstring.data, data->topicName->lenstring.len);
    temp[data->topicName->lenstring.len] = '\0';
    log_i("Message arrived on topic (len : %d, topic : %s)\n", data->topicName->lenstring.len, temp);

    memset(temp, 0, sizeof(temp));
    strncpy(temp, data->message->payload, data->message->payloadlen);
    temp[data->message->payloadlen] = '\0';
    log_i("message (len : %d, payload : %s)\n", data->message->payloadlen, temp);
}

static int mqtt_start(void)
{
    Client client;
    Network network;
    MQTTPacket_connectData connectData = MQTTPacket_connectData_initializer;
    MQTTMessage message;
    int err;
    int loop_cnt = 0;
    int temperature = 0;
    int humidity = 0;

    char sendbuf[256];
    char payload[256] = "[00:00:51.132] ZLCNC5E21XEEEF6 %s";
    char *address = "101.126.136.69";////:1883
    char *username = "ZLCNC5E21XEEEF6";
    char *password = "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJkYXRhIjoiS0NDTkM1RjEyQkVFRUo1IiwiaWF0IjoxNzUwNzUyNTQxLCJpc3MiOiJ3b25kZXJsYW5kIiwic3ViIjoiand0LXRva2VuIiwibmJmIjoxNzUwNzUyNTQxfQ.8PTDptxKvY8vSsyzpYQmZkYn_zBrWz1g6YCl11XrRS8";
    char *subscribeTopic = "hardw";                      //订阅的主题
    char *publishTopic = "h-log";   //发布消息的主题
    char *clientID = "12345|securemode=3,signmethod=hmacsha1|";

_reconnect:
    //初始化网络接口
    NewNetwork(&network);

    SetNetworkRecvTimeout(&network, 1000);

    //初始化客户端
    MQTTClient(&client, &network, COMMAND_TIMEOUT_MS, send_buf, sizeof(send_buf), read_buf, sizeof(read_buf));

    //tcp层连接服务器
    err = ConnectNetwork(&network, address, 1883);
    if (err != 0) {
        log_i("ConnectNetwork fail\n");
        return -1;
    }

    connectData.willFlag = 0;
    connectData.MQTTVersion = 3;                                   //mqtt版本号
    connectData.clientID.cstring = clientID;                       //客户端id
    connectData.username.cstring = username;                       //连接时的用户名
    connectData.password.cstring = password;                       //连接时的密码
    connectData.keepAliveInterval = MQTT_KEEPALIVE_TIME / 1000;    //心跳时间
    connectData.cleansession = 1;                                  //是否使能服务器的cleansession，0:禁止, 1:使能

    //mqtt层连接,向服务器发送连接请求
    err = MQTTConnect(&client, &connectData);
    if (err != 0) {
        network.disconnect(&network);
        log_i("MQTTConnect fail, err : 0x%x\n", err);
        return -1;
    }

    //订阅主题
    err = MQTTSubscribe(&client, subscribeTopic, QOS1, messageArrived);
    if (err != 0) {
        MQTTDisconnect(&client);
        network.disconnect(&network);
        log_i("MQTTSubscribe fail, err : 0x%x\n", err);
        return -1;
    }

    //取消主题订阅
    //MQTTUnsubscribe(&client, subscribeTopic);

    message.qos = QOS1;
    message.retained = 0;

        sprintf(sendbuf, payload,"Login 5 second");///, temperature, humidity
        message.payload = sendbuf;
        message.payloadlen = strlen(sendbuf) + 1;

        temperature += 1;
        humidity += 2;

        if (temperature > 100) {
            temperature = 0;
        }

        if (humidity > 100) {
            humidity = 0;
        }

        //发布消息
        err = MQTTPublish(&client, publishTopic, &message);
        if (err != 0) {
            log_i("MQTTPublish fail, err : 0x%x\n", err);
            return 0;
        }

            log_i("MQTTPublish payload:(%s)\n", sendbuf);

    lfs_file_t file;
    int ret = lfs_file_open(&g_lfs, &file, "app_big_log_B.txt", LFS_O_RDONLY);
    if (ret < 0) {
        log_e("Failed to open log file B: %d", ret);
        return 0;
    }
    log_i("MQTTPublish, app_big_log_B.txt, lfs_file_open OK:%d", lfs_file_size(&g_lfs, &file));

    // 读取缓冲区
    char buffer[228];
    char line[398];
    int line_index = 0;
    lfs_ssize_t bytes_read;
    puts("MQTTPublish, after char line[define] ");
    extern int OTA_download_running;
    OTA_download_running = 1;
    while (1) {
        bytes_read = lfs_file_read(&g_lfs, &file, buffer, sizeof(buffer));
        ///log_i("MQTT file_read, in cir OK:%d", bytes_read );
        
        // 错误处理
        if (bytes_read < 0) {
            log_e("Read error: %d", bytes_read);
            break;
        }
        
        // 文件结束
        if (bytes_read == 0) {
            break;
        }
        
        // 处理读取的数据
        for (int i = 0; i < bytes_read; i++) {
            if (buffer[i] == '\n') {
                // 确保字符串结束
                if (line_index < (int)sizeof(line)) {
                    line[line_index] = '\0';
                } else {
                    line[sizeof(line)-1] = '\0'; // 防止溢出
                }
                
                // 调用处理函数
                if (line_index > 0) {
                    ///log_i("before  MQTTPublish, in cir :%d", line_index );
                    mdelay(3);  // 毫秒延时函数
                    message.payload = line;
                    message.payloadlen = strlen(line) + 1;
                    err = MQTTPublish(&client, publishTopic, &message);
                    ///log_i("MQTTPublish, after OK:%d", line_index );
                }
                
                line_index = 0;
            } else {
                // 检查行缓冲区是否已满
                if (line_index < (int)sizeof(line) - 1) {
                    line[line_index++] = buffer[i];
                } else {
                    // 行过长处理：截断或忽略
                    // 可选：在这里处理过长的行
                    MQTTPublish(&client, publishTopic, &line);
                    log_i("MQTT LONG!!! in cir OK:%d", line_index );
                    line_index = 0;
                }
            }
        }
    }
    
    // 处理最后一行（没有换行符结尾）
    if (line_index > 0) {
        if (line_index < (int)sizeof(line)) {
            line[line_index] = '\0';
        } else {
            line[sizeof(line)-1] = '\0';
        }
        MQTTPublish(&client, publishTopic, &line);
    }
    
    lfs_file_close(&g_lfs, &file);
    lfs_remove(&g_lfs, "app_big_log_B.txt");//  删除文件B, 释放空间~
    log_i("MQTTPublish, app_big_log_B.txt, last LEN:%d", bytes_read);
    MQTTDisconnect(&client);
    network.disconnect(&network);
    OTA_download_running = 0;
}


int app_net_connected_event_handler(struct application *app, struct sys_event *event)
{
    return false;
}

#define WS_TXT_FRAME 0x81
#define WS_BIN_FRAME 0x82

void wss_recv_callback(u8 *buf, u32 len, u8 type)
{
    switch (type) {
    case WS_TXT_FRAME: // text frame
    
        log_i("wbs recv msg : %s\n", buf);
        if (strcmp(buf, "client ping") == 0) {
    
        } else {
            // odmp process
            odmp_rsp_handler(buf, len);
            // loswssp_rsp_handler(buf, len);
        }
        break;
    case WS_BIN_FRAME: // binary frame
        log_i("wbs recv msg : %d\n", len);
        uart_send_set_led_color(10000, 10000, 6000); /// PLAY
        // log_i_buf(buf, len);
        odmp_audio_data_handler(buf, len);
        break;
    }
}

static int net_connected_init(void)
{
    mqtt_start();
    // OTA check version
    int ret = do_check_and_ota();
    if (ret == 1) {
        lwl_app_mode_switch_to(APP_UPDATE_MODE);
        return 0;
    }

#ifdef TEST_LOCOL_MODE
    char *secure_url = get_wss_test_url(); // TODO: 释放资源
    struct websocket_info_t *websockets_info = wss_server_connect(secure_url, wss_recv_callback);
#else
    char *secure_url = get_wss_connection_url(); // TODO: 释放资源
    struct websocket_info_t *websockets_info = wss_server_connect(secure_url, wss_recv_callback);
#endif

    if (websockets_info == NULL) {
        log_i("websockets init failed\n");
        free(websockets_info);
        // TODO 尝试重连WSS
        // lwl_app_mode_switch_to(APP_WSS_CONNECTED_MODE);
    } else {
        /* 将websocket_info保存到全局上下文 */
        lwl_app_context_t *context = lwl_app_core_context_get_instance();
        context->wss_url = secure_url;
        context->websockets_info = websockets_info;
        lwl_app_mode_switch_to(APP_WSS_CONNECTED_MODE);
    }
    return 0;
}


static int net_connected_unint(void)
{
    lwl_app_context_t *context = lwl_app_core_context_get_instance();
    free(context->wss_url);
    context->wss_url = NULL;
    return 0;
}

void app_net_connected_mode(void)
{
    net_connected_init();

    while (1) {
#if 0
        auto_test_application_loop();
#else
#endif
        // 应用代码
        mdelay(10);
        if (lwl_app_mode_exitting()) {
            net_connected_unint();
            return;
        }
    }
}