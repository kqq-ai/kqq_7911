/**
 * @brief MODE_6 - 极简 BLE 基础功能测试
 *
 * 测试流程：
 *   1. 扫描附近 BLE 设备
 *   2. 找到第一个可连接设备后发起连接
 *   3. 连接成功后发送测试数据
 *   4. 2秒后主动断开
 *
 * 所有日志以 [BLE_TEST] 前缀输出，方便过滤查看
 */

#include "mode_list.h"
#include "bt_ble/client/ble_simple_client.h"
#include "btstack/le/ble_api.h"
#include "btstack/le/le_common_define.h"
#include "btstack/btstack_typedef.h"
#include "system/timer.h"
#include "os/os_api.h"
#include <string.h>

//=============================================================================
// 配置参数
//=============================================================================
#define BLE_TEST_SCAN_INTERVAL      ADV_SCAN_MS(50)   // 扫描间隔 50ms
#define BLE_TEST_SCAN_WINDOW        ADV_SCAN_MS(30)   // 扫描窗口 30ms
#define BLE_TEST_CONN_INTERVAL      24                // 连接间隔 30ms (24*1.25)
#define BLE_TEST_CONN_LATENCY       0
#define BLE_TEST_CONN_TIMEOUT       400               // 超时 4s (400*10ms)
#define BLE_TEST_DISCONNECT_DELAY   2000              // 连接后2s断开
#define BLE_TEST_CONNECT_TIMEOUT    5000              // 连接超时 5s

//=============================================================================
// 测试状态
//=============================================================================
typedef enum {
    BLE_TEST_IDLE = 0,
    BLE_TEST_SCANNING,
    BLE_TEST_CONNECTING,
    BLE_TEST_CONNECTED,
    BLE_TEST_DONE,
} ble_test_state_e;

static volatile ble_test_state_e g_test_state = BLE_TEST_IDLE;
static u8  g_target_addr[6];
static u8  g_target_addr_type;
static u16 g_conn_handle = 0;
static u16 g_disconnect_timer = 0;
static u16 g_connect_timer = 0;

// 测试数据
static const u8 g_test_data[] = {0x01, 0x02, 0x03, 0x04, 0x05};

//=============================================================================
// 内部函数
//=============================================================================
static void ble_test_stop_scan(void)
{
    log_i("[BLE_TEST] stop scan");
    ble_op_scan_enable2(0, 0);
}

static void ble_test_start_scan(void)
{
    log_i("[BLE_TEST] start scan...");
    ble_op_set_scan_param(SCAN_ACTIVE, BLE_TEST_SCAN_INTERVAL, BLE_TEST_SCAN_WINDOW);
    ble_op_scan_enable2(1, 0);
    g_test_state = BLE_TEST_SCANNING;
}

static void ble_test_connect_timeout_cb(void *priv)
{
    (void)priv;
    g_connect_timer = 0;

    if (g_test_state == BLE_TEST_CONNECTING) {
        log_w("[BLE_TEST] connect timeout, cancel and rescan");
        ble_op_create_connection_cancel();
        g_test_state = BLE_TEST_SCANNING;
        ble_test_start_scan();
    }
}

static void ble_test_create_connection(void)
{
    struct create_conn_param_t param;

    memset(&param, 0, sizeof(param));
    param.conn_interval = BLE_TEST_CONN_INTERVAL;
    param.conn_latency = BLE_TEST_CONN_LATENCY;
    param.supervision_timeout = BLE_TEST_CONN_TIMEOUT;
    param.peer_address_type = g_target_addr_type;
    memcpy(param.peer_address, g_target_addr, 6);
    
    log_i("[BLE_TEST] connecting to %02X:%02X:%02X:%02X:%02X:%02X (type=%d)",
          g_target_addr[5], g_target_addr[4], g_target_addr[3],
          g_target_addr[2], g_target_addr[1], g_target_addr[0],
          g_target_addr_type);
    
    g_test_state = BLE_TEST_CONNECTING;
    int ret = ble_op_create_connection(&param);
    if (ret) {
        log_w("[BLE_TEST] create_connection failed ret=%d, rescan", ret);
        g_test_state = BLE_TEST_SCANNING;
        ble_test_start_scan();
        return;
    }

    if (g_connect_timer) {
        sys_timeout_del(g_connect_timer);
    }
    g_connect_timer = sys_timeout_add(NULL, ble_test_connect_timeout_cb, BLE_TEST_CONNECT_TIMEOUT);
}

