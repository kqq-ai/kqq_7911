#include "ble_data.h"
#include "string.h"
#include "cpu.h"
#include "os/os_api.h"
#include "wifi/wifi_connect.h"
#include "wifi_app_task.h"
#include "syscfg_id.h"
#include "sys_time.h"
#include "lwip.h"
#include "app_wifi_manage.h"
#include "async_msg_loop.h"
#include "app_mode/lwl_app_core.h"

char current_connected_ssid[MAX_SSID_LEN] = {0};
char current_connected_password[MAX_SSID_LEN] = {0};

static char json_buffer[BUFFER_SIZE];
static int buffer_index = 0;
static int current_fragment = 0;
static int total_fragments = 0;

uint32_t g_wifi_connect_start_time_ms = 0;
uint32_t g_wifi_connect_end_time_ms = 0;
uint32_t g_wifi_connect_timeout_ms = 0;

uint32_t g_wifi_scan_timeout = 0;
uint32_t g_wifi_scan_flag = 1;

void set_current_connected_ssid(char *ssid)
{
    memcpy(current_connected_ssid, ssid, strlen(ssid));
}
void get_current_connected_ssid(char *get_ssid)
{
    memcpy(get_ssid, current_connected_ssid, strlen(current_connected_ssid));
}

void set_current_connected_password(char *password)
{
    memcpy(current_connected_password, password, strlen(password));
}

void get_current_connected_password(char *get_password)
{
    memcpy(get_password, current_connected_password, strlen(current_connected_password));
}

// 清空缓冲区
void clear_buffer()
{
    buffer_index = 0;
    current_fragment = 0;
    total_fragments = 0;
    memset(json_buffer, 0, sizeof(json_buffer));
}

// 解析并拼装数据
int parse_and_assemble(const uint8_t *data, uint16_t data_length, char *assemble_data)
{
    if (buffer_index >= BUFFER_SIZE)
    {
        printf("Buffer overflow\n");
        clear_buffer();
        return -1;
    }

    if (data_length < 4)
    {
        printf("Invalid data length\n");
        clear_buffer();
        return -2;
    }

    // 解析标志位
    uint16_t fragment_number = (data[0] - '0') * 10 + (data[1] - '0');
    uint16_t total_fragments_received = (data[2] - '0') * 10 + (data[3] - '0');

    // 检查是否是新消息的开始
    if (fragment_number == 1)
    {
        clear_buffer();
        current_fragment = 0;
        total_fragments = total_fragments_received;
    }

    // 拷贝原数据到缓冲区
    int payload_start = 4; // 跳过前4个标志位
    int payload_length = data_length - payload_start;

    if (buffer_index + payload_length > BUFFER_SIZE)
    {
        printf("Payload would cause buffer overflow\n");
        clear_buffer();
        return -3;
    }

    memcpy(json_buffer + buffer_index, data + payload_start, payload_length);
    buffer_index += payload_length;

    // 更新当前分片序号
    current_fragment++;

    // 检查是否是最后一包
    if (current_fragment == total_fragments)
    {
        printf("Received complete JSON: %s\n", json_buffer);
        memcpy(assemble_data, json_buffer, strlen(json_buffer));
        printf("Parsed JSON: %s\n", assemble_data);
        clear_buffer(); // 处理完后清空缓冲区
        return 0;
    }
    return -4;
}

int g_scan_start_ms = 0;
// int send_scan_result_count = 0;
static void wifi_scan_timeout_cb(int timeout_s)
{
    set_wifi_info_status(WIFI_INFO_STA_MODE_IDLE);
    printf("scan cmpleted ok !\r\n");
    int time_now = timer_get_ms();
    if (get_wifi_cmd_status(NULL) == WIFI_SCAN_RUNNING)
    {
        if (time_now - g_scan_start_ms < timeout_s * 1000)
        {
            printf("wifi scan once \r\n");
            // send_scan_result_count = send_scan_result_count + 1;
            // if (send_scan_result_count)

            wifi_scan_req();
            set_wifi_scan_status(WIFI_SCAN_RUNNING);
            set_wifi_cmd_status(WIFI_CMD_SCANNING); // 需要验证的地方
            set_wifi_info_status(WIFI_INFO_STA_MODE_SCANNING);
        }
        else
        {
            set_wifi_scan_status(WIFI_SCAN_STOP);
            set_wifi_cmd_status(WIFI_CMD_IDLE);
            //wifi_enter_smp_cfg_mode();           // 待确认
        }
    }
}

// 定义处理不同命令的函数
int handle_wifi_scan(const char *cmd, cJSON *args, char *return_json_data)
{
    printf("=============================in handle_wifi_scan =========================================\r\n");
    int ret = 0;
    char return_errstr[32] = {0};

    // 检查 args 是否为 NULL
    if (args == NULL)
    {
        printf("Error: Arguments are NULL\n");
        ret = 1;
        goto cleanup;
    }

    // 获取 "timeout_s" 项
    cJSON *timeout_item = cJSON_GetObjectItemCaseSensitive(args, "timeout_s");

    // 检查 "timeout_s" 是否存在且为整数类型
    if (!cJSON_IsNumber(timeout_item))
    {
        if (timeout_item == NULL)
        {
            printf("Error: 'timeout_s' not found in arguments\n");
        }
        else
        {
            printf("Error: 'timeout_s' is not a number\n");
        }
        ret = 1;
        goto cleanup;
    }

    // 获取 timeout_s 的值
    uint32_t timeout_s = timeout_item->valueint;

    // 打印调试信息
    printf("Handling %s with timeout_s: %d\n", cmd, timeout_s);

    // 获取开始扫描的时间
    g_scan_start_ms = timer_get_ms();
    printf("wifi_scan_timeout_cb timeout_s = %d\n", timeout_s);
    // 注册超时扫描的回调函数
    register_wifi_scan_func(wifi_scan_timeout_cb, timeout_s);
    set_wifi_cmd_status(WIFI_CMD_SCANNING);

    // 开始扫描
    printf("----------------------wifi_scan_start--------------------------------\r\n");

    /* 将WiFi 扫描任务添加到线程池中 */
    lwl_app_context_t *ctx = lwl_app_core_context_get_instance();
    if (!threadpool_add_task(ctx->threadpool, wifi_scan_task, NULL))
    {
        printf("failed to add wifi_scan_task in threadpool !\r\n");
    }

    // 清除 WiFi list
    clean_wifi_scan_result(NULL);

    if (ret == 0)
    {
        printf("Scan request initiated successfully.\n");
        memcpy(return_errstr, "WIFI_SCAN_CODE_SUCCESS", strlen("WIFI_SCAN_CODE_SUCCESS"));
        g_wifi_scan_flag = 1; /*只有扫描成功后，才能获取wifi list 的标志位*/
        set_wifi_scan_status(WIFI_SCAN_RUNNING);
        // 等待扫描完成事件或定时查询结果
    }
    else if (ret == -1)
    {
        /*前面的状态排查中 可以排除 WiFi off 和 connecting 状态*/
        printf("Scan request failed: WIFI is off, scanning, or connecting.\n");
        // 可以选择等待一段时间后重试
        memcpy(return_errstr, "WIFI_SCAN_CODE_SCANNING", strlen("WIFI_SCAN_CODE_SCANNING"));
        ret = 0;
    }
    else
    {
        printf("Unknown error occurred during scan request.\n");
    }

    // 创建返回的 JSON 数据
    cJSON *root = cJSON_CreateObject();
    if (root == NULL)
    {
        printf("Error: Failed to create cJSON object\n");
        ret = 1;
        goto cleanup;
    }

    cJSON_AddStringToObject(root, "cmd_name", "wifi_scan");
    extern char pt[35];  // 声明外部变量（不分配内存）
        char sn_C_to_phone[16] = {0};
        strncpy(sn_C_to_phone, pt, 15);
        sn_C_to_phone[15] = '\0';
    cJSON_AddStringToObject(root, "sn", sn_C_to_phone);
    cJSON_AddNumberToObject(root, "return_code", ret);
    cJSON_AddStringToObject(root, "return_errstr", return_errstr);

    // 将 cJSON 转换为字符串
    char *json_str = cJSON_PrintUnformatted(root);
    if (json_str == NULL)
    {
        printf("Error: Failed to print cJSON object\n");
        cJSON_Delete(root);
        ret = 1;
        goto cleanup;
    }

    // 确保目标缓冲区足够大
    if (strlen(json_str) >= BUFFER_SIZE)
    {
        printf("Error: Return buffer too small\n");
        free(json_str);
        cJSON_Delete(root);
        ret = 1;
        goto cleanup;
    }

    // 复制 JSON 字符串到返回缓冲区
    memcpy(return_json_data, json_str, strlen(json_str));

    // 清理资源
    free(json_str);
    cJSON_Delete(root);
cleanup:
    return ret;
}

