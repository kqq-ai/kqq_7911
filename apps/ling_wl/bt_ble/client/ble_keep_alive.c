#include "ble_keep_alive.h"
#include "log.h"

#include "os/os_api.h"
#include "os/pthread.h"

#include "btstack/le/ble_api.h"
#include "btstack/btstack_event.h"
#include "btstack/le/le_common_define.h"
#include "ble_simple_client.h"

#include "app_mode/lwl_eend_device.h"

#include "util/cqueue.h"
#include "util/threadpool.h"
#include "app_mode/lwl_app_core.h"

#include "ble_att_util.h"

#define KEEP_ALIVE_CMD_DATA_SIZE 16
#define KEEP_ALIVE_HANDSHAKE_LOST_CNT 10

typedef struct {
    uint32_t cmd_id;
    uint8_t data[KEEP_ALIVE_CMD_DATA_SIZE];
    uint8_t size;
    bka_cmd_cb_funp_t cb;
    void *priv;
} bka_cmd_t;

#define KEEP_ALIVE_DEVICE_MAX_SIZE 3
#define KEEP_ALIVE_CMD_NUM 32

typedef struct {
    lwl_eend_dev_t *pdev;
    bool alive;
    bool handshake_flag;    // 握手标志，0表示没有握手，1表示已握手
    uint8_t alive_hs_fail_cnt;  // 保活判断中，连续握手失败的次数
    uint8_t address_type;  //冗余 用于连接 对方地址类型：0--public address,1--random address 
    uint8_t address[6];    //冗余 用于连接 peer_address 
    uint8_t cache_buf[(sizeof(bka_cmd_t) * KEEP_ALIVE_CMD_NUM)];
    MyBlockingQueue cmd_buffer; // queue<bka_cmd_t>
} bka_eend_dev_handshake_info_t;

static const uint8_t handshake_dummy_data[] = { 0x5A, 0xA5 };

static bka_eend_dev_handshake_info_t g_eend_list[KEEP_ALIVE_DEVICE_MAX_SIZE];
static int g_eend_cnt = 0;

static pthread_mutex_t g_mutex;

/**
 * 只有当前没有正在进行的握手任务时，才能启动一个新的握手任务
 */
static OS_SEM g_conn_sem;   // 连接成功信号量
static bool g_handshake_is_ongoing = false;


// 0000fff0-0000-1000-8000-00805f9b34fb
// 0000fff3-0000-1000-8000-00805f9b34fb
// 0000fff4-0000-1000-8000-00805f9b34fb
const uint8_t uuid_server[]  = {0xfb, 0x34, 0x9b, 0x5f, 0x80, 0x00, 0x00, 0x80, 0x00, 0x10, 0x00, 0x00, 0xf0, 0xff, 0x00, 0x00};
const uint8_t uuid_char_tx[] = {0xfb, 0x34, 0x9b, 0x5f, 0x80, 0x00, 0x00, 0x80, 0x00, 0x10, 0x00, 0x00, 0xf3, 0xff, 0x00, 0x00};
const uint8_t uuid_char_rx[] = {0xfb, 0x34, 0x9b, 0x5f, 0x80, 0x00, 0x00, 0x80, 0x00, 0x10, 0x00, 0x00, 0xf4, 0xff, 0x00, 0x00};

const uint16_t notify_handle = 0x20;    // fixed GATT profile
const uint16_t write_handle = 0x1E;     // fixed GATT profile

/**
 * @brief 生成32位唯一ID（24位时间戳 + 4位设备号 + 4位随机数）
 * @param device_id 设备号（取值范围0~15，超出部分会被截断）
 * @return 生成的32位ID
 */
static uint32_t generate_cmd_id(uint8_t device_id) 
{
    uint32_t timestamp = timer_get_ms() & 0x00FFFFFF;

    uint8_t random_buf;
    get_random_number(&random_buf, 1);
    uint8_t random_num = random_buf & 0x0F;

    uint8_t valid_device_id = device_id & 0x0F;

    uint32_t id = (timestamp << 8) | (valid_device_id << 4) | random_num;
    return id;
}

