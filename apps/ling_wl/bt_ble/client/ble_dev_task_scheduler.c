/**
 * ============================================================================
 * 文件: ble_dev_task_scheduler.c
 * 功能: BLE 设备操作任务调度器
 * 描述: 管理 BLE 设备的异步操作(激活、打开、关闭)，采用任务队列模型统一处理
 * ============================================================================
 *
 * 【实现逻辑】
 * ------
 * 1. 任务队列模型:
 *    - 使用阻塞队列(MyBlockingQueue)存储待执行的BLE操作
 *    - 主任务线程(ble_dev_task_scheduler)循环从队列取出操作并执行
 *    - 用户通过 bdo_active/bdo_turn_on/bdo_turn_off 接口异步提交操作
 *
 * 2. 操作类型(3种):
 *    a) BDPDOT_ACTIVE   - 激活设备: 建立连接->发送激活数据->断连
 *    b) BDPDOT_TURNON   - 打开设备: 建立连接->断连->启用广播
 *    c) BDPDOT_TURNOFF  - 关闭设备: 禁用广播->扫描设备状态变化
 *
 * 3. 事件驱动同步机制:
 *    - 使用信号量(OS_SEM)实现BLE事件与操作的同步
 *    - g_conn_sem: BLE连接完成信号
 *    - g_disconn_sem: BLE断连完成信号
 *    - g_e_connected_sem: 端点设备连接(广播状态)信号
 *    - g_e_disconnected_sem: 端点设备断连(off状态)信号
 *
 * 4. 回调机制:
 *    - 每个操作关联一个回调函数(cb)和私有数据(cb_priv)
 *    - 操作完成或出错时调用回调，传递结果码(BDOCode_*)
 *
 * 【外部接口说明】
 * ------
 *
 * 【BLE 基础接口】(来源: btstack/le/ble_api.h, ble_simple_client.h)
 * -------
 * int ble_simple_client_create_connection(const uint8_t *addr, uint8_t addr_type)
 *     功能: 建立BLE连接
 *     参数: addr - 目标设备地址(6字节)
 *           addr_type - 地址类型(公有/随机)
 *     返回: APP_BLE_OPERATION_SUCCESS / APP_BLE_OPERATION_ERROR
 *     说明: 异步操作，完成时触发 ble_conn_event_handler
 *
 * int ble_simple_client_disconnect(void)
 *     功能: 断开BLE连接
 *     返回: APP_BLE_OPERATION_SUCCESS / APP_BLE_OPERATION_ERROR
 *     说明: 异步操作，完成时触发 ble_disconn_event_handler
 *
 * int ble_simple_client_create_cannel(void)
 *     功能: 取消BLE连接操作(在连接超时时使用)
 *     返回: 操作状态
 *
 * int ble_simple_client_write_without_respond_send(void *param, uint16_t handle,
 *                                                   uint8_t *data, uint16_t len)
 *     功能: 发送BLE特征值写入请求(无需响应)
 *     参数: handle - GATT特征值句柄(本文件固定为write_handle=0x18)
 *           data - 待发送数据
 *           len - 数据长度
 *     返回: BLE_BUFFER_FULL / 其他
 *
 * void ble_conn_event_user_handler_add(ble_callback_t handler, void *param)
 *     功能: 注册BLE连接事件处理器
 *     参数: handler - 事件回调函数指针
 *           param - 回调私有数据(可为NULL)
 *
 * void ble_disconn_event_user_handler_add(ble_callback_t handler, void *param)
 *     功能: 注册BLE断连事件处理器
 *
 * 【GATT 配置信息】
 * -------
 * UUID_SERVER (uuid_server): 0000fff0-0000-1000-8000-00805f9b34fb
 *     BLE服务UUID，用于发现目标设备
 *
 * UUID_CHAR_TX (uuid_char_tx): 0000fff3-0000-1000-8000-00805f9b34fb
 *     特征值UUID - TX(接收)特征
 *
 * UUID_CHAR_RX (uuid_char_rx): 0000fff4-0000-1000-8000-00805f9b34fb
 *     特征值UUID - RX(发送)特征
 *
 * GATT Handles (固定):
 *     - notify_handle (0x1A): 通知特征句柄
 *     - write_handle (0x18): 写入特征句柄
 *
 * 【设备管理接口】(来源: app_mode/lwl_eend_device.h)
 * -------
 * lwl_eend_dev_t *find_eend_dev_by_sn(const char *sn)
 *     功能: 通过序列号查找端点设备
 *     参数: sn - 设备序列号(16字节)
 *     返回: 设备结构体指针，未找到返回NULL
 *
 * 【系统接口】
 * -------
 * u32 lwl_rtc_timer_get_sec(void)
 *     功能: 获取当前RTC时间戳(秒)
 *     返回: 当前时间戳
 *
 * void mdelay(u32 ms)
 *     功能: 阻塞延迟
 *     参数: ms - 延迟毫秒数
 *
 * int thread_fork(const char *name, uint8_t priority, uint16_t stack_size,
 *                 uint32_t flag, uint32_t timeout, void (*func)(void *),
 *                 void *priv)
 *     功能: 创建新线程(来源: task.h)
 *     参数: name - 线程名称
 *           priority - 线程优先级(0-31, 数字越小优先级越高)
 *           stack_size - 堆栈大小(字节)
 *           flag - 线程标志
 *           timeout - 超时时间
 *           func - 线程入口函数
 *           priv - 入口函数私有参数
 *     返回: 0成功, 非0失败
 *
 * 【信号量接口】(来源: OS层)
 * -------
 * int os_sem_create(OS_SEM *sem, u32 value)
 *     功能: 创建信号量
 *     参数: value - 初始计数值
 *
 * int os_sem_set(OS_SEM *sem, u32 value)
 *     功能: 设置信号量值
 *
 * int os_sem_post(OS_SEM *sem)
 *     功能: 信号量加1(释放)
 *
 * int os_sem_pend(OS_SEM *sem, u32 timeout)
 *     功能: 等待信号量(阻塞直到非0)
 *     参数: timeout - 等待超时时间(单位: 10ms)
 *     返回: 0成功, OS_TIMEOUT超时
 *     说明: timeout = MS2TICK(ms) = (ms)/10
 *
 * 【队列接口】(来源: util/cqueue.h)
 * -------
 * int my_blocking_queue_init(MyBlockingQueue *queue, size_t elem_size, size_t capacity)
 *     功能: 初始化阻塞队列
 *     参数: elem_size - 元素大小(这里为 sizeof(ble_dev_op_t))
 *           capacity - 队列容量(BLE_DEV_TASK_SCHEDULER_OP_QUEUE_CAPACITY=100)
 *
 * int my_blocking_queue_enqueue(MyBlockingQueue *queue, const void *elem)
 *     功能: 入队(阻塞式)
 *
 * int my_blocking_queue_try_enqueue(MyBlockingQueue *queue, const void *elem)
 *     功能: 尝试入队(非阻塞)
 *     返回: MYERROR_Success成功, 其他值失败或队列满
 *
 * int my_blocking_queue_dequeue(MyBlockingQueue *queue, void *elem)
 *     功能: 出队(阻塞式，队列为空时等待)
 *
 * int my_blocking_queue_destroy(MyBlockingQueue *queue)
 *     功能: 销毁队列
 *
 * 【UART通信接口】(来源: protocol/cp/c_uart_p.h)
 * -------
 * void uart_send_switch_audio_bc(const uint8_t *broadcast_code, uint8_t flag)
 *     功能: 发送音频广播切换命令
 *     参数: broadcast_code - 广播代码(6字节)
 *           flag - 标志位(0x01)
 *     说明: 用于启用/禁用BDO音频广播功能
 *
 * 【日志接口】(来源: log.h)
 * -------
 * void log_i(const char *fmt, ...)
 *     功能: 打印信息级别日志
 *
 * void log_e(const char *fmt, ...)
 *     功能: 打印错误级别日志
 *
 * void log_i_buf(const uint8_t *buf, uint16_t len)
 *     功能: 打印二进制数据缓冲区(十六进制格式)
 *
 * 【公开接口】(供外部调用)
 * -------
 * void ble_dev_task_scheduler_init(void)
 *     功能: 初始化BLE设备任务调度器
 *     说明: 启动调度器线程，应在应用初始化时调用一次
 *
 * int bdo_active(lwl_eend_dev_t *dev, int timeout, ble_dev_op_cb_func_t cb, void *priv)
 *     功能: 异步激活BLE设备
 *     参数: dev - 目标设备指针
 *           timeout - BLE连接超时(毫秒)
 *           cb - 完成回调函数(可为NULL)
 *           priv - 回调私有数据
 *     返回: 1成功加入队列, 0操作已在进行或队列满
 *     说明: 检查设备是否有截止时间，满足条件则建立连接->发送激活数据->断连
 *
 * int bdo_turn_on(lwl_eend_dev_t *dev, int conn_timeout, int scan_timeout,
 *                  ble_dev_op_cb_func_t cb, void *priv)
 *     功能: 异步打开(启用)BLE设备
 *     参数: dev - 目标设备指针
 *           conn_timeout - BLE连接超时(毫秒)
 *           scan_timeout - 广播扫描超时(毫秒)
 *           cb - 完成回调函数
 *           priv - 回调私有数据
 *     返回: 1成功加入队列, 0操作已在进行
 *     说明: 如果设备处于OFF状态则建立连接，然后启用音频广播功能
 *
 * int bdo_turn_off(lwl_eend_dev_t *dev)
 *     功能: 异步关闭(禁用)BLE设备
 *     参数: dev - 目标设备指针
 *     返回: 1成功
 *     说明: 禁用音频广播，等待设备进入OFF状态
 *
 * void bdo_enable(void)
 *     功能: 启用BDO(Broadcast Device Operation)功能
 *     说明: 向UART发送启用命令和当前广播码
 *
 * void bdo_disable(bool mute)
 *     功能: 禁用BDO功能
 *     说明: 向UART发送禁用命令(使用全0广播码)
 *
 * void bdo_notify_e_conncted(void)
 *     功能: 通知端点设备已连接/启用(供外部事件回调调用)
 *
 * void bdo_notify_e_disconncted(void)
 *     功能: 通知端点设备已断连/禁用(供外部事件回调调用)
 *
 * ============================================================================
 */
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

