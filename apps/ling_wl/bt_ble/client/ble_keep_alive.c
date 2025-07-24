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
static OS_SEM g_search_sem; // 搜索服务完毕信号量
static OS_SEM g_eend_turn_on_resp_sem;      // 接收E端开机指令回复信号
static uint8_t g_eend_turn_on_resp_status;   // E 端开机指令返回状态
bool g_handshake_is_ongoing = false;


// 0000fff0-0000-1000-8000-00805f9b34fb
// 0000fff3-0000-1000-8000-00805f9b34fb
// 0000fff4-0000-1000-8000-00805f9b34fb
const uint8_t uuid_server[]  = {0xfb, 0x34, 0x9b, 0x5f, 0x80, 0x00, 0x00, 0x80, 0x00, 0x10, 0x00, 0x00, 0xf0, 0xff, 0x00, 0x00};
const uint8_t uuid_char_tx[] = {0xfb, 0x34, 0x9b, 0x5f, 0x80, 0x00, 0x00, 0x80, 0x00, 0x10, 0x00, 0x00, 0xf3, 0xff, 0x00, 0x00};
const uint8_t uuid_char_rx[] = {0xfb, 0x34, 0x9b, 0x5f, 0x80, 0x00, 0x00, 0x80, 0x00, 0x10, 0x00, 0x00, 0xf4, 0xff, 0x00, 0x00};

const uint16_t notify_handle = 0x05;    // fixed GATT profile
const uint16_t write_handle =0x03;     // fixed GATT profile

/**
 * @brief 生成命令ID
 * @param sn 设备序列号
 * @return 生成的命令ID
 */
static uint32_t generate_cmd_id(const char *sn) 
{
    ASSERT(strlen(sn) == SN_SIZE - 1);
    // 获取当前时间戳（低24位）
    uint32_t timestamp = timer_get_ms() & 0x00FFFFFF;
    
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
    if (cmd->cb) cmd->cb(code, cmd->priv);
}

static int handshake_task_prepare(bool need_resp)
{
    if (need_resp) {
        /* 查询GATT服务 */
        os_sem_set(&g_search_sem, 0);
        ble_simple_client_profile_start();
        if (os_sem_pend(&g_search_sem, 1000) == OS_TIMEOUT) {
            log_e("[BKA]Error search server timeout!!!");
            return -1;
        }
        /* 订阅特征 */
        u16 tmp_16 = 0x01; //fixed
        if (ble_simple_client_write_send(NULL, notify_handle + 1, (u8 *)&tmp_16, 2)) {
            log_e("[BKA]Unexpected Error !!!");
            return -1;
        }
    }
    return 0;
}

static bka_cmd_code_e handshake_task_execute_cmd_normal(bka_cmd_t *cmd)
{
    /* 其他指令不需要等待回复 */
    log_i_buf(cmd->data, cmd->size);
    if (BLE_BUFFER_FULL == ble_simple_client_write_without_respond_send(NULL, write_handle, cmd->data, cmd->size)) {
        log_e("[BKA]Failed to send cmd, ble buffer full.");
        return BKA_CMD_CODE_BUFFER_FULL;
    }

    mdelay(300); // 等待3个连接事件(connection interval: 100ms)
    return BKA_CMD_CODE_COMPL;
}

static bka_cmd_code_e handshake_task_execute_cmd_turn_on(bka_cmd_t *cmd)
{
    /* 查询GATT服务 */
    os_sem_set(&g_search_sem, 0);
    ble_simple_client_profile_start();
    if (os_sem_pend(&g_search_sem, 1000) == OS_TIMEOUT) {
        log_e("[BKA]Error search server timeout!!!");
        return BKA_CMD_CODE_SEARCH_PROFILE_TIMEOUT;
    }

    /* 订阅特征 */
    u16 tmp_16 = 0x01; //fixed
    if (ble_simple_client_write_send(NULL, notify_handle + 1, (u8 *)&tmp_16, 2)) {
        log_e("[BKA]Unexpected Error !!!");
        return BKA_CMD_CODE_UNEXPECTED;
    }

    /* 开机指令需要等待回复 */
    os_sem_set(&g_eend_turn_on_resp_sem, 0);
    if (BLE_BUFFER_FULL == ble_simple_client_write_without_respond_send(NULL, write_handle, cmd->data, cmd->size)) {
        log_e("[BKA]Failed to send cmd, ble buffer full.");
        return BKA_CMD_CODE_BUFFER_FULL;
    }

    /* 等待E端回复 */
    if (os_sem_pend(&g_eend_turn_on_resp_sem, 2000) == OS_TIMEOUT) {
        log_e("[BKA]Error E BIG connection timeout!!!");
        return BKA_CMD_CODE_E_TURN_ON_TIMEOUT;
    }

    /* 根据E端回复做不同处理 */
    switch (g_eend_turn_on_resp_status) {
    case BKA_TURN_ON_SUCCESS:
        log_i("[BKA]E BIG connected!");
        return BKA_CMD_CODE_COMPL;
    case BKA_TURN_ON_FAILED_ALREADY_BOUND:
        log_i("[BKA]E is already bound by another C");
        return BKA_CMD_CODE_E_TURN_ON_ALREADY_BOUND;
    default:
        log_i("[BKA]Unexcepted turn on resp status: %02X", g_eend_turn_on_resp_status);
        return BKA_CMD_CODE_UNEXPECTED;
    }
}

