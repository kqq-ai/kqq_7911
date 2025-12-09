#include "ble_dev_task_scheduler.h"

#include "app_mode/lwl_app_core.h"
#include "app_mode/lwl_eend_device.h"

#include "btstack/le/ble_api.h"
#include "btstack/btstack_event.h"
#include "btstack/le/le_common_define.h"
#include "ble_simple_client.h"

#include "task.h"

#include "util/cqueue.h"
#include "protocol/cp/c_uart_p.h"

#define BLE_DEV_TASK_SCHEDULER_OP_QUEUE_CAPACITY 100

#define SET_SCAN_INTERVAL   ADV_SCAN_MS(24) // unit: ms
#define SET_SCAN_WINDOW     ADV_SCAN_MS(8) // unit: ms

#define SET_SCAN_HIGH_DUTY_INTERVAL   ADV_SCAN_MS(24) // unit: ms
#define SET_SCAN_HIGH_DUTY_WINDOW     ADV_SCAN_MS(16) // unit: ms

#define BLE_DISCONNECT_TIMEOUT 500 // TODO

#define SET_PI_ADV_INTERVAL_MS 20 // unit: ms range: 20 ~ 1000
#define SET_PI_ADV_DURATION_S 600 // unit: s  range: 0 ~ 65535

// 0000fff0-0000-1000-8000-00805f9b34fb
// 0000fff3-0000-1000-8000-00805f9b34fb
// 0000fff4-0000-1000-8000-00805f9b34fb
const uint8_t uuid_server[]  = {0xfb, 0x34, 0x9b, 0x5f, 0x80, 0x00, 0x00, 0x80, 0x00, 0x10, 0x00, 0x00, 0xf0, 0xff, 0x00, 0x00};
const uint8_t uuid_char_tx[] = {0xfb, 0x34, 0x9b, 0x5f, 0x80, 0x00, 0x00, 0x80, 0x00, 0x10, 0x00, 0x00, 0xf3, 0xff, 0x00, 0x00};
const uint8_t uuid_char_rx[] = {0xfb, 0x34, 0x9b, 0x5f, 0x80, 0x00, 0x00, 0x80, 0x00, 0x10, 0x00, 0x00, 0xf4, 0xff, 0x00, 0x00};

const uint16_t notify_handle = 0x1A;    // fixed GATT profile
const uint16_t write_handle = 0x18;     // fixed GATT profile

static uint8_t active_data[9];

static OS_SEM g_conn_sem;    // 连接成功信号量
static OS_SEM g_disconn_sem; // 断连成功信号量
static OS_SEM g_e_connected_sem; // E 端连接成功信号量
static OS_SEM g_e_disconnected_sem; // E 端断连成功信号量
static MyBlockingQueue g_ble_dev_op_queue;

#define MS2TICK(ms) ((ms) / 10)

static inline void process_cb(ble_dev_op_t *op, u8 code) {
    if (op->cb) op->cb(code, op->cb_priv);
}

void notify_e_conncted()
{
    os_sem_post(&g_e_connected_sem);
}

void notify_e_disconncted()
{
    os_sem_post(&g_e_disconnected_sem);
}

const uint8_t empty_broadcast_code[6] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
void bdo_disable()
{
    log_i("[BDO]bdo_disable");
    uart_send_switch_audio_bc(empty_broadcast_code, 0x00);
    uart_send_set_ctrl(0);
}