#define BLE_DISCONNECT_TIMEOUT 500 // TODO

#define SET_PI_ADV_INTERVAL_MS 30 // unit: ms range: 20 ~ 1000
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

const uint8_t empty_broadcast_code[6] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
uint8_t broadcast_code[6] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

static inline void process_cb(ble_dev_op_t *op, u8 code) {
    if (op->cb) op->cb(code, op->cb_priv);
}

void bdo_disable(bool mute)
{
    log_i("[BDO]bdo_disable");
    uart_send_set_ctrl(mute ? 0x01 : 0x00);
    log_i("[BDO] switch_audio_bc %s ctrl(0x0%d)", broadcast_code, mute);
}

void bdo_enable()
{
    log_i("[BDO]bdo_enable %s", broadcast_code);
    log_i("[BDO] switch_audio_bc %s ctrl(0x03)", broadcast_code);
    uart_send_set_ctrl(0x03);
}

void bdo_notify_e_conncted(char sn[])
{
    if (!sn) return;
    log_i("[BDO] Device %s connected", sn);
    if (strcmp(broadcast_code, &sn[10]) == 0) {
        os_sem_post(&g_e_connected_sem);
    }
    // TODO: 这里可以考虑直接清除断连信号量，以防止误触发
}

void bdo_notify_e_disconncted(char sn[])
{
    if (!sn) return;
    log_i("[BDO] Device %s disconnectd(%s)", sn, broadcast_code);
    if (strcmp(broadcast_code, &sn[10]) == 0) {
        log_i("[BDO] switch_audio_bc %s -> %s ctrl(0x01)", broadcast_code, empty_broadcast_code);
        memcpy(broadcast_code, empty_broadcast_code, sizeof(broadcast_code));
        uart_send_switch_audio_bc(broadcast_code, 0x01);
        uart_send_set_ctrl(0x01); // soft disable
        os_sem_post(&g_e_disconnected_sem);
    }
    // TODO: 这里可以考虑直接清除断连信号量，以防止误触发
}