static void ble_test_send_data(void)
{
    if (g_conn_handle == 0) {
        log_w("[BLE_TEST] send_data: no connection");
        return;
    }
    
    // 使用通用的ATT写入（handle=0x0003是常见的特征值句柄，实际可能需要调整）
    // 这里用 write without response 方式发送测试数据
    log_i("[BLE_TEST] sending %d bytes test data...", sizeof(g_test_data));
    
    // 注意：如果对方没有对应的特征值，写入会失败，但不影响测试连接是否正常
    int ret = ble_op_multi_att_send_data(g_conn_handle, 0x0003, (u8 *)g_test_data, sizeof(g_test_data), ATT_OP_WRITE_WITHOUT_RESPOND);
    if (ret == 0) {
        log_i("[BLE_TEST] send OK");
    } else {
        log_w("[BLE_TEST] send failed: %d (handle may not exist, ignore)", ret);
    }
}

static void ble_test_disconnect(void)
{
    if (g_conn_handle == 0) {
        return;
    }
    log_i("[BLE_TEST] disconnect...");
    ble_op_disconnect(g_conn_handle);
}

static void ble_test_disconnect_timer_cb(void *priv)
{
    (void)priv;
    g_disconnect_timer = 0;
    
    if (g_test_state == BLE_TEST_CONNECTED) {
        log_i("[BLE_TEST] 2s elapsed, disconnecting...");
        ble_test_disconnect();
    }
}

//=============================================================================
// BLE 事件处理 - 由协议栈回调
//=============================================================================

// 广播报告回调 - 找设备
static void ble_test_adv_report_handler(adv_report_t *report)
{
    if (g_test_state != BLE_TEST_SCANNING) {
        return;
    }
    
    // 只接受可连接广播 (ADV_IND=0, ADV_DIRECT_IND=1)
    if (report->event_type > 1) {
        return;
    }
    
    // 找到设备，停止扫描并连接
    memcpy(g_target_addr, report->address, 6);
    g_target_addr_type = report->address_type;
    
    log_i("[BLE_TEST] found device: %02X:%02X:%02X:%02X:%02X:%02X rssi=%d",
          report->address[5], report->address[4], report->address[3],
          report->address[2], report->address[1], report->address[0],
          report->rssi);
    
    ble_test_stop_scan();
    ble_test_create_connection();
}

// 连接完成回调
static void ble_test_connection_complete_handler(u16 handle)
{
    if (g_test_state != BLE_TEST_CONNECTING) {
        return;
    }
    
    g_conn_handle = handle;
    g_test_state = BLE_TEST_CONNECTED;
    
    log_i("[BLE_TEST] === CONNECTED === handle=0x%04X", handle);
    
    // 连接成功后立即发送测试数据
    ble_test_send_data();
    
    // 启动2s后断开的定时器
    if (g_disconnect_timer) {
        sys_timeout_del(g_disconnect_timer);
    }
    g_disconnect_timer = sys_timeout_add(NULL, ble_test_disconnect_timer_cb, BLE_TEST_DISCONNECT_DELAY);
}

// 断开连接回调
static void ble_test_disconnection_complete_handler(u16 handle, u8 reason)
{
    (void)handle;
    
    log_i("[BLE_TEST] === DISCONNECTED === reason=0x%02X", reason);
    
    if (g_disconnect_timer) {
        sys_timeout_del(g_disconnect_timer);
        g_disconnect_timer = 0;
    }
    
    g_conn_handle = 0;
    g_test_state = BLE_TEST_DONE;
    
    log_i("[BLE_TEST] ========== TEST COMPLETE ==========");
}

// 连接失败回调
static void ble_test_connection_failed_handler(void)
{
    log_w("[BLE_TEST] connection failed, restart scan");
    g_test_state = BLE_TEST_SCANNING;
    ble_test_start_scan();
}