static void handshake_task(void *priv)
{
    bka_eend_dev_handshake_info_t *pdev_info = (bka_eend_dev_handshake_info_t *)priv;
    if (pdev_info == NULL) {
        log_e("[BKA]Error pdev_info is null!!!");
        goto __finish;
    }


    /* 建立连接 */
    os_sem_set(&g_conn_sem, 0);
    if (ble_simple_client_create_connection(pdev_info->address, pdev_info->address_type) == APP_BLE_OPERATION_ERROR) {
        log_e("[BKA]Error failed to create connection APP_BLE_OPERATION_ERROR!!!");
        goto __finish;
    }
    if (os_sem_pend(&g_conn_sem, 400) == OS_TIMEOUT) {
        ble_simple_client_create_cannel();
        log_e("[BKA]Error ble connection timeout!!!");
        ble_simple_client_scan_enable(1);
        goto __finish;
    }
    log_i("[BKA]ble connected\n");

    bool has_cmd = false;
    bka_cmd_t cmd;
    if (my_blocking_queue_try_dequeue(&pdev_info->cmd_buffer, &cmd) == MYERROR_Success) {
        has_cmd = true;
    }

    /* 发送握手数据 */
    int ret = 0;
    if (has_cmd) { /* send cmd */
        ret = ble_simple_client_write_without_respond_send(NULL, write_handle, cmd.data, cmd.size);
    } else { /* send dummy */
        ret = ble_simple_client_write_without_respond_send(NULL, write_handle, handshake_dummy_data, sizeof(handshake_dummy_data));
    }
    if (ret == BLE_BUFFER_FULL) {
        log_i("[BKA]Failed to send cmd, ble buffer full.");
    }
    mdelay(200); // connection interval: 60ms

    /* 执行指令完成回调 */
    if (has_cmd) {
        if (cmd.cb) {
            cmd.cb(BKA_CMD_CODE_COMPL, cmd.priv);
        }
    }

    /* 断开连接 */
    ble_simple_client_disconnect();
    /* handshake completed */
    pdev_info->handshake_flag = 1;

__finish:
    g_handshake_is_ongoing = false;
}

static void start_handshake(bka_eend_dev_handshake_info_t *pdev_info)
{
    lwl_app_context_t *context = lwl_app_core_context_get_instance();
    if (!threadpool_add_task(context->threadpool, handshake_task, pdev_info)) {
        log_e("[BKA]Failed to add task: handshake_task\n");
        return;
    } 
    g_handshake_is_ongoing = true;
}

static void ble_conn_event_handler(uint8_t *packet)
{
    u16 handle = INVALID_HANDLE;
    bd_addr_t addr;
    uint8_t addr_type;
    switch (hci_event_le_meta_get_subevent_code(packet)) {
        case HCI_SUBEVENT_LE_ENHANCED_CONNECTION_COMPLETE:
            handle = hci_subevent_le_enhanced_connection_complete_get_connection_handle(packet);
            hci_subevent_le_enhanced_connection_complete_get_peer_addresss(packet, addr);
            addr_type = hci_subevent_le_enhanced_connection_complete_get_peer_address_type(packet);
            break;
        case HCI_SUBEVENT_LE_CONNECTION_COMPLETE:
            handle = hci_subevent_le_connection_complete_get_connection_handle(packet);
            hci_subevent_le_connection_complete_get_peer_address(packet, addr);
            addr_type = hci_subevent_le_connection_complete_get_peer_address_type(packet);
            break;
        default:
            log_e("unexpected le meta subevent code.");
            ASSERT(0);
            break;
    }
    
    // u8 tmp[2] = { 0x00, 0x01 };
    // log_info("write_ntf_ccc:%04x\n", notify_handle);
    // client_operation_send(notify_handle + 1, tmp, 2, ATT_OP_WRITE);

    os_sem_post(&g_conn_sem);
}

static void ble_disconn_event_handler(uint8_t *packet)
{
    u16 handle = hci_event_disconnection_complete_get_connection_handle(packet);
    log_i("[BKA]ble_conn_event_handler: %0x\n", handle);
}

static void ble_adv_report_event_handler(adv_report_t *report_pt, u16 len)
{
    if (!adv_pre_match_impl(report_pt->data, report_pt->length)) {
        /* 不是规定的设备 */
        return;
    }
    for (int i = 0; i < KEEP_ALIVE_DEVICE_MAX_SIZE; ++i) {
        if (g_eend_list[i].pdev == NULL) {
            /* 无效 */
            continue;
        }
        if (g_eend_list[i].handshake_flag == 1) {
            /* 本轮已握手过了 */
            continue;
        }
        if (!g_eend_list[i].pdev->adv_match(g_eend_list[i].pdev, report_pt->data, report_pt->length)) {
            /* 广播数据与设备不匹配 */
            continue;
        }
        g_eend_list[i].alive = 1;
        g_eend_list[i].alive_hs_fail_cnt = 0;
        g_eend_list[i].address_type = report_pt->address_type;
        memcpy(g_eend_list[i].address, report_pt->address, 6);

        if (!g_handshake_is_ongoing) {
            /* 如果没有正在进行的握手任务，就启动一次握手流程 */
            start_handshake(&g_eend_list[i]);
        }
        break;
    }
}