static void ble_dev_op_handler_active(ble_dev_op_t* op)
{
    log_i("[BDO]Active start");
    /* - 查询当前设备状态，是否是一个 off stat，且有 broadcast code */
    lwl_eend_dev_t *dev = find_eend_dev_by_sn(op->sn);
    ASSERT(dev);
    u32 timestamp_now = lwl_rtc_timer_get_sec();
    if (dev->adv_state == BDPAS_OFF && (dev->has_deadline && dev->deadline >= timestamp_now + 10)) {
        dev->op_state &= ~BDO_STATE_ACTIVATING;
        process_cb(op, BDOCode_Skip);
        return;
    }

    /* - 建立连接 */
    log_i("[BDO]ble handshake_task %s\n", op->sn);
    os_sem_set(&g_conn_sem, 0);
    if (ble_simple_client_create_connection(op->active.address, op->active.address_type) == APP_BLE_OPERATION_ERROR) {
        log_e("[BDO]Error failed to create connection APP_BLE_OPERATION_ERROR!!!");
        dev->op_state &= ~BDO_STATE_ACTIVATING;
        process_cb(op, BDOCode_BLE_Api_Error);
        return;
    }
    if (os_sem_pend(&g_conn_sem, MS2TICK(op->active.conn_timeout)) == OS_TIMEOUT) {
        ble_simple_client_create_cannel();
        log_e("[BDO]Error ble connection timeout!!!");
        ble_simple_client_scan_enable(1, SET_SCAN_INTERVAL, SET_SCAN_WINDOW);
        dev->op_state &= ~BDO_STATE_ACTIVATING;
        process_cb(op, BDOCode_Connection_Timeout);
        return;
    }
    log_i("[BDO]ble connected %s Send deadline = %d\n", op->sn, timestamp_now);

    /* - 发送激活数据 */
    uint16_t quick_adv_interval = (SET_PI_ADV_INTERVAL_MS) * (1 / 0.625);
    uint16_t quick_adv_count = SET_PI_ADV_DURATION_S;

    active_data[0] = 0xA0;                          // fixed 0xA0 cmd
    active_data[1] = (quick_adv_interval) & 0xFF;
    active_data[2] = (quick_adv_interval >> 8) & 0xFF;
    active_data[3] = (quick_adv_count) & 0xFF;
    active_data[4] = (quick_adv_count >> 8) & 0xFF;
    active_data[5] = (timestamp_now) & 0xFF;
    active_data[6] = (timestamp_now >> 8) & 0xFF;
    active_data[7] = (timestamp_now >> 16) & 0xFF;
    active_data[8] = (timestamp_now >> 24) & 0xFF;

    log_i_buf(active_data, sizeof(active_data));
    if (BLE_BUFFER_FULL == ble_simple_client_write_without_respond_send(NULL, write_handle, active_data, sizeof(active_data))) {
        log_e("[BDO]Failed to send cmd, ble buffer full.");
        dev->op_state &= ~BDO_STATE_ACTIVATING;
        process_cb(op, BDOCode_BLE_Buffer_Full);
        return;
    } else {
        mdelay(120); // 等待3个连接事件(connection interval: 100ms)
    }

    /* - 关闭连接 */
    os_sem_set(&g_disconn_sem, 0);
    if (ble_simple_client_disconnect() == APP_BLE_OPERATION_ERROR) {
        log_e("[BDO]Error failed to disconnect APP_BLE_OPERATION_ERROR!!!");
        dev->op_state &= ~BDO_STATE_ACTIVATING;
        process_cb(op, BDOCode_BLE_Api_Error);
        return;
    }
    if (os_sem_pend(&g_disconn_sem, MS2TICK(BLE_DISCONNECT_TIMEOUT)) == OS_TIMEOUT) {
        log_e("[BDO]Error BLE disconnect timeout!!!");
        // dev->op_state &= ~BDO_STATE_ACTIVATING;
        // process_cb(op, BDOCode_Disconnect_Timeout);
        // return;
    }
    log_i("[BDO]ble disconnected");

    log_i("[BDO]Active end");
    dev->op_state &= ~BDO_STATE_ACTIVATING;
    process_cb(op, BDOCode_Success);
}