int handle_wifi_list_ready(const char *cmd, cJSON *args, char *return_json_data)
{
    printf("=============================in handle_wifi_list_ready =========================\r\n");

    int ret = 0;
    char return_errstr[32] = {0};
    struct wifi_scan_ssid_info *temp_all_wifi_info = NULL;
    int all_wifi_count = 0;

    if (args == NULL)
    {
        printf("Error: Arguments are NULL\n");
        ret = 1;
        goto cleanup;
    }

    // 获取 "details" 项
    cJSON *details_item = cJSON_GetObjectItemCaseSensitive(args, "details");

    // 检查 "details" 是否存在且为整数类型
    if (!cJSON_IsNumber(details_item))
    {
        if (details_item == NULL)
        {
            printf("Error: 'details' not found in arguments\n");
            ret = 1;
            goto cleanup;
        }
        else
        {
            printf("Error: 'details' is not a number\n");
            ret = 1;
            goto cleanup;
        }
    }

    get_wifi_scan_result(&temp_all_wifi_info, &all_wifi_count);

    if (all_wifi_count == 0)
    {
        printf("the wifi list info is empty\n");
        ret = 0;
        memcpy(return_errstr, "WIFI_LIST_READY_CODE_LIST_EMPTY", strlen("WIFI_LIST_READY_CODE_LIST_EMPTY"));
    }
    else
    {
        ret = 0;
        memcpy(return_errstr, "WIFI_LIST_READY_CODE_LIST_OK", strlen("WIFI_LIST_READY_CODE_LIST_OK"));
    }

    // 封装rerurn_json_data
    cJSON *root = cJSON_CreateObject();
    if (root == NULL)
    {
        ret = 1;
        printf("Error: Failed to create cJSON object\n");
        goto cleanup;
    }
    cJSON_AddStringToObject(root, "cmd_name", "wifi_list_ready");
    cJSON_AddNumberToObject(root, "return_code", ret);
    cJSON_AddStringToObject(root, "return_errstr", return_errstr);
    char *json_str = cJSON_PrintUnformatted(root);
    if (json_str == NULL)
    {
        ret = 1;
        printf("Error: Failed to print cJSON object\n");
        cJSON_Delete(root);
        goto cleanup;
    }
    memcpy(return_json_data, json_str, strlen(json_str));
    free(json_str);
    cJSON_Delete(root);
cleanup:
    return ret;
}

// 定义 details 的含义
#define DETAILS_BASIC 0 /* 仅显示 SSID 和 RSSI*/
#define DETAILS_FULL 1  /* 显示更多详细信息，信道 ch 和加密方式 encrytion */

