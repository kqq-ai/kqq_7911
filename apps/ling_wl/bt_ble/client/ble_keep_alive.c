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

static const uint8_t handshake_dummy_data[] = { 0x5A, 0xA5 };

// static bka_eend_dev_handshake_info_t g_eend_list[KEEP_ALIVE_DEVICE_MAX_SIZE];
static int g_eend_cnt = 0;

static pthread_mutex_t g_mutex;

/**
 * 只有当前没有正在进行的握手任务时，才能启动一个新的握手任务
 */
static OS_SEM g_conn_sem;   // 连接成功信号量
static OS_SEM g_eend_big_connected_sem;  // E端BLE Audio连接成功信号
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
 * @brief 生成命令ID
 * @param sn 设备序列号
 * @return 生成的命令ID
 */
static uint32_t generate_cmd_id(const char *sn) 
{
    ASSERT(strlen(sn) == SN_SIZE - 1);
    // 获取当前时间戳（低24位）
    uint32_t timestamp = (uint32_t)time(NULL) & 0x00FFFFFF;
    
    // 计算字符串简单哈希（取前8位）
    uint8_t str_hash = 0;
    size_t len = strlen(sn);
    for (size_t i = 0; i < len; i++) {
        str_hash = 31 * str_hash + sn[i];
    }
    
    // 组合时间戳和哈希
    return (timestamp << 8) | str_hash;
}

static inline void call_cmd_cb(bka_cmd_t *cmd, int code) {
    if (cmd->cb) cmd->cb(BKA_CMD_CODE_BUFFER_FULL, cmd->priv);
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

    int code = BKA_CMD_CODE_COMPL;
    /* 发送握手数据 */
    int ret = 0;
    if (has_cmd) { /* send cmd */
        if (cmd.data[0] == 0xA0 && cmd.data[1] == 0x01) {   /* 开机指令需要等待回复 */
            os_sem_set(&g_eend_big_connected_sem, 0);
            if (BLE_BUFFER_FULL == ble_simple_client_write_without_respond_send(NULL, write_handle, cmd.data, cmd.size)) {
                log_e("[BKA]Failed to send cmd, ble buffer full.");
                call_cmd_cb(&cmd, BKA_CMD_CODE_BUFFER_FULL);
            } else if (os_sem_pend(&g_eend_big_connected_sem, 2000) == OS_TIMEOUT) { /* 等待E端回复 */
                log_e("[BKA]Error E BIG connection timeout!!!");
                call_cmd_cb(&cmd, BKA_CMD_CODE_BIG_CONNECTION_TIMEOUT);
            } else {
                call_cmd_cb(&cmd, BKA_CMD_CODE_COMPL);
            }
        } else {    /* 其他指令不需要等待回复 */
            if (BLE_BUFFER_FULL == ble_simple_client_write_without_respond_send(NULL, write_handle, cmd.data, cmd.size)) {
                log_e("[BKA]Failed to send cmd, ble buffer full.");
                call_cmd_cb(&cmd, BKA_CMD_CODE_BUFFER_FULL);
            } else {
                mdelay(200); // 等待3个连接事件(connection interval: 60ms)
                call_cmd_cb(&cmd, BKA_CMD_CODE_COMPL);
            }
        }
    } else { /* send dummy */
        if (BLE_BUFFER_FULL == ble_simple_client_write_without_respond_send(NULL, write_handle, handshake_dummy_data, sizeof(handshake_dummy_data))) {
            log_e("[BKA]Failed to send cmd, ble buffer full.");
        } else {
            mdelay(200); // 等待3个连接事件(connection interval: 60ms)
        }
    }

    /* 断开连接 */
    ble_simple_client_disconnect();
    /* handshake completed */
    os_mutex_pend(&pdev_info->mutex, 0);
    pdev_info->handshake_flag = 1;
    os_mutex_post(&pdev_info->mutex);

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
        return;
    }

    s8 rssi = report_pt->rssi;
    
    uint8_t buffer[32];
    int r = ble_adv_report_get_data(report_pt->data, report_pt->length, HCI_EIR_DATATYPE_MANUFACTURER_SPECIFIC_DATA, buffer);
    char *sn = (char *)buffer + 2;
    sn[15] = '\0'; // 确保字符串以 null 结尾
    lwl_eend_dev_t *pdev = find_eend_dev_by_sn(sn);
    if (pdev == NULL) {
        pdev = add_new_eend_dev(sn);
    }

    os_mutex_pend(&pdev->handshake_info.mutex, 0);

    if (pdev->handshake_info.handshake_flag == 1) { /* 已经握手过了 */
        os_mutex_post(&pdev->handshake_info.mutex);
        return;
    }
    
    /* 更新状态 */
    pdev->handshake_info.alive = 1;
    pdev->handshake_info.alive_hs_fail_cnt = 0;
    pdev->handshake_info.address_type = report_pt->address_type;
    memcpy(pdev->handshake_info.address, report_pt->address, 6);

    if (!g_handshake_is_ongoing) {
        /* 如果没有正在进行的握手任务，就启动一次握手流程 */
        start_handshake(&pdev->handshake_info);
    }
    os_mutex_post(&pdev->handshake_info.mutex);
}

