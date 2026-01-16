#include "app_wifi_manage.h"
#include "system/includes.h"
#include "wifi/wifi_connect.h"
#include "os/os_api.h"
#include "app_lfs.h"
#include "lwip.h"
#include "le/att.h"
#include "stdlib.h" // 包含 qsort 和 abs 所需的头文件
#include "syscfg/syscfg_id.h"
#include "sys_time.h" // 用于 timer_get_ms()
#include <stdint.h> // 用于 uint32_t
#include <stdbool.h> // 用于 bool 类型
#include <string.h> // memcpy/memset/strcmp/strncpy


/*-------------------------------------------------本文件的 变量及其 所有函数的定义-----------------------------------*/
static OS_SEM g_disconnect_sem;                                                     // 断连函数(阻塞方式)所用到的sem
static OS_SEM g_block_connect_sem;                                                  // 连接函数(阻塞方式)所用到的sem
static CONNECT_FUNC_FLAG g_connect_func_flag = CONNECT_FUNC_BLOCK_DEFAULT;          // 连接函数(阻塞方式)的flag
static DISCONNECT_FUNC_FLAG g_disconnect_func_flag = DISCONNECT_FUNC_BLOCK_DEFAULT; // 断连函数(阻塞方式)的flag

static WIFI_MANAGER_MODE g_wifi_manager_mode = WIFI_IDLE_MODE;        // 当前wifi 模式
static WIFI_connect_STATUES g_wifi_connect_state = WIFI_CONNECT_IDLE; // 当前wifi 连接状态
static WIFI_INFO_STATUES g_wifi_info_state = WIFI_INFO_IDLE;          // 当前wifi 模块状态
static WIFI_SCAN_STATUES g_wifi_scan_state = WIFI_SCAN_STOP;          // 当前wifi 扫描状态
static WIFI_CMD_STATUES g_wifi_cmd_state = WIFI_CMD_DEFAULT;          // 当前命令状态

static struct wifi_scan_ssid_info g_all_wifi_info[STORE_WIFI_NUM_MAX] = {0};
static int g_all_wifi_info_num = 0;

/*-------------------------------------------------哈希表相关定义和函数-----------------------------------*/
// 哈希表节点结构（链式哈希）
typedef struct hash_node {
    int index;  // 在 g_all_wifi_info 数组中的索引
    struct hash_node *next;
} hash_node_t;

#define HASH_TABLE_SIZE 53  // 素数，接近50的2倍，减少冲突
static hash_node_t *wifi_hash_table[HASH_TABLE_SIZE] = {0};
static hash_node_t hash_nodes[STORE_WIFI_NUM_MAX];  // 节点池
static int hash_nodes_used = 0;

// 字符串哈希函数（djb2哈希算法）
static uint32_t hash_ssid(const char *ssid)
{
    uint32_t hash = 5381;
    const char *p = ssid;
    while (*p) {
        hash = ((hash << 5) + hash) + *p;  // hash * 33 + *p
        p++;
    }
    return hash % HASH_TABLE_SIZE;
}

// 初始化哈希表
static void init_wifi_hash_table(void)
{
    for (int i = 0; i < HASH_TABLE_SIZE; i++) {
        wifi_hash_table[i] = NULL;
    }
    hash_nodes_used = 0;
}

// 清理哈希表
static void clear_wifi_hash_table(void)
{
    for (int i = 0; i < HASH_TABLE_SIZE; i++) {
        wifi_hash_table[i] = NULL;
    }
    hash_nodes_used = 0;
}

// 在哈希表中查找SSID，返回数组索引，未找到返回-1
static int hash_table_find(const char *ssid)
{
    uint32_t hash_val = hash_ssid(ssid);
    hash_node_t *node = wifi_hash_table[hash_val];
    
    while (node != NULL) {
        if (strcmp(g_all_wifi_info[node->index].ssid, ssid) == 0) {
            return node->index;
        }
        node = node->next;
    }
    return -1;
}