static int max_rssi = -127;
static bool need_active = true;

static void ble_dev_op_handler_active(ble_dev_op_t* op)
{
    log_i("[BDO]Active start");
    /* - 查询当前设备状态，是否是一个 off stat，且有 broadcast code */
    lwl_eend_dev_t *dev = find_eend_dev_by_sn(op->sn);
    if (!dev) {
        log_i("[BDO]Error dev not found!");
        return;
    }
    for (int i=0;i<10;i++) {
        mdelay(50);
        if (!my_blocking_queue_is_empty(&g_ble_dev_op_queue) || need_active == false) {
            log_i("[BDO]There are other pending tasks, skip this activation!");
            dev->op_state &= ~BDO_STATE_ACTIVATING;
            process_cb(op, BDOCode_Skip);
            return;
        }
    }
    if (dev->max_rssi < max_rssi) {
        log_i("[BDO]not the closest dev, ignore!");
        dev->op_state &= ~BDO_STATE_ACTIVATING;
        process_cb(op, BDOCode_Skip);
        return;
    }
    max_rssi = -127;
    u32 timestamp_now = lwl_rtc_timer_get_sec();
    if (dev->adv_state == BDPAS_OFF && (dev->has_deadline && dev->deadline >= timestamp_now + 10)) {
        log_i("[BDO]Device already active, skip!");
        dev->op_state &= ~BDO_STATE_ACTIVATING;
        process_cb(op, BDOCode_Skip);
        return;
    }

    /* - 建立连接 */
    log_i("[BDO]ble handshake_task %s\n", op->sn);
    os_sem_set(&g_conn_sem, 0);
    if (ble_simple_client_create_connection(op->active.address, op->active.address_type) == APP_BLE_OPERATION_ERROR) {
        log_e("[BDO]Error failed to create connection APP_BLE_OPERATION_ERROR!!!");
        ble_simple_client_module_enable(1);
        dev->op_state &= ~BDO_STATE_ACTIVATING;
        process_cb(op, BDOCode_BLE_Api_Error);
        return;
    }
    if (os_sem_pend(&g_conn_sem, MS2TICK(op->active.conn_timeout)) == OS_TIMEOUT) {
        ble_simple_client_create_cannel();
        log_e("[BDO]Error ble connection timeout!!!");
        ble_simple_client_module_enable(1);
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
    
    for (int i=0;i<10;i++) {
        if (dev->distance_info.rssi_sample_count > 0)  break;
        mdelay(50);
    }
    if (dev->distance_info.rssi_sample_count == 0) {
        log_i("[BDO]Error dev lost!");
        dev->op_state &= ~BDO_STATE_TURNNING_ON;
        process_cb(op, BDOCode_Dev_Busy);
        return;
    }

    if (dev->adv_state == BDPAS_CONNECTED) {
        memcpy(broadcast_code, op->turnon.broadcast_code, sizeof(broadcast_code));
        os_sem_set(&g_e_disconnected_sem, 0);
        if (os_sem_pend(&g_e_disconnected_sem, MS2TICK(op->turnon.scan_timeout)) == OS_TIMEOUT) {
            log_e("[BDO]Error dev busy! %d", op->turnon.scan_timeout);
            dev->op_state &= ~BDO_STATE_TURNNING_ON;
            process_cb(op, BDOCode_Dev_Busy);
            return;
        }
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
            ble_simple_client_module_enable(1);
            dev->op_state &= ~BDO_STATE_TURNNING_ON;
            process_cb(op, BDOCode_BLE_Api_Error);
            return;
        }
        if (os_sem_pend(&g_conn_sem, MS2TICK(op->turnon.conn_timeout)) == OS_TIMEOUT) {
            ble_simple_client_create_cannel();
            log_e("[BDO]Error ble connection timeout!!!");
            ble_simple_client_module_enable(1);
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
    } else if (dev->adv_state == BDPAS_CONNECTED) {
        log_i("[BDO]Error dev busy!");
        dev->op_state &= ~BDO_STATE_TURNNING_ON;
        process_cb(op, BDOCode_Dev_Busy);
        return;
    }
    log_i("[BDO]Waiting adv state change...");
    process_cb(op, BDOCode_Success);
    msleep(300);
    if (dev->op_state & BDO_STATE_TURNNING_ON) {
        log_i("[BDO]Continue turn on...");
        log_i("[BDO] switch_audio_bc %s -> %s ctrl(0x01)", broadcast_code, op->turnon.broadcast_code);
        memcpy(broadcast_code, op->turnon.broadcast_code, sizeof(broadcast_code));
        uart_send_set_ctrl(0x01);
        uart_send_switch_audio_bc(broadcast_code, 0x01);
        uart_send_set_ctrl(0x01);

        /* - 扫描设备 ADV 状态变化 -> connected */
        if (os_sem_pend(&g_e_connected_sem, MS2TICK(op->turnon.scan_timeout)) == OS_TIMEOUT) {
            log_e("[BDO]Error scan adv state timeout!!!");
            dev->op_state &= ~BDO_STATE_TURNNING_ON;
            process_cb(op, BDOCode_Scan_Adv_Stat_Timeout);
            return;
        }
    }

    if (dev->op_state & BDO_STATE_TURNNING_ON) {
        log_i("[BDO]Turn on end");
        dev->op_state &= ~BDO_STATE_TURNNING_ON;
        process_cb(op, BDOCode_Ready_to_Play);
    }
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
    if (dev->adv_state == BDPAS_OFF) {
        dev->op_state &= ~BDO_STATE_TURNNING_OFF;
        process_cb(op, BDOCode_Success);
        return;
    }
    if (strcmp(broadcast_code, &dev->sn[10]) == 0) {
        log_i("[BDO] switch_audio_bc %s ctrl(0x00)", broadcast_code);
        uart_send_set_ctrl(0x00);
    }

    os_sem_set(&g_e_disconnected_sem, 0);
    /* - 扫描设备 ADV 状态变化 -> off */
    if (os_sem_pend(&g_e_disconnected_sem, MS2TICK(op->turnoff.scan_timeout)) == OS_TIMEOUT) {
        log_e("[BDO]Error scan adv state timeout!!! %d", op->turnoff.scan_timeout);
        dev->op_state &= ~BDO_STATE_TURNNING_OFF;
        process_cb(op, BDOCode_Scan_Adv_Stat_Timeout);
        return;
    }

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

void ble_dev_task_scheduler_init()
{
    log_i("ble_dev_task_scheduler_init");
    ASSERT(my_blocking_queue_init(&g_ble_dev_op_queue, sizeof(ble_dev_op_t), BLE_DEV_TASK_SCHEDULER_OP_QUEUE_CAPACITY) == 0);
    
    os_sem_create(&g_conn_sem, 0);
    os_sem_create(&g_disconn_sem, 0);
    os_sem_create(&g_e_connected_sem, 0);
    os_sem_create(&g_e_disconnected_sem, 0);
    ble_conn_event_user_handler_add(ble_conn_event_handler, NULL);
    ble_disconn_event_user_handler_add(ble_disconn_event_handler, NULL);
}

int dev_task_is_on = 0;
int dev_task_is_running = 0;

static void ble_dev_task_scheduler()
{

    ble_dev_op_t op;

    log_i("ble_dev_task_scheduler");

    dev_task_is_running = 1;

    while (dev_task_is_on) {
        if (my_blocking_queue_is_empty(&g_ble_dev_op_queue)) {
            max_rssi = -127;
        }
        my_blocking_queue_dequeue(&g_ble_dev_op_queue, &op);
        ASSERT(op.op_type < g_op_handler_entries_count);

        if (g_op_handler_entries[op.op_type] != NULL) {
            g_op_handler_entries[op.op_type](&op);
            need_active = false;
        }
    }
    dev_task_is_running = 0;
    my_blocking_queue_clear(&g_ble_dev_op_queue);
}

void ble_dev_task_scheduler_start()
{
    if (dev_task_is_running) {
        return;
    }
    dev_task_is_on = true;
    max_rssi = -127;
    log_i("ble_dev_task_scheduler_start");
    if (thread_fork("ble_dev_task_scheduler", 15, 512, 0, 0, ble_dev_task_scheduler, NULL)) {
        log_e("Error: Failed to create task ble_dev_task_scheduler.");
    }
}

void ble_dev_task_scheduler_stop()
{
    dev_task_is_on = false;
}

void ble_dev_task_enable()
{
    need_active = true;
}

int bdo_active(lwl_eend_dev_t *dev, int timeout, ble_dev_op_cb_func_t cb, void *priv)
{
    log_i("[BDO]Active %s, %d", dev->sn, dev->max_rssi);
    // 获取dev的最大rssi信号值
    int rssi = dev->max_rssi;
    if (max_rssi > dev->max_rssi) {
        return -1;
    }
    
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
    log_i("[BDO]bdo_turn_on %s", dev->sn);
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
    log_i("[BDO]bdo_turn_on %s(%s)", dev->sn, op.turnon.broadcast_code);
    op.cb = cb;
    op.cb_priv = priv;
    return my_blocking_queue_push_front(&g_ble_dev_op_queue, &op) == MYERROR_Success;
}

int bdo_turn_off(lwl_eend_dev_t *dev)
{
    log_i("[BDO]bdo_turn_off %s", dev->sn);
    if (dev->op_state & BDO_STATE_TURNNING_ON) {
        log_i("[BDO]Turn on abort");
        dev->op_state &= ~BDO_STATE_TURNNING_ON;
        os_sem_post(&g_e_connected_sem);

    }
    dev->op_state = BDO_STATE_IDLE;
    // broadcast_code 等于dev->sn的后6位则继续
    if (strcmp(broadcast_code, &dev->sn[10]) == 0) {
        log_i("[BDO] switch_audio_bc %s ctrl(0x00)", broadcast_code);
        uart_send_set_ctrl(0x00);
    }
    return 1;
}
