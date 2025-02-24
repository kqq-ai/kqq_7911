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

#include "ble_att_util.h"

#define KEEP_ALIVE_CMD_DATA_SIZE 10

typedef struct {
    uint8_t data[KEEP_ALIVE_CMD_DATA_SIZE];
    uint8_t size;
    bka_cmd_cb_funp_t cb;
    void *priv;
} bka_cmd_t;

#define KEEP_ALIVE_DEVICE_MAX_SIZE 3
#define KEEP_ALIVE_CMD_NUM 10

typedef struct {
    lwl_eend_dev_t *pdev;
    bool alive;
    bool handshake_flag;    // 握手标志，0表示没有握手，1表示已握手
    uint8_t cache_buf[(sizeof(bka_cmd_t) * KEEP_ALIVE_CMD_NUM)];
    MyBlockingQueue cmd_buffer; // queue<bka_cmd_t>
} bka_eend_dev_handshake_info_t;

static const uint8_t handshake_dummy_data[] = { 0x5A, 0xA5 };

static bka_eend_dev_handshake_info_t g_eend_list[KEEP_ALIVE_DEVICE_MAX_SIZE];
static int g_eend_cnt = 0;
static int g_eend_current_idx = 0;

static pthread_mutex_t g_mutex;

extern threadpool_t *pool;

// 0000fff0-0000-1000-8000-00805f9b34fb
// 0000fff3-0000-1000-8000-00805f9b34fb
// 0000fff4-0000-1000-8000-00805f9b34fb
const uint8_t uuid_server[]  = {0xfb, 0x34, 0x9b, 0x5f, 0x80, 0x00, 0x00, 0x80, 0x00, 0x10, 0x00, 0x00, 0xf0, 0xff, 0x00, 0x00};
const uint8_t uuid_char_tx[] = {0xfb, 0x34, 0x9b, 0x5f, 0x80, 0x00, 0x00, 0x80, 0x00, 0x10, 0x00, 0x00, 0xf3, 0xff, 0x00, 0x00};
const uint8_t uuid_char_rx[] = {0xfb, 0x34, 0x9b, 0x5f, 0x80, 0x00, 0x00, 0x80, 0x00, 0x10, 0x00, 0x00, 0xf4, 0xff, 0x00, 0x00};

const uint16_t notify_handle = 0x20;    // fixed
const uint16_t write_handle = 0x1E;     // fixed

static void ble_disconnect_delay(void *priv)
{
    u16 handle = (u16)priv;
    bka_cmd_t cmd;
    if (my_blocking_queue_try_dequeue(&g_eend_list[g_eend_current_idx].cmd_buffer, &cmd) == MYERROR_Success) {
        /* send cmd */
        int ret = ble_simple_client_write_without_respond_send(NULL, write_handle, cmd.data, cmd.size);
        if (ret == BLE_BUFFER_FULL) {
            log_i("[BKA]Failed to send cmd, ble buffer full.");
        }
        mdelay(150); // connection interval: 60ms
        ble_simple_client_disconnect();
        /* cmd callback */
        if (cmd.cb) {
            cmd.cb(cmd.priv);
        }
        /* handshake completed */
        g_eend_list[g_eend_current_idx].handshake_flag = 1;
    } else {
        /* send dummy */
        int ret = ble_simple_client_write_without_respond_send(NULL, write_handle, handshake_dummy_data, sizeof(handshake_dummy_data));
        if (ret == BLE_BUFFER_FULL) {
            log_i("[BKA]Failed to send cmd, ble buffer full.");
        }
        mdelay(150); // connection interval: 60ms
        ble_simple_client_disconnect();
        /* handshake completed */
        g_eend_list[g_eend_current_idx].handshake_flag = 1;
    }
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
    log_i("[BKA]ble_conn_event_handler: %0x, type: %d\n", handle, addr_type);
    printf_buf(addr, 6);
    
    // u8 tmp[2] = { 0x00, 0x01 };
    // log_info("write_ntf_ccc:%04x\n", notify_handle);
    // client_operation_send(notify_handle + 1, tmp, 2, ATT_OP_WRITE);

    if (!threadpool_add_task(pool, ble_disconnect_delay, (void *)handle)) {
        log_e("[BKA]Failed to add task: ble_disconnect_delay\n");
    }
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
        log_i("[BKA]*********create_connection***********");
        g_eend_current_idx = i;
        ble_simple_client_create_connection(report_pt->address, report_pt->address_type);
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
 * @param[in] p_eend_dev 设备引用
 * @param[in] cmd_buffer 控制指令数据
 * @param[in] size 控制指令数据长度
 * @param[in] cb 函数指针，控制指令传输完成时的回调
 * @param[in] priv 用户私有数据
 * 
 * @return 0: 指令添加成功, 
 *         1: 设备列表中未找到指定sn的设备, 
 *         2: 对应设备的控制指令队列已满
 */
int ble_keep_alive_device_push_cmd(lwl_eend_dev_t *p_eend_dev, uint8_t *cmd_buffer, uint32_t size, bka_cmd_cb_funp_t cb, void *priv) 
{
    ASSERT(size < KEEP_ALIVE_CMD_DATA_SIZE);
    pthread_mutex_lock(&g_mutex); // 上锁
    int ret = 1;

    /* 构造 cmd_pair */
    bka_cmd_t cmd_pair;
    memset(&cmd_pair, 0, sizeof(bka_cmd_t));
    memcpy(cmd_pair.data, cmd_buffer, size);
    cmd_pair.size = size;
    cmd_pair.cb = cb;
    cmd_pair.priv = priv;

    /* 检索设备 */
    int idx;
    for (idx = 0; idx < KEEP_ALIVE_DEVICE_MAX_SIZE; ++idx) {
        if (g_eend_list[idx].pdev == p_eend_dev) {
            break;
        }
    }
    if (idx == KEEP_ALIVE_DEVICE_MAX_SIZE) {
        ret = 1; /* sn没检索到 */
    } else if (my_blocking_queue_try_enqueue(&g_eend_list[idx].cmd_buffer, &cmd_pair) == MYERROR_Is_Full) {
        ret = 2; /* 对应设备的 cmd_buffer 满了 */
    } else {
        ret = 0;
    }
    pthread_mutex_unlock(&g_mutex); // 解锁
    return ret;
}