static void ble_dev_op_handler_turn_on(ble_dev_op_t* op)
{
    log_i("[BDO]Turn on %s", op->sn);

    lwl_eend_dev_t *dev = find_eend_dev_by_sn(op->sn);
    if (!dev) {
        log_i("[BDO]Error dev not found!");
        process_cb(op, BDOCode_Null_Pointer);
        return;
    }
    if (dev->distance_info.rssi_sample_count < 1) {
        log_i("[BDO]Error dev lost!");
        dev->op_state &= ~BDO_STATE_TURNNING_ON;
        process_cb(op, BDOCode_Dev_Busy);
        return;
    }
    // lwl_peri_dev_7016_on(true);  // 开启 7016
    // log_i("[BDO]ble_dev_op_handler_turn_on lwl_peri_dev_7016_on true");
    if (dev->adv_state == BDPAS_OFF) {

        /* - 建立连接 */
        log_i("[BDO]ble handshake_task %s\n", op->sn);
        os_sem_set(&g_conn_sem, 0);
        os_sem_set(&g_e_connected_sem, 0);
        if (ble_simple_client_create_connection(op->turnon.address, op->turnon.address_type) == APP_BLE_OPERATION_ERROR) {
            log_e("[BDO]Error failed to create connection APP_BLE_OPERATION_ERROR!!!");
            dev->op_state &= ~BDO_STATE_TURNNING_ON;
            process_cb(op, BDOCode_BLE_Api_Error);
            return;
        }
        if (os_sem_pend(&g_conn_sem, MS2TICK(op->turnon.conn_timeout)) == OS_TIMEOUT) {
            ble_simple_client_create_cannel();
            log_e("[BDO]Error ble connection timeout!!!");
            ble_simple_client_scan_enable(1, SET_SCAN_INTERVAL, SET_SCAN_WINDOW);
            dev->op_state &= ~BDO_STATE_TURNNING_ON;
            process_cb(op, BDOCode_Connection_Timeout);
            return;
        }
        log_i("[BDO]ble connected");

        /* - 关闭连接 */
        os_sem_set(&g_disconn_sem, 0);
        if (ble_simple_client_disconnect() == APP_BLE_OPERATION_ERROR) {
            log_e("[BDO]Error failed to disconnect APP_BLE_OPERATION_ERROR!!!");
            dev->op_state &= ~BDO_STATE_TURNNING_ON;
            process_cb(op, BDOCode_BLE_Api_Error);
            return;
        }
        if (os_sem_pend(&g_disconn_sem, MS2TICK(BLE_DISCONNECT_TIMEOUT)) == OS_TIMEOUT) {
            log_e("[BDO]Error BLE disconnect timeout!!!");
            // dev->op_state &= ~BDO_STATE_TURNNING_ON;
            // process_cb(op, BDOCode_Disconnect_Timeout);
            // return;
        }
        log_i("[BDO]ble disconnected");
    } else {
        // mdelay(500);
    }
    log_i("[BDO]ble_dev_op_handler_turn_on send switch audio bc");
    uart_send_switch_audio_bc(op->turnon.broadcast_code, 0x01);
    
    dev->set_ctrl(dev, 1, 1);

#if 0
    log_i("[BDO]Waiting adv state change...");
    ble_simple_client_scan_enable(1, SET_SCAN_HIGH_DUTY_INTERVAL, SET_SCAN_HIGH_DUTY_WINDOW);
    /* - 扫描设备 ADV 状态变化 -> connected */
    if (os_sem_pend(&g_e_connected_sem, MS2TICK(op->turnon.scan_timeout)) == OS_TIMEOUT) {
        log_e("[BDO]Error scan adv state timeout!!!");
        ble_simple_client_scan_enable(1, SET_SCAN_INTERVAL, SET_SCAN_WINDOW);
        dev->op_state &= ~BDO_STATE_TURNNING_ON;
        process_cb(op, BDOCode_Scan_Adv_Stat_Timeout);
        return;
    }
#endif

    log_i("[BDO]Turn on end");
    dev->op_state &= ~BDO_STATE_TURNNING_ON;
    process_cb(op, BDOCode_Success);
}

static void ble_dev_op_handler_turn_off(ble_dev_op_t* op)
{
    log_i("[BDO]Turn off %s", op->sn);

    lwl_eend_dev_t *dev = find_eend_dev_by_sn(op->sn);
    if (!dev) {
        log_i("[BDO]Error dev not found!");
        process_cb(op, BDOCode_Null_Pointer);
        return;
    }
    
    dev->set_ctrl(dev, 0, 0);

    os_sem_set(&g_e_disconnected_sem, 0);
    ble_simple_client_scan_enable(1, SET_SCAN_HIGH_DUTY_INTERVAL, SET_SCAN_HIGH_DUTY_WINDOW);
    /* - 扫描设备 ADV 状态变化 -> off */
    if (os_sem_pend(&g_e_disconnected_sem, MS2TICK(op->turnoff.scan_timeout)) == OS_TIMEOUT) {
        log_e("[BDO]Error scan adv state timeout!!!");
        ble_simple_client_scan_enable(1, SET_SCAN_INTERVAL, SET_SCAN_WINDOW);
        dev->op_state &= ~BDO_STATE_TURNNING_OFF;
        process_cb(op, BDOCode_Scan_Adv_Stat_Timeout);
        bdo_disable();
        return;
    }
    bdo_disable();

    log_i("[BDO]Turn off end");
    dev->op_state &= ~BDO_STATE_TURNNING_OFF;
    process_cb(op, BDOCode_Success);
}

static ble_dev_op_handler_func_t g_op_handler_entries[] = {
    [BDPDOT_ACTIVE] = ble_dev_op_handler_active,
    [BDPDOT_TURNON] = ble_dev_op_handler_turn_on,
    [BDPDOT_TURNOFF] = ble_dev_op_handler_turn_off,
};
static const int g_op_handler_entries_count = sizeof(g_op_handler_entries) / sizeof(g_op_handler_entries[0]);