/**
 * @details
 * BLE SCAN 只要扫描到需要握手的设备，就会建立连接与其握手传指令
 * 此函数会检查当设备的握手状态，并重置设备的握手状态
 */
static void ble_keep_alive_start_handshake()
{
    char sn_string[SN_SIZE * 2 + 1] = "(NULL)";
    pthread_mutex_lock(&g_mutex); // 上锁
    for (int i = 0; i < KEEP_ALIVE_DEVICE_MAX_SIZE; ++i) {
        if (g_eend_list[i].pdev == NULL) {
            /* 无效 */
            continue;
        }
        if (g_eend_list[i].handshake_flag == 0) {
            /* 上一轮没有握手成功 */
            g_eend_list[i].pdev->get_string(g_eend_list[i].pdev, sn_string, sizeof(sn_string));
            log_i("[BKA]%s handshake failed.", sn_string);

            g_eend_list[i].alive_hs_fail_cnt++;
            if (g_eend_list[i].alive_hs_fail_cnt >= KEEP_ALIVE_HANDSHAKE_LOST_CNT) {
                /* 连续n轮没有握手成功判定为丢失 */
                g_eend_list[i].alive = 0;

                /* 清空所有cmd */
                bka_cmd_t cmd;
                while (my_blocking_queue_try_dequeue(&g_eend_list[i].cmd_buffer, &cmd) == MYERROR_Success) {
                    if (cmd.cb) {
                        cmd.cb(BKA_CMD_CODE_DEVLOST, cmd.priv);
                    }
                }
            }
        }
        /* 重置状态 */
        g_eend_list[i].handshake_flag = 0;
    }
    pthread_mutex_unlock(&g_mutex); // 解锁
}

static void ble_keep_alive_task(void *priv)
{
    while (1) {
        ble_keep_alive_start_handshake();
        mdelay(5000);
    }
}

void ble_keep_alive_init(void)
{
    pthread_mutex_init(&g_mutex, NULL);
    os_sem_create(&g_conn_sem, 0);

    memset(g_eend_list, 0, sizeof(g_eend_list));
    int r = thread_fork("ble_keep_alive_task", 15, 512 * 3, 0, 0, ble_keep_alive_task, NULL);
    ASSERT(r == 0);
    
    ble_conn_event_user_handler_add(ble_conn_event_handler, NULL);
    ble_disconn_event_user_handler_add(ble_disconn_event_handler, NULL);
    ble_adv_report_event_user_handler_add(ble_adv_report_event_handler, NULL);
}

/**
 * @brief 添加一个握手设备
 * @param[in] p_eend_dev 设备引用
 */
void ble_keep_alive_device_add(lwl_eend_dev_t *p_eend_dev)
{
    pthread_mutex_lock(&g_mutex); // 上锁
    int idx;
    for (idx = 0; idx < KEEP_ALIVE_DEVICE_MAX_SIZE; ++idx) {
        if (g_eend_list[idx].pdev == NULL) {
            break;
        }
    }
    if (idx == KEEP_ALIVE_DEVICE_MAX_SIZE) {
        log_i("[BKA]Failed to register device, Max size is %d.", KEEP_ALIVE_DEVICE_MAX_SIZE);
    } else {
        g_eend_list[idx].pdev = p_eend_dev;
        g_eend_list[idx].alive = 0;
        g_eend_list[idx].alive_hs_fail_cnt = 0;
        memset(&g_eend_list[idx].cache_buf, 0, (sizeof(bka_cmd_t) * KEEP_ALIVE_CMD_NUM));
        my_blocking_queue_init_static(&g_eend_list[idx].cmd_buffer, g_eend_list[idx].cache_buf, sizeof(bka_cmd_t), KEEP_ALIVE_CMD_NUM);
        g_eend_cnt++;
        log_i("[BKA]register device");
    }
    pthread_mutex_unlock(&g_mutex); // 解锁
}

/**
 * @brief 移除一个已添加的设备
 * @param[in] p_eend_dev 设备引用
 */