int handle_wifi_get_list(const char *cmd, cJSON *args, char *return_json_data)
{
    printf(" ================ in wifi_get_list ====================\r\n");
    int ret = 0;
    char return_errstr[32] = {0};
    struct wifi_scan_ssid_info *wifi_list_info_array = NULL;
    int temp_all_wifi_count = 0;
    int judge_size = 0;

    // 检查 args 是否为 NULL
    if (args == NULL)
    {
        printf("Error: Arguments are NULL\n");
        ret = 1;
        goto cleanup;
    }

    // 获取 "details" 项
    cJSON *details_item = cJSON_GetObjectItemCaseSensitive(args, "details");

    // 检查 "details" 是否存在且为整数类型
    if (!cJSON_IsNumber(details_item))
    {
        if (details_item == NULL)
        {
            printf("Error: 'details' not found in arguments\n");
            ret = 1;
            goto cleanup;
        }
        else
        {
            printf("Error: 'details' is not a number\n");
            ret = 1;
            goto cleanup;
        }
    }

    // 获取 details 的值
    int details = details_item->valueint;

    // 验证 details 的值是否合法
    if (details != DETAILS_BASIC && details != DETAILS_FULL)
    {
        printf("Warning: 'details' value %d is not recognized. Using default value %d.\n", details, DETAILS_BASIC);
        ret = 1;
        memcpy(return_errstr, "WIFI_GET_LIST_CODE_EMPTY", strlen("WIFI_GET_LIST_CODE_EMPTY"));
    }

    // 打印调试信息
    printf("Handling %s with details level: %d\n", cmd, details);

    // 获取 wifi 扫描结果
    get_wifi_scan_result(&wifi_list_info_array, &temp_all_wifi_count);

    // malloc 存储 wifi 列表的数组
    struct wifi_scan_ssid_info *temp_all_wifi_info = (struct wifi_scan_ssid_info *)malloc(sizeof(struct wifi_scan_ssid_info) * temp_all_wifi_count);
    if (temp_all_wifi_info == NULL) {
        printf("Error: Failed to allocate memory for temp_all_wifi_info\n");
        ret = 1;
        goto cleanup;
    }
    memcpy(temp_all_wifi_info, wifi_list_info_array, sizeof(struct wifi_scan_ssid_info) * temp_all_wifi_count);

    // 判断 wifi 列表是否为空
    if (temp_all_wifi_count == 0)
    {
        printf("the wifi list info is empty\n");
        ret = 0;
        memcpy(return_errstr, "WIFI_GET_LIST_CODE_EMPTY", strlen("WIFI_GET_LIST_CODE_EMPTY"));
    }
    else
    {
        printf("the wifi list info num is %d\r\n", temp_all_wifi_count);
        ret = 0;
        memcpy(return_errstr, "WIFI_GET_LIST_CODE_SUCCESS", strlen("WIFI_GET_LIST_CODE_SUCCESS"));
    }
    g_wifi_scan_flag = 1; /*此次获取完wifi list 之后还需要再扫描才能拿到新的 wifi list*/

    // 创建根 JSON 对象
    cJSON *root = cJSON_CreateObject();
    if (root == NULL)
    {
        printf("Error: Failed to create cJSON object\n");
        free(temp_all_wifi_info); // 释放malloc存储 wifi list 的空间
        ret = 1;
        goto cleanup;
    }

    // 添加命令名称
    cJSON_AddStringToObject(root, "cmd_name", "wifi_get_list");

    // 创建 WiFi 列表数组
    cJSON *wifi_list = cJSON_CreateArray();
    if (wifi_list == NULL)
    {
        printf("Error: Failed to create cJSON array\n");
        cJSON_Delete(root);
        free(temp_all_wifi_info); // 释放malloc存储 wifi list 的空间
        ret = 1;
        goto cleanup;
    }

    judge_size += 100;

    // 根据 details 的值执行不同的处理逻辑
    if (temp_all_wifi_count > 0)
    {
        for (int i = 0; i < temp_all_wifi_count; ++i)
        {
            cJSON *wifi_item = cJSON_CreateObject();
            if (wifi_item == NULL)
            {
                printf("Error: Failed to create cJSON object for WiFi item\n");
                cJSON_Delete(root);
                cJSON_Delete(wifi_list);
                free(temp_all_wifi_info); // 释放malloc存储 wifi list 的空间
                ret = 1;
                goto cleanup;
            }

            cJSON_AddStringToObject(wifi_item, "ssid", temp_all_wifi_info[i].ssid);
            cJSON_AddNumberToObject(wifi_item, "rssi", temp_all_wifi_info[i].rssi);

            if (details == DETAILS_FULL)
            {
                cJSON_AddNumberToObject(wifi_item, "channel", temp_all_wifi_info[i].channel_number);
            }

            cJSON_AddItemToArray(wifi_list, wifi_item);

            judge_size += strlen(temp_all_wifi_info[i].ssid) + 22;

            if (judge_size >= (BUFFER_SIZE - 150) )
            {
                printf("the wifi_list array is approach the max size --> stop add wifi ssid !\r\n");
                break;
            }
        }
    }
    else
    {
        cJSON_AddStringToObject(wifi_list, "ssid", "");
        cJSON_AddNumberToObject(wifi_list, "rssi", 0);
    }

    cJSON_AddItemToObject(root, "wifi_list", wifi_list);
    cJSON_AddNumberToObject(root, "return_code", ret);
    cJSON_AddStringToObject(root, "return_errstr", return_errstr);

    // 将 cJSON 转换为字符串
    char *json_str = cJSON_PrintUnformatted(root);
    if (json_str == NULL)
    {
        printf("Error: Failed to print cJSON object\n");
        cJSON_Delete(root);
        free(temp_all_wifi_info); // 释放malloc存储 wifi list 的空间
        ret = 1;
        goto cleanup;
    }

    printf("the wifi_list json_str size is %d\r\n", strlen(json_str));

    // 确保目标缓冲区足够大
    if (strlen(json_str) >= BUFFER_SIZE)
    {
        printf("Error: Return buffer too small\n");
        free(json_str);
        cJSON_Delete(root);
        free(temp_all_wifi_info); // 释放malloc存储 wifi list 的空间
        ret = 1;
        goto cleanup;
    }

    // 复制 JSON 字符串到返回缓冲区
    memcpy(return_json_data, json_str, strlen(json_str));

    // 清理资源
    free(json_str);
    free(temp_all_wifi_info); // 释放malloc存储 wifi list 的空间
    cJSON_Delete(root);

cleanup:
    return ret;
}

int handle_wifi_stop_scan(const char *cmd, cJSON *args, char *return_json_data)
{
    int ret = 0;
    char return_errstr[32] = {0};
    cJSON *details = cJSON_GetObjectItemCaseSensitive(args, "details");
    if (cJSON_IsString(details))
    {
        printf("Handling %s with details: %s\n", cmd, details->valuestring);
    }
    // 在这里添加实际的处理逻辑

    if (WIFI_SCAN_STOP == get_wifi_scan_status(NULL))
    {
        printf("the wifi scan timeout is already stop\r\n");
        ret = 1;
        memcpy(return_errstr, "WIFI_STOP_SCAN_CODE_TIMEOUT", strlen("WIFI_STOP_SCAN_CODE_TIMEOUT"));
    }
    else
    {
        printf("the wifi scan timeout is not end\r\n");
        set_wifi_cmd_status(WIFI_SCAN_STOP);
        set_wifi_info_status(WIFI_INFO_STA_MODE_IDLE);
        wifi_enter_smp_cfg_mode();

        if (WIFI_SCAN_STOP == get_wifi_cmd_status(NULL))
        {
            printf("the wifi scan timeout is really ok\r\n");
            ret = 0;
            memcpy(return_errstr, "WIFI_STOP_SCAN_CODE_SUCCESS", strlen("WIFI_STOP_SCAN_CODE_SUCCESS"));
        }
        set_wifi_cmd_status(WIFI_CMD_IDLE);
    }

    // 封装rerurn_json_data
    cJSON *root = cJSON_CreateObject();
    if (root == NULL)
    {
        ret = 1;
        printf("Error: Failed to create cJSON object\n");
        goto cleanup;
    }
    cJSON_AddStringToObject(root, "cmd_name", "wifi_stop_scan");
    cJSON_AddNumberToObject(root, "return_code", ret);
    cJSON_AddStringToObject(root, "return_errstr", return_errstr);
    char *json_str = cJSON_PrintUnformatted(root);
    if (json_str == NULL)
    {
        ret = 1;
        printf("Error: Failed to print cJSON object\n");
        cJSON_Delete(root);
        goto cleanup;
    }
    memcpy(return_json_data, json_str, strlen(json_str));
    free(json_str);
    cJSON_Delete(root);
cleanup:
    return ret;
}

uint32_t set_wifi_connect_timeout_ms(uint32_t wifi_connect_timeout_ms)
{
    g_wifi_connect_timeout_ms = wifi_connect_timeout_ms;
    return g_wifi_connect_timeout_ms;
}
uint32_t get_wifi_connect_timeout_ms(void *pri)
{
    return g_wifi_connect_timeout_ms;
}
uint32_t set_wifi_connect_start_time_ms(uint32_t wifi_connect_start_time_ms)
{
    g_wifi_connect_start_time_ms = wifi_connect_start_time_ms;
}
uint32_t set_wifi_connect_end_time_ms(uint32_t wifi_connect_end_time)
{
    g_wifi_connect_end_time_ms = wifi_connect_end_time;
    return g_wifi_connect_end_time_ms;
}