// 在哈希表中添加SSID索引
static void hash_table_add(int index, const char *ssid)
{
    if (hash_nodes_used >= STORE_WIFI_NUM_MAX) {
        printf("[blee] Warning: hash nodes pool exhausted\n");
        return;
    }
    
    uint32_t hash_val = hash_ssid(ssid);
    hash_node_t *new_node = &hash_nodes[hash_nodes_used++];
    new_node->index = index;
    new_node->next = wifi_hash_table[hash_val];
    wifi_hash_table[hash_val] = new_node;
}

// 重建哈希表（在排序后调用，因为索引可能变化）
static void rebuild_wifi_hash_table(void)
{
    clear_wifi_hash_table();
    for (int i = 0; i < g_all_wifi_info_num; i++) {
        hash_table_add(i, g_all_wifi_info[i].ssid);
    }
}

static struct netif_info g_netip_info = {0};
// IPv4 string, max "255.255.255.255" (15 chars) + '\0' => 16
static char g_wifi_ip[WIFI_IP_LEN] = {0};

static app_wifi_info_t g_app_wifi_info = {0};

// 申明
int get_wifi_scan_ssid_info(struct wifi_scan_ssid_info **ssid_info);

// 状态相关的函数定义

WIFI_SCAN_STATUES set_wifi_scan_status(WIFI_SCAN_STATUES status)
{
    g_wifi_scan_state = status;
    return g_wifi_scan_state;
}
WIFI_SCAN_STATUES get_wifi_scan_status(void *pri)
{
    return g_wifi_scan_state;
}

WIFI_CMD_STATUES set_wifi_cmd_status(WIFI_CMD_STATUES status)
{
    g_wifi_cmd_state = status;
    return g_wifi_cmd_state;
}
WIFI_CMD_STATUES get_wifi_cmd_status(void *pri)
{
    return g_wifi_cmd_state;
}

WIFI_MANAGER_MODE set_wifi_manager_mode(WIFI_MANAGER_MODE mode)
{
    g_wifi_manager_mode = mode;
    return g_wifi_manager_mode;
}
WIFI_MANAGER_MODE get_wifi_manager_mode(void *pri)
{
    return g_wifi_manager_mode;
}

WIFI_connect_STATUES set_wifi_connect_status(WIFI_connect_STATUES status)
{
    g_wifi_connect_state = status;
    return g_wifi_connect_state;
}
WIFI_connect_STATUES get_wifi_connect_status(void *pri)
{
    return g_wifi_connect_state;
}
WIFI_INFO_STATUES set_wifi_info_status(WIFI_INFO_STATUES status)
{
    g_wifi_info_state = status;
    return g_wifi_info_state;
}
WIFI_INFO_STATUES get_wifi_info_status(void *pri)
{
    return g_wifi_info_state;
}

// 连接相关的函数定义
/*
 * 返回值：
 * WIFI_CONNECT_SUCC : 连接成功
 * WIFI_CONNECT_FAIL : 连接失败
 */
WIFI_connect_STATUES wifi_connect_block(char *ssid, char *pwd, int timeout_s)
{
    int ret = 0;
    if (g_connect_func_flag == CONNECT_FUNC_BLOCK_DEFAULT)
    {
        g_connect_func_flag = CONNECT_FUNC_BLOCK_RUNNING;
        wifi_set_sta_connect_timeout(timeout_s);
        wifi_enter_sta_mode(ssid, pwd);
        ret = block_connect_sem_pend(NULL); // 等待连接成功或者失败
        printf("the ret = ---------------------------------------------------------------------%d\r\n", ret);
        if (0 == ret)
        {
            g_connect_func_flag = CONNECT_FUNC_BLOCK_DEFAULT;
            return get_wifi_connect_status(NULL);
        }
        else
        {
            g_connect_func_flag = CONNECT_FUNC_BLOCK_DEFAULT;
            return -1;
        }
    }
    else if (g_connect_func_flag == CONNECT_FUNC_BLOCK_RUNNING)
    {
        printf("the wifi_sta_mode_connect_block is running\r\n");
        return -5;
    }
}

 /* 自动重连函数 */