static void ble_conn_event_handler(uint8_t *packet) {
    log_i("[BDO]BLE connnected\n");
    os_sem_post(&g_conn_sem);
}

static void ble_disconn_event_handler(uint8_t *packet) {
    log_i("[BDO]BLE disconnnected\n");
    os_sem_post(&g_disconn_sem);
}

static void ble_dev_task_scheduler()
{
    ASSERT(my_blocking_queue_init(&g_ble_dev_op_queue, sizeof(ble_dev_op_t), BLE_DEV_TASK_SCHEDULER_OP_QUEUE_CAPACITY) == 0);
    
    os_sem_create(&g_conn_sem, 0);
    os_sem_create(&g_disconn_sem, 0);
    os_sem_create(&g_e_connected_sem, 0);
    os_sem_create(&g_e_disconnected_sem, 0);
    ble_conn_event_user_handler_add(ble_conn_event_handler, NULL);
    ble_disconn_event_user_handler_add(ble_disconn_event_handler, NULL);

    ble_dev_op_t op;

    log_i("ble_dev_task_scheduler");

    while (true) {
        my_blocking_queue_dequeue(&g_ble_dev_op_queue, &op);
        ASSERT(op.op_type < g_op_handler_entries_count);

        if (g_op_handler_entries[op.op_type] != NULL) {
            g_op_handler_entries[op.op_type](&op);
        }
    }
    my_blocking_queue_destroy(&g_ble_dev_op_queue);
}

void ble_dev_task_scheduler_init()
{
    if (thread_fork("ble_dev_task_scheduler", 15, 512, 0, 0, ble_dev_task_scheduler, NULL)) {
        log_e("Error: Failed to create task ble_dev_task_scheduler.");
    }
}

int bdo_active(lwl_eend_dev_t *dev, int timeout, ble_dev_op_cb_func_t cb, void *priv)
{
    log_i("[MEASDIST]Active %s", dev->sn);
    if (dev->op_state & BDO_STATE_ACTIVATING)
        return 0;
    dev->op_state |= BDO_STATE_ACTIVATING;

    ble_dev_op_t op;
    op.op_type = BDPDOT_ACTIVE;
    memcpy(op.sn, dev->sn, sizeof(op.sn));
    op.active.conn_timeout = timeout;
    op.active.address_type = dev->address_type;
    memcpy(op.active.address, dev->address, sizeof(op.active.address));
    op.cb = cb;
    op.cb_priv = priv;
    return my_blocking_queue_try_enqueue(&g_ble_dev_op_queue, &op) == MYERROR_Success;
}

int bdo_turn_on(lwl_eend_dev_t *dev, int conn_timeout, int scan_timeout, ble_dev_op_cb_func_t cb, void *priv)
{
    // log_i("[MEASDIST]bdo_turn_on %s", dev->sn);
    if (dev->op_state & BDO_STATE_TURNNING_ON)
        return 0;
    dev->op_state |= BDO_STATE_TURNNING_ON;

    ble_dev_op_t op;
    op.op_type = BDPDOT_TURNON;
    memcpy(op.sn, dev->sn, sizeof(op.sn));
    op.turnon.conn_timeout = conn_timeout;
    op.turnon.scan_timeout = scan_timeout;
    op.turnon.address_type = dev->address_type;
    memcpy(op.turnon.address, dev->address, sizeof(op.turnon.address));
    memcpy(op.turnon.broadcast_code, &dev->sn[10], sizeof(op.turnon.broadcast_code));
    log_i("[MEASDIST]bdo_turn_on %s(%s)", dev->sn, op.turnon.broadcast_code);
    op.cb = cb;
    op.cb_priv = priv;
    return my_blocking_queue_try_enqueue(&g_ble_dev_op_queue, &op) == MYERROR_Success;
}

int bdo_turn_off(lwl_eend_dev_t *dev, int timeout, ble_dev_op_cb_func_t cb, void *priv)
{
    log_i("[MEASDIST]bdo_turn_off %s", dev->sn);
    if (dev->op_state & BDO_STATE_TURNNING_OFF)
        return 0;
    dev->op_state |= BDO_STATE_TURNNING_OFF;
    dev->set_ctrl(dev, 0, 0);

    ble_dev_op_t op;
    op.op_type = BDPDOT_TURNOFF;
    memcpy(op.sn, dev->sn, sizeof(op.sn));
    op.turnoff.scan_timeout = timeout;
    op.cb = cb;
    op.cb_priv = priv;
    return my_blocking_queue_try_enqueue(&g_ble_dev_op_queue, &op) == MYERROR_Success;
}