//=============================================================================
// 注册到 ble_simple_client 的回调（复用现有框架的事件分发）
//=============================================================================
static void mode6_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size)
{
    if (get_current_mode() != WORK_MODE_6) return;

    // Debug: log first few packets to verify hook is working
    static int debug_pkt_count = 0;
    if (debug_pkt_count < 5) {
        log_i("[BLE_TEST] hook recv pkt type=%d", packet_type);
        debug_pkt_count++;
    }

    if (packet_type == HCI_EVENT_PACKET) {
        switch (hci_event_packet_get_type(packet)) {
        case GAP_EVENT_ADVERTISING_REPORT:
            if (g_test_state == BLE_TEST_SCANNING) {
                adv_report_t *report = (adv_report_t *)&packet[2];
                ble_test_adv_report_handler(report);
            }
            break;
            
        case HCI_EVENT_LE_META:
            switch (hci_event_le_meta_get_subevent_code(packet)) {
            case HCI_SUBEVENT_LE_ENHANCED_CONNECTION_COMPLETE:
            case HCI_SUBEVENT_LE_CONNECTION_COMPLETE: {
                if (g_test_state == BLE_TEST_CONNECTING) {
                    u8 status = hci_subevent_le_connection_complete_get_status(packet);
                    if (status) {
                        // ble_test_connection_failed_handler();
                        log_w("[BLE_TEST] connection failed status=0x%x", status);
                        g_test_state = BLE_TEST_SCANNING;
                        ble_test_start_scan();
                    } else {
                        u16 handle = little_endian_read_16(packet, 3);
                        // ble_test_connection_complete_handler(handle);
                        g_conn_handle = handle;
                        g_test_state = BLE_TEST_CONNECTED;
                        log_i("[BLE_TEST] === CONNECTED === handle=0x%04X", handle);
                        ble_test_send_data();
                        if (g_connect_timer) {
                            sys_timeout_del(g_connect_timer);
                            g_connect_timer = 0;
                        }
                        if (g_disconnect_timer) sys_timeout_del(g_disconnect_timer);
                        g_disconnect_timer = sys_timeout_add(NULL, ble_test_disconnect_timer_cb, BLE_TEST_DISCONNECT_DELAY);
                    }
                }
            } break;
            }
            break;

        case HCI_EVENT_DISCONNECTION_COMPLETE: {
            u16 handle = little_endian_read_16(packet, 3);
            u8 reason = packet[5];
            if (handle == g_conn_handle) {
                // ble_test_disconnection_complete_handler(handle, reason);
                log_i("[BLE_TEST] === DISCONNECTED === reason=0x%02X", reason);
                if (g_connect_timer) {
                    sys_timeout_del(g_connect_timer);
                    g_connect_timer = 0;
                }
                if (g_disconnect_timer) { sys_timeout_del(g_disconnect_timer); g_disconnect_timer = 0; }
                g_conn_handle = 0;
                g_test_state = BLE_TEST_DONE;
                log_i("[BLE_TEST] ========== TEST COMPLETE ==========");
            }
        } break;
        }
    }
}

//=============================================================================
// 对外接口
//=============================================================================
void mode6_init(void)
{
    log_i("[BLE_TEST] ========================================");
    log_i("[BLE_TEST] BLE Basic Test Start");
    log_i("[BLE_TEST] Flow: Scan -> Connect -> Send Data -> Disconnect");
    log_i("[BLE_TEST] ========================================");
    
    g_test_state = BLE_TEST_IDLE;
    g_conn_handle = 0;
    g_disconnect_timer = 0;
    g_connect_timer = 0;
    memset(g_target_addr, 0, sizeof(g_target_addr));
    
    // 注册底层 HCI 事件回调
    ble_simple_client_set_raw_packet_handler(mode6_packet_handler);

    // 确保模块启用 (可能需要它来初始化某些状态)
    ble_simple_client_module_enable(1);

    // 停止框架的自动扫描，使用手动控制
    ble_combined_request_scan(0);
    ble_combined_apply();
    
    // 启动扫描
    ble_test_start_scan();
}

void mode6_uninit(void)
{
    log_i("[BLE_TEST] uninit");
    
    if (g_connect_timer) {
        sys_timeout_del(g_connect_timer);
        g_connect_timer = 0;
    }
    if (g_disconnect_timer) {
        sys_timeout_del(g_disconnect_timer);
        g_disconnect_timer = 0;
    }
    
    // 如果还在连接中，断开
    if (g_conn_handle) {
        ble_op_disconnect(g_conn_handle);
        g_conn_handle = 0;
    }
    
    ble_test_stop_scan();
    
    // 取消回调注册
    ble_simple_client_set_raw_packet_handler(NULL);
    
    // 恢复默认
    ble_simple_client_module_enable(0);

    g_test_state = BLE_TEST_IDLE;
}

void process_mode_6(void)
{
    // 周期性打印状态，方便调试
    static u32 last_print = 0;
    if (timer_get_ms() - last_print > 1000) {
        last_print = timer_get_ms();
        log_i("[BLE_TEST] State: %d (0=IDLE, 1=SCAN, 2=CONNECTING, 3=CONNECTED, 4=DONE)", g_test_state);
    }
}