WIFI_connect_STATUES wifi_auto_reconnect(void)
{
    int ret = 0;
    char auto_ssid[33] = {0};
    char auto_pwd[33] = {0};
    
    get_wifi_config(auto_ssid,auto_pwd);
    printf("the auto_ssid is %s     the auto_pwd is %s\r\n",auto_ssid,auto_pwd);

    return wifi_connect_block(auto_ssid,auto_pwd, 10);
}

WIFI_connect_STATUES wifi_connect(
    char *ssid,
    char *pwd,
    int timeout_s,
    wifi_connect_succ_cb connect_succ_func,
    wifi_connect_fail_cb connect_fail_func)
{
    int ret = 0;

    wifi_set_sta_connect_timeout(timeout_s);
    wifi_set_connect_sta_block(0); // 确保连接为非阻塞方式
    wifi_enter_sta_mode(ssid, pwd);
    register_wifi_connect_succ_func(connect_succ_func, NULL);
    register_wifi_connect_fail_func(connect_fail_func, NULL);
}
WIFI_connect_STATUES wifi_disconnect(void (*wifi_disconnect_cb)(void *pri))
{
    int ret = 0;
    wifi_enter_smp_cfg_mode();
    return 0;
}
WIFI_connect_STATUES wifi_disconnect_block(void *pri)
{
    int ret = 0;
    if (g_disconnect_func_flag == DISCONNECT_FUNC_BLOCK_DEFAULT)
    {
        struct wifi_mode_info info;
        wifi_enter_smp_cfg_mode();
        if (!disconnect_sem_pend(NULL))
        {
            wifi_get_mode_cur_info(&info);
            if (info.mode == SMP_CFG_MODE)
            {
                ret = WIFI_CONNECT_IDLE;
                return ret;
            }
        }
    }
    else if (g_disconnect_func_flag == DISCONNECT_FUNC_BLOCK_RUNNING)
    {
        printf("the wifi_sta_mode_disconnect_block is running\r\n");
        return -5;
    }
}

// WIFI_connect_STATUES wifi_reconnect_config(lfs_t *lfs, int count)
// {
//     change_wifi_retry_connect_count(lfs, count);
//     return 0;
// }


/* block connect 所需要的sem 相关的函数*/

int block_connect_sem_creat(void *pri)
{
    int ret = 0;
    if (os_sem_valid(&g_block_connect_sem)) {
        return 0;
    }
    ret = os_sem_create(&g_block_connect_sem, 0);
    if (0 != ret)
    {
        printf("the block_connect_sem_creat fail\r\n");
    }
    return ret;
}
int block_connect_sem_pend(void *pri)
{
    int ret = 0;
    if (!os_sem_valid(&g_block_connect_sem)) {
        printf("[wifi] g_block_connect_sem invalid, recreate\r\n");
        os_sem_create(&g_block_connect_sem, 0);
    }
    ret = os_sem_pend(&g_block_connect_sem, 0);
    if (0 != ret)
    {
        printf("the block_connect_sem_pend fail\r\n");
    }
    return ret;
}
int block_connect_sem_post(void *pri)
{
    int ret = 0;
    if (!os_sem_valid(&g_block_connect_sem)) {
        // 如果初始化顺序异常/被破坏，先尝试重建，避免内核对象被非法操作进一步损坏
        printf("[wifi] g_block_connect_sem invalid, recreate\r\n");
        os_sem_create(&g_block_connect_sem, 0);
    }
    ret = os_sem_post(&g_block_connect_sem);
    if (0 != ret)
    {
        printf("the block_connect_sem_post fail\r\n");
    }
    return ret;
}