int handle_wifi_connect(const char *cmd, cJSON *args, char *return_json_data)
{
    int ret = 0;
    char return_errstr[32] = {0};
    // char return_status[32] = {0};
    // char ip_address[16] = {0};
    int now_wifi_cnnect_status;

    const char *ssid = cJSON_GetObjectItemCaseSensitive(args, "ssid")->valuestring;
    const char *password = cJSON_GetObjectItemCaseSensitive(args, "password")->valuestring;
    int connect_timeout_s = cJSON_GetObjectItemCaseSensitive(args, "timeout_s")->valueint;
    // 检查 ssid 和 password 是否为 NULL
    if (ssid == NULL || password == NULL)
    {
        printf("Error: SSID or password is missing\n");
        ret = 1;
        goto cleanup;
    }
    printf("Handling %s with ssid: %s, password: %s, timeout: %d\n", cmd, ssid, password, connect_timeout_s);
    // 在这里添加实际的处理逻辑
    if (strlen(ssid) > MAX_SSID_LEN || strlen(password) > MAX_PWD_LEN)
    {
        printf("Error: SSID or password is too long\n");
        ret = 1;
        goto cleanup;
    }

    /*后面两个参数，需要实际需求的时候加上 两个callback函数*/
    // my_task_wifi_connect(ssid, password, connect_timeout_s, NULL, NULL, NULL, NULL);

    lwl_app_context_t *ctx = lwl_app_core_context_get_instance();
    /* 在wifi_connect_task任务中会进行释放 */
    wifi_connect_params_t *wifi_connect_params = (wifi_connect_params_t *)malloc(sizeof(wifi_connect_params_t));
    if (wifi_connect_params == NULL)
    {
        printf("Failed to allocate memory for wifi_connect_params\n");
    }
    memset(wifi_connect_params, 0, sizeof(wifi_connect_params_t));

    memcpy(wifi_connect_params->ssid, ssid, sizeof(wifi_connect_params->ssid));
    memcpy(wifi_connect_params->pwd, password, sizeof(wifi_connect_params->pwd));
    wifi_connect_params->timeout_s = connect_timeout_s;
    wifi_connect_params->connect_succ_func = NULL;
    wifi_connect_params->connect_fail_func = NULL;
    /* 将WiFi 连接任务添加到线程池中 */
    if (!threadpool_add_task(ctx->threadpool, wifi_connect_task, wifi_connect_params))
    {
        printf("failed to add wifi_connect_task in threadpool !\r\n");
        free(wifi_connect_params);
    }

    set_wifi_connect_start_time_ms(timer_get_ms());

    memcpy(return_errstr, "WIFI_CONNECT_CODE_SUCCESS", strlen("WIFI_CONNECT_CODE_SUCCESS"));

    set_wifi_cmd_status(WIFI_CMD_CONNECTING);

    // 保存连接信息
    app_wifi_info_t *app_wifi_info = (app_wifi_info_t *)malloc(sizeof(app_wifi_info_t));
    if (app_wifi_info == NULL)
    {
        printf("Failed to allocate memory for app_wifi_info\n");
    }
    memcpy(app_wifi_info->ssid, ssid, sizeof(app_wifi_info->ssid));
    memcpy(app_wifi_info->pwd, password, sizeof(app_wifi_info->pwd));
    set_app_wifi_info(app_wifi_info);


    // 封装return_json_data
    cJSON *root = cJSON_CreateObject(); // 创建根节点
    if (!root)
    {
        printf("Failed to create cJSON object\n");
        ret = 1;
        goto cleanup;
    }
    // memcpy(ip_address, "192.168.1.19", strlen("192.168.1.19"));

    cJSON_AddStringToObject(root, "cmd_name", cmd);    // 添加命令名称
    cJSON_AddNumberToObject(root, "return_code", ret); // 添加返回码
    // cJSON_AddStringToObject(root, "return_status", return_status); // 添加状态信息
    cJSON_AddStringToObject(root, "return_errstr", return_errstr); // 添加错误信息
    // cJSON_AddStringToObject(root, "ip_address", ip_address);       // 添加IP地址

    // 将 JSON 对象转换为字符串
    char *json_str = cJSON_PrintUnformatted(root);
    if (json_str == NULL)
    {
        ret = 1;
        printf("Error: Failed to print cJSON object\n");
        cJSON_Delete(root);
        goto cleanup;
    }
    memcpy(return_json_data, json_str, strlen(json_str));
    free(json_str);
    cJSON_Delete(root);
cleanup:
    return ret;
}

int handle_wifi_disconnect(const char *cmd, cJSON *args, char *return_json_data)
{
    int ret = 0;
    char return_errstr[32] = {0};
    enum wifi_sta_connect_state now_wifi_cnnect_status;

    cJSON *ssid = cJSON_GetObjectItemCaseSensitive(args, "ssid");
    if (cJSON_IsString(ssid))
    {
        printf("Handling %s with ssid: %s\n", cmd, ssid->valuestring);
    }
    // 在这里添加实际的处理逻辑

    /*第一个参数为 callback函数,需要传入的时候申明之后再传入*/

    /* 将WiFi 断连任务添加到线程池中 */
    lwl_app_context_t *ctx = lwl_app_core_context_get_instance();
    if (!threadpool_add_task(ctx->threadpool, wifi_disconnect_task, NULL))
    {
        printf("failed to add wifi_disconnect_task in threadpool !\r\n");
    }

    memcpy(return_errstr, "WIFI_DISCONNECT_CODE_SUCCESS", strlen("WIFI_DISCONNECT_CODE_SUCCESS"));
    // 封装return_json_data
    cJSON *root = cJSON_CreateObject();
    if (root == NULL)
    {
        ret = 1;
        printf("Error: Failed to create cJSON object\n");
        goto cleanup;
    }
    cJSON_AddStringToObject(root, "cmd_name", cmd);
    cJSON_AddNumberToObject(root, "return_code", ret);
    cJSON_AddStringToObject(root, "return_errstr", return_errstr);
    char *json_str = cJSON_PrintUnformatted(root);
    if (json_str == NULL)
    {
        ret = 1;
        printf("Error: Failed to print cJSON object\n");
        cJSON_Delete(root);
        goto cleanup;
    }
    memcpy(return_json_data, json_str, strlen(json_str));
    free(json_str);
    cJSON_Delete(root);
cleanup:
    return ret;
}