static bka_cmd_code_e handshake_task_execute_cmd(bka_cmd_t *cmd)
{
    if (cmd->data[0] == 0xA0 && cmd->data[1] == 0x01) {
        log_i_buf(cmd->data, cmd->size);
        return handshake_task_execute_cmd_turn_on(cmd);
    }
    return handshake_task_execute_cmd_normal(cmd);
}

static void handshake_task_send_dummy()
{
    log_i_buf(handshake_dummy_data, sizeof(handshake_dummy_data));
    if (BLE_BUFFER_FULL == ble_simple_client_write_without_respond_send(NULL, write_handle, handshake_dummy_data, sizeof(handshake_dummy_data))) {
        log_e("[BKA]Failed to send cmd, ble buffer full.");
    } else {
        mdelay(300); // 等待3个连接事件(connection interval: 100ms)
    }
}

static void handshake_task(void *priv)
{
    int ret = 0;
    lwl_eend_dev_t *pdev = (lwl_eend_dev_t *)priv;
    ASSERT(pdev != NULL);
    bka_eend_dev_handshake_info_t *pdev_info = &pdev->handshake_info;
    ASSERT(pdev_info != NULL);
    os_mutex_pend(&pdev_info->mutex, 0);

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
    log_i("has cmd: %d %02X %02X", has_cmd, cmd.data[0], cmd.data[1]);

    bka_cmd_code_e code = BKA_CMD_CODE_COMPL;
    if (has_cmd) { /* send cmd */
        code = handshake_task_execute_cmd(&cmd);
        if (code == BKA_CMD_CODE_COMPL) {
            pdev->power_on_state = true;
            memcpy(pdev->bound_mac, lwl_app_core_context_get_instance()->mac, 6);
        }
        call_cmd_cb(&cmd, code);
    } else { /* send dummy */
        handshake_task_send_dummy();
    }

    /* 断开连接 */
    ble_simple_client_disconnect();

    /* 本次握手通信成功 */
    if (code == BKA_CMD_CODE_COMPL) {
        pdev_info->handshake_flag = 1;
        pdev_info->alive_hs_fail_cnt = 0;
    }

__finish:
    os_mutex_post(&pdev_info->mutex);
    g_handshake_is_ongoing = false;
}

void start_handshake(lwl_eend_dev_t *pdev)
{
    lwl_app_context_t *context = lwl_app_core_context_get_instance();
    if (!threadpool_add_task(context->threadpool, handshake_task, pdev)) {
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
    os_sem_post(&g_conn_sem);
}

static void ble_disconn_event_handler(uint8_t *packet)
{
    u16 handle = hci_event_disconnection_complete_get_connection_handle(packet);
    log_i("[BKA]ble_disconn_event_handler: %0x\n", handle);
}

static void ble_client_report_data_event_handler(att_data_report_t *report_data, void *priv)
{
    /* log_info("\n-report_data:type %02x,handle %04x,offset %d,len %d:",report_data->packet_type, */
    /* 		report_data->value_handle,report_data->value_offset,report_data->blob_length); */
    log_i_buf(report_data->blob, report_data->blob_length);

    uint8_t *data = report_data->blob;
    uint16_t len = report_data->blob_length;

    // 开机指令
    if (len == 3 && data[0] == 0xA0 && data[1] == 0x01) {
        g_eend_turn_on_resp_status = data[2];
        os_sem_post(&g_eend_turn_on_resp_sem);
    }
}

static void ble_search_completed_event_handler(void *priv)
{
    os_sem_post(&g_search_sem);
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
                call_cmd_cb(&cmd, BKA_CMD_CODE_DEVLOST);
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
        mdelay(10000);
    }
}