/* disconnect 所需要的sem 相关的函数 */
int disconnect_sem_creat(void *pri)
{
    int ret = 0;
    if (os_sem_valid(&g_disconnect_sem)) {
        return 0;
    }
    ret = os_sem_create(&g_disconnect_sem, 0);
    if (0 != ret)
    {
        printf("the disconnect_sem_creat fail\r\n");
    }
    return ret;
}
int disconnect_sem_pend(void *pri)
{
    int ret = 0;
    if (!os_sem_valid(&g_disconnect_sem)) {
        printf("[wifi] g_disconnect_sem invalid, recreate\r\n");
        os_sem_create(&g_disconnect_sem, 0);
    }
    ret = os_sem_pend(&g_disconnect_sem, 0);
    if (0 != ret)
    {
        printf("the disconnect_sem_pend fail\r\n");
    }
    return ret;
}
int disconnect_sem_post(void *pri)
{
    int ret = 0;
    if (!os_sem_valid(&g_disconnect_sem)) {
        printf("[wifi] g_disconnect_sem invalid, recreate\r\n");
        os_sem_create(&g_disconnect_sem, 0);
    }
    ret = os_sem_post(&g_disconnect_sem);
    if (0 != ret)
    {
        printf("the disconnect_sem_post fail\r\n");
    }
    return ret;
}

/*扫描相关的函数定义*/
int wifi_scan_start(void)
{
    int ret = 0;
    ret = wifi_scan_req();
    return ret;
}
// void wifi_scan_stop(void *pri);
int get_wifi_scan_ssid_info(struct wifi_scan_ssid_info **ssid_info)
{
    printf("wifi_scan_ssid_info\r\n");
    struct wifi_scan_ssid_info *sta_ssid_info;
    u32 sta_ssid_num = 0;
    sta_ssid_info = wifi_get_scan_result(&sta_ssid_num);
    wifi_clear_scan_result();

    for (u32 i = 0; i < sta_ssid_num; i++)
    {
        printf("find scan ssid = [%s]\r\n", sta_ssid_info[i].ssid);
    }

    *ssid_info = sta_ssid_info;
    return sta_ssid_num;
}

// 比较函数，用于 qsort 按照 RSSI 值降序排序
static int compare_wifi_scan_ssid_info(const void *a, const void *b) {
    const struct wifi_scan_ssid_info *wifi_a = (const struct wifi_scan_ssid_info *)a;
    const struct wifi_scan_ssid_info *wifi_b = (const struct wifi_scan_ssid_info *)b;
    return wifi_b->rssi - wifi_a->rssi; // 降序排序
}