static void ble_client_report_data_event_handler(att_data_report_t *report_data, void *priv)
{
    /* log_info("\n-report_data:type %02x,handle %04x,offset %d,len %d:",report_data->packet_type, */
    /* 		report_data->value_handle,report_data->value_offset,report_data->blob_length); */
    log_i_buf(report_data->blob, report_data->blob_length);

    uint8_t *data = report_data->blob;
    uint16_t len = report_data->blob_length;

    if (len == 8 && data[0] == 0xA0 && data[1] == 0x01) {
        log_i("E BIG connected!");
        os_sem_post(&g_eend_big_connected_sem);
    }
}

static void ble_keep_alive_do_start_handshake(lwl_eend_dev_t *pdev, void *priv)
{
    char sn_string[SN_SIZE * 2 + 1] = "(NULL)";
    os_mutex_pend(&pdev->handshake_info.mutex, 0);

    if (pdev->handshake_info.handshake_flag == 0) {
        /* 上一轮没有握手成功 */
        pdev->get_string(pdev, sn_string, sizeof(sn_string));
        log_i("[BKA]%s handshake failed.", sn_string);

        pdev->handshake_info.alive_hs_fail_cnt++;
        if (pdev->handshake_info.alive_hs_fail_cnt >= KEEP_ALIVE_HANDSHAKE_LOST_CNT) {
            /* 连续n轮没有握手成功判定为丢失 */
            pdev->handshake_info.alive = 0;

            /* 清空所有cmd */
            bka_cmd_t cmd;
            while (my_blocking_queue_try_dequeue(&pdev->handshake_info.cmd_buffer, &cmd) == MYERROR_Success) {
                if (cmd.cb) {
                    cmd.cb(BKA_CMD_CODE_DEVLOST, cmd.priv);
                }
            }
        }
    }
    /* 重置状态 */
    pdev->handshake_info.handshake_flag = 0;
    
    os_mutex_post(&pdev->handshake_info.mutex);
}

/**
 * @details
 * BLE SCAN 只要扫描到需要握手的设备，就会建立连接与其握手传指令
 * 此函数会检查当设备的握手状态，并重置设备的握手状态
 */
static void ble_keep_alive_start_handshake()
{
    eend_dev_traverse(ble_keep_alive_do_start_handshake, NULL);
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
    os_sem_create(&g_eend_big_connected_sem, 0);

    int r = thread_fork("ble_keep_alive_task", 15, 512 * 3, 0, 0, ble_keep_alive_task, NULL);
    ASSERT(r == 0);
    
    ble_conn_event_user_handler_add(ble_conn_event_handler, NULL);
    ble_disconn_event_user_handler_add(ble_disconn_event_handler, NULL);
    ble_adv_report_event_user_handler_add(ble_adv_report_event_handler, NULL);
    ble_client_report_data_event_user_handler_add(ble_client_report_data_event_handler, NULL);
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
    ASSERT(p_eend_dev != NULL);
    
    os_mutex_pend(&p_eend_dev->handshake_info.mutex, 0);
    int ret = BKACODE_Success;
    
    uint32_t cmd_id = generate_cmd_id((const char *)p_eend_dev->sn);

    /* 构造 cmd_pair */
    bka_cmd_t cmd_pair;
    memset(&cmd_pair, 0, sizeof(bka_cmd_t));
    memcpy(cmd_pair.data, cmd_buffer, size);
    cmd_pair.size = size;
    cmd_pair.cb = cb;
    cmd_pair.priv = priv;
    cmd_pair.cmd_id = cmd_id;

    if (my_blocking_queue_try_enqueue(&p_eend_dev->handshake_info.cmd_buffer, &cmd_pair) == MYERROR_Is_Full) {
        ret = BKACODE_Cmd_Queue_Full; /* 对应设备的 cmd_buffer 满了 */
    } else {
        *cmd_id_out = cmd_id;
        p_eend_dev->handshake_info.handshake_flag = 0;
    }

    os_mutex_post(&p_eend_dev->handshake_info.mutex);
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
    ASSERT(p_eend_dev != NULL);
    os_mutex_pend(&p_eend_dev->handshake_info.mutex, 0);
    int ret = BKACODE_Success;

    if (MYERROR_Not_Found == my_blocking_queue_try_remove_if(&p_eend_dev->handshake_info.cmd_buffer, 
            bka_device_remove_cmd_condition, (void*)cmd_id)) {
        ret = BKACODE_Cmd_Not_Found;
    }

    os_mutex_post(&p_eend_dev->handshake_info.mutex);
    return ret;
}