void ble_keep_alive_init(void)
{
    pthread_mutex_init(&g_mutex, NULL);
    os_sem_create(&g_conn_sem, 0);
    os_sem_create(&g_search_sem, 0);
    os_sem_create(&g_eend_turn_on_resp_sem, 0);

    int r = thread_fork("ble_keep_alive_task", 15, 512 * 3, 0, 0, ble_keep_alive_task, NULL);
    ASSERT(r == 0);
    
    ble_conn_event_user_handler_add(ble_conn_event_handler, NULL);
    ble_search_completed_event_user_handler_add(ble_search_completed_event_handler, NULL);
    ble_disconn_event_user_handler_add(ble_disconn_event_handler, NULL);

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
 * @return BKACODE_Success       : 指令添加成功
 *         BKACODE_Cmd_Queue_Full: 对应设备的控制指令队列已满
 * 
 * @note 执行cb时可能会持有部分锁
 *       - p_eend_dev->handshake_info.mutex
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

/**
 * @brief 往指定设备的控制指令队列中添加一条开机指令
 * 
 * @param[in]  p_eend_dev 设备引用
 * @param[in]  cb 函数指针，控制指令传输完成时的回调
 * @param[in]  priv 用户私有数据
 * @param[out] cmd_id_out 指令ID
 * 
 * @return BKACODE_Success       : 指令添加成功
 *         BKACODE_Cmd_Queue_Full: 对应设备的控制指令队列已满
 * 
 * @note 执行cb时可能会持有部分锁
 *       - p_eend_dev->handshake_info.mutex
 */
int bka_e_turn_on(lwl_eend_dev_t *p_eend_dev, bka_cmd_cb_funp_t cb, void *priv, uint32_t *cmd_id_out) 
{
    ASSERT(p_eend_dev != NULL);
    lwl_app_context_t *ctx = lwl_app_core_context_get_instance();
    p_eend_dev->handshake_info.need_handshake=true; // 设置握手标志
    uart_send_set_sn(p_eend_dev->sn);

    u8 cmd_turn_on[] = {0xA0, 0x01, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    memcpy(cmd_turn_on + 2, ctx->mac, 6);

    return ble_keep_alive_device_push_cmd(p_eend_dev, cmd_turn_on, sizeof(cmd_turn_on), cb, priv, cmd_id_out);
}

/**
 * @brief 往指定设备的控制指令队列中添加一条关机指令
 * 
 * @param[in]  p_eend_dev 设备引用
 * @param[in]  cb 函数指针，控制指令传输完成时的回调
 * @param[in]  priv 用户私有数据
 * @param[out] cmd_id_out 指令ID
 * 
 * @return BKACODE_Success       : 指令添加成功
 *         BKACODE_Cmd_Queue_Full: 对应设备的控制指令队列已满
 * 
 * @note 执行cb时可能会持有部分锁
 *       - p_eend_dev->handshake_info.mutex
 */
int bka_e_turn_off(lwl_eend_dev_t *p_eend_dev, bka_cmd_cb_funp_t cb, void *priv, uint32_t *cmd_id_out) 
{
    ASSERT(p_eend_dev != NULL);
    uart_send_set_sn("000000000000000");
    const u8 cmd_turn_off[] = {0xA0, 0x00};
    return ble_keep_alive_device_push_cmd(p_eend_dev, cmd_turn_off, sizeof(cmd_turn_off), cb, priv, cmd_id_out);
}

/**
 * @brief 删除指定设备的控制指令队列中的指定指令
 * 
 * @param[in] p_eend_dev 指定设备引用
 * @param[in] cmd_id 指定指令ID
 * 
 * @return BKACODE_Success : 删除成功
 *         BKACODE_Cmd_Not_Found : 设备的控制指令队列中未找到指定指令ID的指令
 */
int bka_cmd_remove(lwl_eend_dev_t *p_eend_dev, uint32_t cmd_id) 
{
    return ble_keep_alive_device_remove_cmd(p_eend_dev, cmd_id);
}