int store_wifi_scan_result(void *pri)
{
    int ret = 0;
    struct wifi_scan_ssid_info *temp_all_wifi_info = NULL;
    
    // 记录更新前的WiFi数量，用于检测变化
    int prev_count = g_all_wifi_info_num;
    bool has_update = false;  // 标记是否有WiFi信息被更新（RSSI变化等）
    bool has_new = false;     // 标记是否有新WiFi被添加
    
    printf("[blee] store_wifi_scan_result called, prev_count=%d\n", prev_count);
    
    // 如果哈希表为空（首次调用或清理后），初始化哈希表
    // 如果 prev_count > 0 但哈希表未同步，也需要重建
    if (prev_count == 0 || hash_nodes_used == 0) {
        init_wifi_hash_table();
        // 如果有已有数据，重建哈希表
        if (prev_count > 0) {
            rebuild_wifi_hash_table();
        }
    }
    
    ret = get_wifi_scan_ssid_info(&temp_all_wifi_info);
    printf("[blee] get_wifi_scan_ssid_info returned %d WiFi networks\n", ret);

    /*
        将temp_all_wifi_info 拷贝到g_all_wifi_info
        并使用哈希表快速查找，如果temp_all_wifi_info有g_all_wifi_info没有的ssid，则添加到g_all_wifi_info
    */
    for (int i = 0; i < ret; i++)
    {
        // 使用哈希表查找SSID（O(1)平均复杂度）
        int found_index = hash_table_find(temp_all_wifi_info[i].ssid);
        
        if (found_index >= 0)
        {
            // SSID已存在，检查是否有变化（RSSI变化或信道变化）
            int rssi_old = (int)g_all_wifi_info[found_index].rssi;
            int rssi_new = (int)temp_all_wifi_info[i].rssi;
            bool channel_changed = (temp_all_wifi_info[i].channel_number != g_all_wifi_info[found_index].channel_number);
            
            // 只要有RSSI变化或信道变化，就标记为更新
            if (rssi_new != rssi_old || channel_changed)
            {
                has_update = true;
                printf("[blee] WiFi %s updated: rssi %d->%d, channel changed=%d\n", 
                       temp_all_wifi_info[i].ssid, rssi_old, rssi_new, channel_changed ? 1 : 0);
            }
            // 更新信息
            memcpy(&g_all_wifi_info[found_index], &temp_all_wifi_info[i], sizeof(struct wifi_scan_ssid_info));
        }
        else
        {
            // 如果ssid不存在且数组未满，添加到g_all_wifi_info
            // 注意：不立即添加到哈希表，等排序后统一重建，避免索引变化问题
            if (g_all_wifi_info_num < STORE_WIFI_NUM_MAX)
            {
                memcpy(&g_all_wifi_info[g_all_wifi_info_num], &temp_all_wifi_info[i], sizeof(struct wifi_scan_ssid_info));
                g_all_wifi_info_num++; // 只增加新添加的SSID数量
                has_new = true;
                printf("[blee] New WiFi added: %s\n", temp_all_wifi_info[i].ssid);
            }
            else
            {
                // 处理数组已满的情况
                printf("[blee] Warning: g_all_wifi_info is full, cannot add new SSID: %s\n", temp_all_wifi_info[i].ssid);
            }
        }
    }
    // 对 g_all_wifi_info 数组按照 RSSI 值进行排序
    qsort(g_all_wifi_info, g_all_wifi_info_num, sizeof(struct wifi_scan_ssid_info), compare_wifi_scan_ssid_info);
    // 排序后重建哈希表（因为索引可能变化，包括新增项的索引）
    rebuild_wifi_hash_table();
    
    // 检测是否有变化：数量变化或内容更新
    bool has_changed = (prev_count != g_all_wifi_info_num) || has_update || has_new;
    printf("[blee] After processing: prev_count=%d, curr_count=%d, has_update=%d, has_new=%d, has_changed=%d\n", 
           prev_count, g_all_wifi_info_num, has_update, has_new, has_changed);
    
    // 如果列表有变化且正在扫描中，主动推送WiFi列表
    if (has_changed)
    {
        // 检查是否在扫描状态（同时检查命令状态和扫描状态）
        WIFI_CMD_STATUES cmd_status = get_wifi_cmd_status(NULL);
        WIFI_SCAN_STATUES scan_status = get_wifi_scan_status(NULL);
        printf("[blee] Checking cmd_status: %d (WIFI_CMD_SCANNING=%d), scan_status: %d (WIFI_SCAN_RUNNING=%d)\n", 
               cmd_status, WIFI_CMD_SCANNING, scan_status, WIFI_SCAN_RUNNING);
        
        // 如果命令状态是扫描中，或者扫描状态是运行中，都发送通知
        if (cmd_status == WIFI_CMD_SCANNING || scan_status == WIFI_SCAN_RUNNING)
        {
            extern int notify_app_wifi_list_update(void *pri);
            int notify_ret = notify_app_wifi_list_update(NULL);
            printf("[blee] WiFi list changed (prev:%d, curr:%d, update:%d, new:%d), notified app, notify_ret=%d\n", 
                   prev_count, g_all_wifi_info_num, has_update, has_new, notify_ret);
        }
        else
        {
            printf("[blee] WiFi list changed but not in scanning state (cmd=%d, scan=%d), skip notification\n", 
                   cmd_status, scan_status);
        }
    }
    else
    {
        printf("[blee] WiFi list not changed, skip notification\n");
    }
    
    ret = g_all_wifi_info_num;
    return ret;
}