int handle_wifi_reconnect(const char *cmd, cJSON *args, char *return_json_data)
{
    int ret = 0;
    char return_errstr[32] = {0};
    bool enable;

    struct wifi_stored_sta_info wifi_stored_info[NETWORK_SSID_INFO_CNT];
    struct wifi_mode_info wifi_mode_info;
    memset(wifi_stored_info, 0, sizeof(wifi_stored_info));

    if (cJSON_GetObjectItemCaseSensitive(args, "enable")->type == cJSON_True)
    {
        enable = true;
    }
    else if (cJSON_GetObjectItemCaseSensitive(args, "enable")->type == cJSON_False)
    {
        enable = false;
    }
    int retry = cJSON_GetObjectItemCaseSensitive(args, "retry")->valueint;

    printf("Handling %s with enable: %d, retry: %d\n", cmd, enable, retry);

    // 在这里添加实际的处理逻辑
    if (true == enable)
    {
        // printf("prepare wifi reconnect\r\n");
        // ret = wifi_get_stored_sta_info(wifi_stored_info);
        // printf("the saved wifi ssid count : %d\r\n", ret);
        // for (int i = 0; i < ret; i++)
        // {
        //     if (wifi_stored_info[i].ssid != NULL)
        //     {
        //         printf("the saved wifi ssid : %s\r\n", wifi_stored_info[i].ssid);
        //         printf("the saved wifi password : %s\r\n", wifi_stored_info[i].pwd);
        //     }
        // }
        // while (retry--)
        // {
        //     // wifi_enter_sta_mode(wifi_stored_info[0].ssid, wifi_stored_info[0].pwd);
        //     // my_task_wifi_connect(wifi_stored_info[0].ssid, wifi_stored_info[0].pwd, 10, NULL, NULL, NULL, NULL);
        // }
        ret = 0;
        memcpy(return_errstr, "WIFI_RECONNECT_CODE_SUCCESS", strlen("WIFI_RECONNECT_CODE_SUCCESS"));
    }
    else if (false == enable)
    {
        ret = 0;
        memcpy(return_errstr, "WIFI_RECONNECT_CODE_SUCCESS", strlen("WIFI_RECONNECT_CODE_SUCCESS"));
    }
    // 封装return_json_data
    cJSON *root = cJSON_CreateObject();
    if (root == NULL)
    {
        ret = 1;
        printf("Error: Failed to create cJSON object\n");
        goto cleanup;
    }
    cJSON_AddStringToObject(root, "cmd_name", cmd);
    cJSON_AddNumberToObject(root, "return_code", ret);
    cJSON_AddStringToObject(root, "return_errstr", return_errstr);

    char *json_str = cJSON_PrintUnformatted(root);
    if (json_str == NULL)
    {
        ret = 1;
        printf("Error: Failed to print cJSON object\n");
        cJSON_Delete(root);
        goto cleanup;
    }
    memcpy(return_json_data, json_str, strlen(json_str));
    free(json_str);
    cJSON_Delete(root);

cleanup:
    return ret;
}

int handle_wifi_get_status(const char *cmd, cJSON *args, char *return_json_data)
{
    int ret = 0;
    char return_errstr[32] = {0};
    char saved_ssid[MAX_SSID_LEN] = {0};
    char connect_status[32] = {0};
    char return_str[32] = {0};

    char temp_ssid[32] = {0};
    enum wifi_sta_connect_state temp_connect_state;

    cJSON *ssid = cJSON_GetObjectItemCaseSensitive(args, "ssid");
    if (cJSON_IsString(ssid))
    {
        printf("Handling %s with ssid: %s\n", cmd, ssid->valuestring);
    }
    // 在这里添加实际的处理逻辑
    if (ssid == NULL)
    {
        get_current_connected_ssid(temp_ssid);
    }
    else if (ssid != NULL)
    {
        memcpy(temp_ssid, ssid->valuestring, strlen(ssid->valuestring));
    }

    temp_connect_state = get_wifi_connect_status(NULL);
    if (temp_connect_state == WIFI_CONNECT_IDLE)
    {
        memcpy(connect_status, "WIFI_STA_DISCONNECT", strlen("WIFI_STA_DISCONNECT"));
    }
    else if (temp_connect_state == WIFI_CONNECT_SUCC)
    {
        memcpy(connect_status, "WIFI_STA_CONNECT_SUCC", strlen("WIFI_STA_CONNECT_SUCC"));
    }
    else if (temp_connect_state == WIFI_CONNECT_ASSOCIAT_FAIL)
    {
        memcpy(connect_status, "WIFI_STA_CONNECT_TIMEOUT_NOT_FOUND_SSID", strlen("WIFI_STA_CONNECT_TIMEOUT_NOT_FOUND_SSID"));
    }
    else if (temp_connect_state == WIFI_CONNECT_ASSOCIAT_TIMEOUT)
    {
        memcpy(connect_status, "WIFI_STA_CONNECT_ASSOCIAT_FAIL", strlen("WIFI_STA_CONNECT_ASSOCIAT_FAIL"));
    }
    else if (temp_connect_state == WIFI_CONNECT_STA_DISCONNECT)
    {
        memcpy(connect_status, "WIFI_STA_CONNECT_TIMEOUT_NOT_FOUND_SSID", strlen("WIFI_STA_CONNECT_TIMEOUT_NOT_FOUND_SSID"));
    }
    else if (temp_connect_state == WIFI_CONNECT_TIMEOUT_NO_SSID)
    {
        memcpy(connect_status, "WIFI_STA_CONNECT_TIMEOUT_NOT_FOUND_SSID", strlen("WIFI_STA_CONNECT_TIMEOUT_NOT_FOUND_SSID"));
    }
    else if (temp_connect_state == WIFI_CONNECT_DHCP_SUCC)
    {
        memcpy(connect_status, "WIFI_STA_NETWORK_STACK_DHCP_SUCC", strlen("WIFI_STA_NETWORK_STACK_DHCP_SUCC"));
    }
    else if (temp_connect_state == WIFI_CONNECT_DHCP_TIMEOUT)
    {
        memcpy(connect_status, "WIFI_STA_NETWORK_STACK_DHCP_TIMEOUT", strlen("WIFI_STA_NETWORK_STACK_DHCP_TIMEOUT"));
    }

    memcpy(return_str, "WIFI_INFO_CODE_SUCCESS", strlen("WIFI_INFO_CODE_SUCCESS"));

    // 封装return_json_data
    cJSON *root = cJSON_CreateObject();
    if (root == NULL)
    {
        ret = 1;
        printf("Error: Failed to create cJSON object\n");
        goto cleanup;
    }
    cJSON_AddStringToObject(root, "cmd_name", cmd);
    cJSON_AddStringToObject(root, "connect_status", connect_status);
    cJSON_AddStringToObject(root, "saved_ssid", temp_ssid);
    cJSON_AddNumberToObject(root, "return_code", ret);
    cJSON_AddStringToObject(root, "return_errstr", return_errstr);

    char *json_str = cJSON_PrintUnformatted(root);
    if (json_str == NULL)
    {
        ret = 1;
        printf("Error: Failed to print cJSON object\n");
        cJSON_Delete(root);
        goto cleanup;
    }
    memcpy(return_json_data, json_str, strlen(json_str));
    free(json_str);
    cJSON_Delete(root);
    printf("Exiting parse_and_dispatch_json\n");

cleanup:
    return ret;
}