void ble_keep_alive_device_remove(lwl_eend_dev_t *p_eend_dev)
{
    pthread_mutex_lock(&g_mutex); // 上锁
    for (int i = 0; i < KEEP_ALIVE_DEVICE_MAX_SIZE; ++i) {
        if (g_eend_list[i].pdev == p_eend_dev) {
            /* 初始化 */
            memset(&g_eend_list[i], 0, sizeof(bka_eend_dev_handshake_info_t));
            g_eend_cnt--;
            break;
        }
    }
    pthread_mutex_unlock(&g_mutex); // 解锁
}


/**
 * 
 * @brief 往指定设备的控制指令队列中添加一条指令
 * 
 * @param[in]  p_eend_dev 设备引用
 * @param[in]  cmd_buffer 控制指令数据
 * @param[in]  size 控制指令数据长度
 * @param[in]  cb 函数指针，控制指令传输完成时的回调
 * @param[in]  priv 用户私有数据
 * @param[out] cmd_id_out 指令ID
 * 
 * @return BKACODE_Success       : 指令添加成功, 
 *         BKACODE_Not_Found     : 设备列表中未找到指定sn的设备, 
 *         BKACODE_Cmd_Queue_Full: 对应设备的控制指令队列已满
 */
int ble_keep_alive_device_push_cmd(lwl_eend_dev_t *p_eend_dev, uint8_t *cmd_buffer, uint32_t size, bka_cmd_cb_funp_t cb, void *priv, uint32_t *cmd_id_out) 
{
    ASSERT(size < KEEP_ALIVE_CMD_DATA_SIZE);
    pthread_mutex_lock(&g_mutex); // 上锁
    int ret = BKACODE_Success;
    
    /* 检索设备 */
    int idx;
    for (idx = 0; idx < KEEP_ALIVE_DEVICE_MAX_SIZE; ++idx) {
        if (g_eend_list[idx].pdev == p_eend_dev) {
            break;
        }
    }
    if (idx == KEEP_ALIVE_DEVICE_MAX_SIZE) {
        ret = BKACODE_Dev_Not_Found; /* sn没检索到 */
        goto final;
    }

    uint32_t cmd_id = generate_cmd_id(idx);

    /* 构造 cmd_pair */
    bka_cmd_t cmd_pair;
    memset(&cmd_pair, 0, sizeof(bka_cmd_t));
    memcpy(cmd_pair.data, cmd_buffer, size);
    cmd_pair.size = size;
    cmd_pair.cb = cb;
    cmd_pair.priv = priv;
    cmd_pair.cmd_id = cmd_id;

    if (my_blocking_queue_try_enqueue(&g_eend_list[idx].cmd_buffer, &cmd_pair) == MYERROR_Is_Full) {
        ret = BKACODE_Cmd_Queue_Full; /* 对应设备的 cmd_buffer 满了 */
        goto final;
    }

    *cmd_id_out = cmd_id;
    g_eend_list[idx].handshake_flag = 0;

final:
    pthread_mutex_unlock(&g_mutex); // 解锁
    return ret;
}

static int bka_device_remove_cmd_condition(void *pcmd_data, void *priv) {
    return ((bka_cmd_t *)pcmd_data)->cmd_id == (uint32_t)priv;
}

/**
 * @brief 删除指定设备的控制指令队列中的指定指令
 * 
 * @param[in] p_eend_dev 指定设备引用
 * @param[in] cmd_id 指定指令ID
 * 
 * @return BKACODE_Success : 删除成功
 *         BKACODE_Dev_Not_Found : 设备列表中未找到指定设备引用
 *         BKACODE_Cmd_Not_Found : 设备的控制指令队列中未找到指定指令ID的指令
 */
int ble_keep_alive_device_remove_cmd(lwl_eend_dev_t *p_eend_dev, uint32_t cmd_id)
{
    pthread_mutex_lock(&g_mutex); // 上锁
    int ret = BKACODE_Success;
    
    /* 检索设备 */
    int idx;
    for (idx = 0; idx < KEEP_ALIVE_DEVICE_MAX_SIZE; ++idx) {
        if (g_eend_list[idx].pdev == p_eend_dev) {
            break;
        }
    }
    if (idx == KEEP_ALIVE_DEVICE_MAX_SIZE) {
        ret = BKACODE_Dev_Not_Found; /* sn没检索到 */
        goto final;
    }

    if (MYERROR_Not_Found == 
        my_blocking_queue_try_remove_if(&g_eend_list[idx].cmd_buffer, bka_device_remove_cmd_condition, (void*)cmd_id)) {
        ret = BKACODE_Cmd_Not_Found;
        goto final;
    }

final:
    pthread_mutex_unlock(&g_mutex); // 解锁
    return ret;
}