/*清空SCAN到的 WiFi info*/
int clean_wifi_scan_result(void *pri)
{
    memset(g_all_wifi_info, 0, sizeof(g_all_wifi_info));
    g_all_wifi_info_num = 0;
    clear_wifi_hash_table();  // 清理哈希表
    printf("[blee] WiFi scan result cleared.\r\n"); /* 清除 计数器*/
    return 0;
}

/*存储 wifi ip 信息*/
void store_wifi_ip_info(void *pri)
{
    memset(&g_netip_info, 0, sizeof(g_netip_info));
    lwip_get_netif_info(1, &g_netip_info);
    u32 temp_ip = htonl(g_netip_info.ip);
    snprintf(g_wifi_ip, sizeof(g_wifi_ip), "%d.%d.%d.%d",
             (temp_ip >> 24) & 0xFF, // 第一字节
             (temp_ip >> 16) & 0xFF, // 第二字节
             (temp_ip >> 8) & 0xFF,  // 第三字节
             temp_ip & 0xFF          // 第四字节
    );
    printf("####################### wifi ip:%s #######################", g_wifi_ip);
    // TODO store
}

/*获取 wifi ip 信息*/
void get_wifi_ip_info(char *ip)
{
    // Caller should provide a buffer of at least WIFI_IP_LEN bytes.
    memcpy(ip, g_wifi_ip, sizeof(g_wifi_ip));
}

void get_wifi_scan_result(struct wifi_scan_ssid_info **ssid_info, int *num)
{
    *ssid_info = g_all_wifi_info;
    *num = g_all_wifi_info_num;
}

// 初始化wifi管理模块
int wifi_manage_init(void *pri)
{
    int ret = 0;
    ret = disconnect_sem_creat(NULL);
    if (0 != ret)
    {
        printf("the wifi_manage_init fail\r\n");
    }
    ret = block_connect_sem_creat(NULL);
    if (0 != ret)
    {
        printf("the wifi_manage_init fail\r\n");
    }

    return ret;
}




/*-------------------------------------------------wifi 交给线程池相关的task start----------------------------------*/

void wifi_connect_task(void *arg)
{
    wifi_connect_params_t *params = (wifi_connect_params_t *)arg;
    wifi_connect(
        params->ssid,
        params->pwd,
        params->timeout_s,
        params->connect_succ_func,
        params->connect_fail_func);
        free(params);
}
void wifi_disconnect_task(void *arg)
{
    wifi_disconnect(NULL);
}

/* -------------------------------------------------wifi 交给线程池相关的task end----------------------------------*/



/* ！！！！！！！！！！可能要加互斥锁 ！！！！！！！！！！*/
/* 设置app_wifi_info */
void set_app_wifi_info(app_wifi_info_t *info)
{
    if (info == NULL) {
        printf("Error: info is NULL in set_app_wifi_info\n");
        return;
    }
    memcpy(&g_app_wifi_info, info, sizeof(app_wifi_info_t));
}
/* 获取app_wifi_info */
void get_app_wifi_info(app_wifi_info_t *info)
{
    if (info == NULL) {
        printf("Error: info is NULL in get_app_wifi_info\n");
        return;
    }
    memcpy(info, &g_app_wifi_info, sizeof(app_wifi_info_t));
}

/* 删除已连接过的并且存储的WiFi ssid */
void app_del_stored_wifi_ssid(void)
{
    struct wifi_stored_sta_info app_wifi_stored_ssid[NETWORK_SSID_INFO_CNT] = {0};
    int ret = 0;
    ret = wifi_get_stored_sta_info(app_wifi_stored_ssid);
    log_i("app_del_stored_wifi_ssid: %d\n", ret);
    for (int i = 0; i < ret; i++)
    { 
        log_i("app_del_stored_wifi_ssid: %s\n", app_wifi_stored_ssid[i].ssid);
        wifi_del_stored_sta_info(app_wifi_stored_ssid[i].ssid);
    }
}