int handle_wifi_info(const char *cmd, cJSON *args, char *return_json_data)
{
    int ret = 0;
    char return_errstr[32] = {0};
    char sta_localip[WIFI_IP_LEN] = {0};
    u8 mac[6] = {0};
    char mac_str[32] = {0};
    char hostname[32] = {0};
    bool bool_sta_localip;
    bool bool_mac;
    bool bool_hostname;
    struct netif_info temp_netif_info;
    memset(&temp_netif_info, 0, sizeof(temp_netif_info));

    // 在这里添加实际的处理逻辑
    cJSON *ssid = cJSON_GetObjectItemCaseSensitive(args, "ssid");
    if (cJSON_GetObjectItemCaseSensitive(args, "sta_localip")->type == cJSON_True)
    {
        bool_sta_localip = true;
        // lwip_get_netif_info(1, &temp_netif_info);
        // u32 temp_ip = htonl(temp_netif_info.ip);
        // snprintf(sta_localip, 32, "%d.%d.%d.%d",
        //          (temp_ip >> 24) & 0xFF, // 第一字节
        //          (temp_ip >> 16) & 0xFF, // 第二字节
        //          (temp_ip >> 8) & 0xFF,  // 第三字节
        //          temp_ip & 0xFF          // 第四字节
        // );
        get_wifi_ip_info(sta_localip);
        printf("sta_localip : %s\n", sta_localip);
    }
    else if (cJSON_GetObjectItemCaseSensitive(args, "sta_localip")->type == cJSON_False)
        bool_sta_localip = false;
    if (cJSON_GetObjectItemCaseSensitive(args, "mac")->type == cJSON_True)
    {
        bool_mac = true;
        wifi_get_mac(mac);
        snprintf(mac_str, 32, "%02X:%02X:%02X:%02X:%02X:%02X",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    }
    else if (cJSON_GetObjectItemCaseSensitive(args, "mac")->type == cJSON_False)
        bool_mac = false;
    if (cJSON_GetObjectItemCaseSensitive(args, "hostname")->type == cJSON_True)
    {
        bool_hostname = true;
    }
    else if (cJSON_GetObjectItemCaseSensitive(args, "hostname")->type == cJSON_False)
        bool_hostname = false;

    // 封装return_json_data
    cJSON *root = cJSON_CreateObject();
    if (root == NULL)
    {
        ret = 1;
        printf("Error: Failed to create cJSON object\n");
        goto cleanup;
    }
    memcpy(return_errstr, "WIFI_INFO_CODE_SUCCESS", strlen("WIFI_INFO_CODE_SUCCESS"));

    cJSON_AddStringToObject(root, "cmd_name", cmd);
    cJSON_AddNumberToObject(root, "return_code", ret);
    cJSON_AddStringToObject(root, "return_errstr", return_errstr);
    if (true == bool_sta_localip)
    {
        cJSON_AddStringToObject(root, "sta_localip", sta_localip);
    }
    else
    {
        cJSON_AddStringToObject(root, "sta_localip", "");
    }
    if (true == bool_mac)
    {
        cJSON_AddStringToObject(root, "mac", mac_str);
    }
    else
    {
        cJSON_AddStringToObject(root, "mac", "");
    }
    if (true == bool_hostname)
    {
        cJSON_AddStringToObject(root, "hostname", hostname);
    }
    else
    {
        cJSON_AddStringToObject(root, "hostname", "");
    }
    char *json_str = cJSON_PrintUnformatted(root);
    if (json_str == NULL)
    {
        ret = 1;
        printf("Error: Failed to print cJSON object\n");
        cJSON_Delete(root);
        goto cleanup;
    }
    memcpy(return_json_data, json_str, strlen(json_str));
    free(json_str);
    cJSON_Delete(root);
    printf("Exiting parse_and_dispatch_json\n");

cleanup:
    return ret;
}

// 主解析函数
int parse_and_dispatch_json(const char *json_data, char *return_json_data)
{
    printf("Entering parse_and_dispatch_json\n");  // 函数入口
    printf("JSON data to parse: %s\n", json_data); // JSON解析前
    int ret = 0;
    char return_errstr[64] = {0};

    cJSON *root = cJSON_Parse(json_data);
    if (!root)
    {
        const char *error_ptr = cJSON_GetErrorPtr();
        if (error_ptr)
        {
            printf("Error before: %s\n", error_ptr);
        }
        printf("Exiting parse_and_dispatch_json due to JSON parsing error\n"); // 函数出口
        ret = 1;
        goto cleanup;
    }
    printf("cJSON parsed successfully\n"); // JSON解析后

    cJSON *cmd_name_item = cJSON_GetObjectItemCaseSensitive(root, "cmd_name");
    cJSON *cmd_args_item = cJSON_GetObjectItemCaseSensitive(root, "cmd_args");

    // 打印cmd_args_item
    printf("cmd_args_item: %s\n", cJSON_Print(cmd_args_item));

    if (cJSON_IsString(cmd_name_item) && cmd_name_item->valuestring != NULL && cJSON_IsObject(cmd_args_item))
    {
        printf("the current wifi_cmd_status is %d\r\n", get_wifi_cmd_status(NULL));
        char *cmd_name = cmd_name_item->valuestring;
        printf("Command name: %s\n", cmd_name); // 命令处理前
        if (strcmp(cmd_name, "wifi_get_list") == 0)
        {
            // printf("the current wifi_cmd_status is %d\r\n",get_wifi_cmd_status(NULL));
            if (WIFI_CMD_IDLE == get_wifi_cmd_status(NULL) || WIFI_CMD_SCANNING == get_wifi_cmd_status(NULL))
            {
                if (1 == g_wifi_scan_flag)
                {
                    ret = handle_wifi_get_list(cmd_name, cmd_args_item, return_json_data);
                    if (1 == ret)
                    {
                        memcpy(return_errstr, "WIFI_GET_LIST_CODE_CMD_ERROR", strlen("WIFI_GET_LIST_CODE_NO_WIFI_LIST"));
                    }
                    printf("wifi_get_list_handled\n"); // 命令处理后
                }
                else
                {
                    ret = 1;
                    memcpy(return_errstr, "WIFI_GET_LIST_CODE_NO_WIFI_SCAN", strlen("WIFI_GET_LIST_CODE_NO_WIFI_SCAN"));
                }
            }
            else if (WIFI_CMD_DEFAULT == get_wifi_cmd_status(NULL))
            {
                ret = 1;
                memcpy(return_errstr, "WIFI_SCAN_CODE_DEFAULT", strlen("WIFI_SCAN_CODE_DEFAULT"));
            }
            // else if (WIFI_CMD_SCANNING == get_wifi_cmd_status(NULL))
            // {
            //     ret = 1;
            //     memcpy(return_errstr, "WIFI_INFO_CODE_SCANNING", strlen("WIFI_INFO_CODE_SCANNING"));
            // }
            else if (WIFI_CMD_CONNECTING == get_wifi_cmd_status(NULL))
            {
                ret = 1;
                memcpy(return_errstr, "WIFI_INFO_CODE_CONNECTING", strlen("WIFI_INFO_CODE_CONNECTING"));
            }
            else if (WIFI_CMD_RECONNECTING == get_wifi_cmd_status(NULL))
            {
                ret = 1;
                memcpy(return_errstr, "WIFI_INFO_CODE_RECONNECTING", strlen("WIFI_INFO_CODE_RECONNECTING"));
            }
        }
        else if (strcmp(cmd_name, "wifi_list_ready") == 0)
        {
            if (WIFI_CMD_IDLE == get_wifi_cmd_status(NULL) || WIFI_CMD_SCANNING == get_wifi_cmd_status(NULL))
            {
                if (1 == g_wifi_scan_flag)
                {
                    ret = handle_wifi_list_ready(cmd_name, cmd_args_item, return_json_data);
                    if (1 == ret)
                    {
                        memcpy(return_errstr, "WIFI_LIST_READY_CODE_CMD_ERROR", strlen("WIFI_LIST_READY_CODE_CMD_ERROR"));
                    }
                    printf("wifi_get_list_handled\n"); // 命令处理后
                }
                else
                {
                    ret = 1;
                    memcpy(return_errstr, "WIFI_LIST_READY_CODE_LIST_NOT", strlen("WIFI_LIST_READY_CODE_LIST_NOT"));
                }
            }
            else if (WIFI_CMD_DEFAULT == get_wifi_cmd_status(NULL))
            {
                ret = 1;
                memcpy(return_errstr, "WIFI_LIST_READY_CODE_DEFAULT", strlen("WIFI_LIST_READY_CODE_DEFAULT"));
            }
            // else if (WIFI_CMD_SCANNING == get_wifi_cmd_status(NULL))
            // {
            //     ret = 1;
            //     memcpy(return_errstr, "WIFI_LIST_READY_CODE_SCANNING", strlen("WIFI_LIST_READY_CODE_SCANNING"));
            // }
            else if (WIFI_CMD_CONNECTING == get_wifi_cmd_status(NULL))
            {
                ret = 1;
                memcpy(return_errstr, "WIFI_LIST_READY_CODE_CONNECTING", strlen("WIFI_LIST_READY_CODE_CONNECTING"));
            }
            else if (WIFI_CMD_RECONNECTING == get_wifi_cmd_status(NULL))
            {
                ret = 1;
                memcpy(return_errstr, "WIFI_LIST_READY_CODE_RECONNECTING", strlen("WIFI_LIST_READY_CODE_RECONNECTING"));
            }
        }
        else if (strcmp(cmd_name, "wifi_scan") == 0)
        {
            printf("=====================the current wifi_manager_mode : %d========================\r\n", get_wifi_manager_mode(NULL));
            /* 只有WIFI_SMP_CFG_MODE模式下才能扫描*/
            // if (WIFI_CMD_IDLE == get_wifi_cmd_status(NULL) && WIFI_SMP_CFG_MODE == get_wifi_manager_mode(NULL))
            if (WIFI_CMD_IDLE == get_wifi_cmd_status(NULL))
            {
                ret = handle_wifi_scan(cmd_name, cmd_args_item, return_json_data);
                if (1 == ret)
                {
                    memcpy(return_errstr, "WIFI_SCAN_CODE_CMD_ERROR", strlen("WIFI_SCAN_CODE_CMD_ERROR"));
                }
                printf("wifi_scan_handled\n"); // 命令处理后
            }
            else if (WIFI_CMD_DEFAULT == get_wifi_cmd_status(NULL))
            {
                ret = 1;
                memcpy(return_errstr, "WIFI_SCAN_CODE_DEFAULT", strlen("WIFI_SCAN_CODE_DEFAULT"));
            }
            else if (WIFI_CMD_SCANNING == get_wifi_cmd_status(NULL))
            {
                ret = 1;
                memcpy(return_errstr, "WIFI_SCAN_CODE_SCANNING", strlen("WIFI_SCAN_CODE_SCANNING"));
            }
            else if (WIFI_CMD_RECONNECTING == get_wifi_cmd_status(NULL))
            {
                ret = 1;
                memcpy(return_errstr, "WIFI_SCAN_CODE_CONNECTING", strlen("WIFI_SCAN_CODE_CONNECTING"));
            }
            else if (WIFI_CMD_RECONNECTING == get_wifi_cmd_status(NULL))
            {
                ret = 1;
                memcpy(return_errstr, "WIFI_SCAN_CODE_RECONNECTING", strlen("WIFI_SCAN_CODE_RECONNECTING"));
            }
        }
        else if (strcmp(cmd_name, "wifi_stop_scan") == 0)
        {
            if (WIFI_CMD_SCANNING == get_wifi_cmd_status(NULL))
            {
                ret = handle_wifi_stop_scan(cmd_name, cmd_args_item, return_json_data);
                if (1 == ret)
                {
                    memcpy(return_errstr, "WIFI_STOP_SCAN_CODE_CMD_ERROR", strlen("WIFI_STOP_SCAN_CODE_CMD_ERROR"));
                }
                printf("wifi_stop_scan_handled\n"); // 命令处理后
            }
            else
            {
                ret = 1;
                memcpy(return_errstr, "WIFI_STOP_SCAN_CODE_NOT_SCANNING", strlen("WIFI_STOP_SCAN_CODE_NOT_SCANNING"));
            }
        }
        else if (strcmp(cmd_name, "wifi_connect") == 0)
        {
            if (WIFI_CMD_IDLE == get_wifi_cmd_status(NULL) || WIFI_CMD_SCANNING == get_wifi_cmd_status(NULL))
            {
                ret = handle_wifi_connect(cmd_name, cmd_args_item, return_json_data);
                if (1 == ret)
                {
                    memcpy(return_errstr, "WIFI_CONNECT_CODE_CMD_ERROR", strlen("WIFI_CONNECT_CODE_CMD_ERROR"));
                }
                printf("wifi_connect_handled\n"); // 命令处理后
            }
            else if (WIFI_CMD_DEFAULT == get_wifi_cmd_status(NULL))
            {
                ret = 1;
                memcpy(return_errstr, "WIFI_CONNECT_CODE_DEFAULT", strlen("WIFI_CONNECT_CODE_DEFAULT"));
            }
            // else if (WIFI_CMD_SCANNING == get_wifi_cmd_status(NULL))
            // {
            //     ret = 1;
            //     memcpy(return_errstr, "WIFI_CONNECT_CODE_SCANNING", strlen("WIFI_CONNECT_CODE_SCANNING"));
            // }
            else if (WIFI_CMD_CONNECTING == get_wifi_cmd_status(NULL))
            {
                ret = 1;
                memcpy(return_errstr, "WIFI_CONNECT_CODE_CONNECTING", strlen("WIFI_CONNECT_CODE_CONNECTING"));
            }
            else if (WIFI_CMD_RECONNECTING == get_wifi_cmd_status(NULL))
            {
                ret = 1;
                memcpy(return_errstr, "WIFI_CONNECT_CODE_RECONNECTING", strlen("WIFI_CONNECT_CODE_RECONNECTING"));
            }
        }
        else if (strcmp(cmd_name, "wifi_disconnect") == 0)
        {
            if (WIFI_CMD_IDLE == get_wifi_cmd_status(NULL))
            {
                if (WIFI_CONNECT_SUCC == get_wifi_connect_status(NULL))
                {
                    ret = handle_wifi_disconnect(cmd_name, cmd_args_item, return_json_data);
                    if (1 == ret)
                    {
                        memcpy(return_errstr, "WIFI_DISCONNECT_CODE_CMD_ERROR", strlen("WIFI_DISCONNECT_CODE_CMD_ERROR"));
                    }
                    printf("wifi_disconnect_handled\n"); // 命令处理后
                }
                else
                {
                    ret = 1;
                    memcpy(return_errstr, "WIFI_DISCONNECT_CODE_NOT_CONNECTED", strlen("WIFI_DISCONNECT_CODE_NOT_CONNECTED"));
                }
            }
            else if (WIFI_CMD_DEFAULT == get_wifi_cmd_status(NULL))
            {
                ret = 1;
                memcpy(return_errstr, "WIFI_DISCONNECT_CODE_DEFAULT", strlen("WIFI_DISCONNECT_CODE_DEFAULT"));
            }
            else if (WIFI_CMD_SCANNING == get_wifi_cmd_status(NULL))
            {
                ret = 1;
                memcpy(return_errstr, "WIFI_DISCONNECT_CODE_SCANNING", strlen("WIFI_DISCONNECT_CODE_SCANNING"));
            }
            else if (WIFI_CMD_CONNECTING == get_wifi_cmd_status(NULL))
            {
                ret = 1;
                memcpy(return_errstr, "WIFI_DISCONNECT_CODE_CONNECTING", strlen("WIFI_DISCONNECT_CODE_CONNECTING"));
            }
            else if (WIFI_CMD_RECONNECTING == get_wifi_cmd_status(NULL))
            {
                ret = 1;
                memcpy(return_errstr, "WIFI_DISCONNECT_CODE_RECONNECTING", strlen("WIFI_DISCONNECT_CODE_RECONNECTING"));
            }
        }
        else if (strcmp(cmd_name, "wifi_reconnect") == 0)
        {
            if (WIFI_CMD_IDLE == get_wifi_cmd_status(NULL))
            {
                ret = handle_wifi_reconnect(cmd_name, cmd_args_item, return_json_data);
                if (1 == ret)
                {
                    memcpy(return_errstr, "WIFI_RECONNECT_CODE_CMD_ERROR", strlen("WIFI_RECONNECT_CODE_CMD_ERROR"));
                }
                printf("wifi_reconnect_handled\n"); // 命令处理后
            }
            else if (WIFI_CMD_DEFAULT == get_wifi_cmd_status(NULL))
            {
                ret = 1;
                memcpy(return_errstr, "WIFI_RECONNECT_CODE_DEFAULT", strlen("WIFI_RECONNECT_CODE_DEFAULT"));
            }
            else if (WIFI_CMD_SCANNING == get_wifi_cmd_status(NULL))
            {
                ret = 1;
                memcpy(return_errstr, "WIFI_RECONNECT_CODE_SCANNING", strlen("WIFI_RECONNECT_CODE_SCANNING"));
            }
            else if (WIFI_CMD_CONNECTING == get_wifi_cmd_status(NULL))
            {
                ret = 1;
                memcpy(return_errstr, "WIFI_RECONNECT_CODE_CONNECTING", strlen("WIFI_RECONNECT_CODE_CONNECTING"));
            }
            else if (WIFI_CMD_RECONNECTING == get_wifi_cmd_status(NULL))
            {
                ret = 1;
                memcpy(return_errstr, "WIFI_RECONNECT_CODE_RECONNECTING", strlen("WIFI_RECONNECT_CODE_RECONNECTING"));
            }
        }
        else if (strcmp(cmd_name, "wifi_get_status") == 0)
        {
            if (WIFI_CMD_IDLE == get_wifi_cmd_status(NULL))
            {
                ret = handle_wifi_get_status(cmd_name, cmd_args_item, return_json_data);
                if (1 == ret)
                {
                    memcpy(return_errstr, "WIFI_GET_STATUS_CODE_CMD_ERROR", strlen("WIFI_GET_STATUS_CODE_CMD_ERROR"));
                }
                printf("wifi_get_status_handled\n"); // 命令处理后
            }
            else if (WIFI_CMD_DEFAULT == get_wifi_cmd_status(NULL))
            {
                ret = 1;
                memcpy(return_errstr, "WIFI_GET_STATUS_CODE_DEFAULT", strlen("WIFI_GET_STATUS_CODE_DEFAULT"));
            }
            else if (WIFI_CMD_SCANNING == get_wifi_cmd_status(NULL))
            {
                ret = 1;
                memcpy(return_errstr, "WIFI_GET_STATUS_CODE_SCANNING", strlen("WIFI_GET_STATUS_CODE_SCANNING"));
            }
            else if (WIFI_CMD_CONNECTING == get_wifi_cmd_status(NULL))
            {
                ret = 1;
                memcpy(return_errstr, "WIFI_GET_STATUS_CODE_CONNECTING", strlen("WIFI_GET_STATUS_CODE_CONNECTING"));
            }
            else if (WIFI_CMD_RECONNECTING == get_wifi_cmd_status(NULL))
            {
                ret = 1;
                memcpy(return_errstr, "WIFI_GET_STATUS_CODE_RECONNECTING", strlen("WIFI_GET_STATUS_CODE_RECONNECTING"));
            }
        }
        else if (strcmp(cmd_name, "wifi_info") == 0)
        {
            if (WIFI_CMD_IDLE == get_wifi_cmd_status(NULL))
            {
                if (WIFI_CONNECT_SUCC == get_wifi_connect_status(NULL))
                {
                    ret = handle_wifi_info(cmd_name, cmd_args_item, return_json_data);
                    if (1 == ret)
                    {
                        memcpy(return_errstr, "WIFI_INFO_CODE_CMD_ERROR", strlen("WIFI_INFO_CODE_CMD_ERROR"));
                    }
                    printf("wifi_info_handled\n"); // 命令处理后
                }
                else
                {
                    ret = 1;
                    memcpy(return_errstr, "WIFI_INFO_CODE_WIFI_NOT_CONNECTED", strlen("WIFI_INFO_CODE_WIFI_NOT_CONNECTED"));
                }
            }
            else if (WIFI_CMD_DEFAULT == get_wifi_cmd_status(NULL))
            {
                ret = 1;
                memcpy(return_errstr, "WIFI_INFO_CODE_DEFAULT", strlen("WIFI_INFO_CODE_DEFAULT"));
            }
            else if (WIFI_CMD_SCANNING == get_wifi_cmd_status(NULL))
            {
                ret = 1;
                memcpy(return_errstr, "WIFI_INFO_CODE_SCANNING", strlen("WIFI_INFO_CODE_SCANNING"));
            }
            else if (WIFI_CMD_CONNECTING == get_wifi_cmd_status(NULL))
            {
                ret = 1;
                memcpy(return_errstr, "WIFI_INFO_CODE_CONNECTING", strlen("WIFI_INFO_CODE_CONNECTING"));
            }
            else if (WIFI_CMD_RECONNECTING == get_wifi_cmd_status(NULL))
            {
                ret = 1;
                memcpy(return_errstr, "WIFI_INFO_CODE_RECONNECTING", strlen("WIFI_INFO_CODE_RECONNECTING"));
            }
        }
        else
        {
            printf("Unknown command: %s\n", cmd_name);
        }
        if (1 == ret)
        {
            // 封装return_json_data
            cJSON *root = cJSON_CreateObject();
            if (root == NULL)
            {
                ret = 1;
                printf("Error: Failed to create cJSON object\n");
                goto cleanup;
            }
            cJSON_AddStringToObject(root, "cmd_name", cmd_name);
            cJSON_AddNumberToObject(root, "return_code", ret);
            cJSON_AddStringToObject(root, "return_errstr", return_errstr);

            char *json_str = cJSON_PrintUnformatted(root);
            if (json_str == NULL)
            {
                ret = 1;
                printf("Error: Failed to print cJSON object\n");
                cJSON_Delete(root);
                goto cleanup;
            }
            memcpy(return_json_data, json_str, strlen(json_str));
            free(json_str);
            cJSON_Delete(root);
            ret = 0;
        }
    }
    else
    {
        printf("Invalid JSON format\n");
        ret = 1;
    }
    cJSON_Delete(root);
    printf("Exiting parse_and_dispatch_json\n"); // 函数出口
cleanup:
    if (ret == 1)
    {
        memset(json_data, 0, strlen(json_data));
    }
    return ret;
}