/**
 * @brief This file is mainly used for bluetooth network configuration. 
 *  It contains functions and data structures related to bluetooth network pairing and communication, 
 *  which is crucial for establishing a stable connection and data exchange between bluetooth devices and the APP.
 * @author --
 * @date 2024/12/20
 */
#include "ble_simple_server.h"
#include "ble_simple_server_gatt_profile.inl"


#include "system/app_core.h"
#include "system/includes.h"

#include "app_config.h"

#include "btstack/btstack_task.h"
#include "btstack/bluetooth.h"
#include "btcontroller_modules.h"
#include "bt_common.h"

#include "le_common.h"

#include "third_party/common/ble_user.h"
#include "JL_rcsp_api.h"

#include "bt_ble/ble_combined.h"

#include "bt_ble/ble_data.h"
#include "bt_ble/app_wifi_manage.h"  // 添加这行以访问 get_wifi_scan_status
#include "app_cookie.h"
#include "log.h"

// LOG 暂时采用printf 而不是log.h的函数
#if 1
extern void printf_buf(u8 *buf, u32 len);
#define log_info(...)          printf("[blee] " __VA_ARGS__)
#define log_info_hexdump       printf_buf
#else
#define log_info(...)
#define log_info_hexdump(...)
#endif

//------
//ATT发送的包长,    note: 20 <=need >= MTU
#define ATT_LOCAL_MTU_SIZE      (200)                   //
//ATT缓存的buffer大小,  note: need >= 20,可修改
#define ATT_SEND_CBUF_SIZE      (2560)                   //

//共配置的RAM
#define ATT_RAM_BUFSIZE         (ATT_CTRL_BLOCK_SIZE + ATT_LOCAL_MTU_SIZE + ATT_SEND_CBUF_SIZE)                   //note:
static u8 att_ram_buffer[ATT_RAM_BUFSIZE] __attribute__((aligned(4)));
//---------------

//---------------
// 广播周期 (unit:0.625ms)
#define ADV_INTERVAL_MIN        (160*5)

#define HOLD_LATENCY_CNT_MIN    (3)  //(0~0xffff)
#define HOLD_LATENCY_CNT_MAX    (15) //(0~0xffff)
#define HOLD_LATENCY_CNT_ALL    (0xffff)

static const char ble_ext_name[] = "(BLE)";
static const char ble_ext_busy[] = "(m)";
static const char ble_ext_idle[] = "(t)";

//连接参数更新请求设置
//是否使能参数请求更新,0--disable, 1--enable
static const uint8_t connection_update_enable = 1; ///0--disable, 1--enable
//当前请求的参数表index
static uint8_t connection_update_cnt = 0; //
// 标志：是否已推送初始WiFi列表（防止重复推送）
static bool initial_wifi_list_pushed = false;
//参数表
static const struct conn_update_param_t connection_param_table[] = {
    {16, 24, 10, 600},//11
    {12, 28, 10, 600},//3.7
    { 8, 20, 10, 600},
 /* {12, 28,  4, 600},//3.7  */
 /* {12, 24, 30, 600},//3.05 */
};
// 用于打印
static const char *const phy_result[] = {
    "None",
    "1M",
    "2M",
    "Coded",
};

//共可用的参数组数
#define CONN_PARAM_TABLE_CNT    (sizeof(connection_param_table) / sizeof(struct conn_update_param_t))

#if (ATT_RAM_BUFSIZE < 64)
#error "adv_data & rsp_data buffer error!!!!!!!!!!!!"
#endif

static u8 adv_data_len;
static u8 adv_data[ADV_RSP_PACKET_MAX];//max is 31
static u8 scan_rsp_data_len;
static u8 scan_rsp_data[ADV_RSP_PACKET_MAX];//max is 31

static char gap_device_name[BT_NAME_LEN_MAX+1] = "wl80";
static u8 gap_device_name_len = 0; //名字长度，不包含结束符
// 广播 Manufacturer data 末尾标记：t=0, m=1（给小程序 advertisData 用）
static u8 adv_tm_flag = 0;
static u8 ble_work_state = 0;      //ble 状态变化
static u8 adv_ctrl_en;             //广播控制
static OS_MUTEX mutex;             //
static void (*app_ble_state_callback)(void *priv, ble_state_e state) = NULL;
static void (*ble_resume_send_wakeup)(void) = NULL;

static int app_send_user_data(u16 conn_handle, u16 handle, u8 *data, u16 len, u8 handle_type);
static int send_return_json_data(const uint8_t * return_json_data);

// Complete Local Name  默认的蓝牙名字

//------------------------------------------------------
//广播参数设置
static void advertisements_setup_init();
extern const char *bt_get_local_name();
//------------------------------------------------------

//=BLE other==============================================================================================
// BLE other
//=BLE other==============================================================================================

static void send_request_connect_parameter(u8 table_index)
{
    struct conn_update_param_t *param = (void *)&connection_param_table[table_index];//static ram

    log_info("update_request:-%d-%d-%d-%d-\n", param->interval_min, param->interval_max, param->latency, param->timeout);
    if (server_con_handle != INVALID_HANDLE) {
        ble_op_conn_param_request(server_con_handle, param);
    }
}

static void check_connetion_updata_deal(void)
{
    if (connection_update_enable) {
        if (connection_update_cnt < CONN_PARAM_TABLE_CNT) {
            send_request_connect_parameter(connection_update_cnt);
        }
    }
}

static void connection_update_complete_success(u16 conn_handle, u8 *packet)
{
    int conn_interval, conn_latency, conn_timeout;

    conn_interval = hci_subevent_le_connection_update_complete_get_conn_interval(packet);
    conn_latency = hci_subevent_le_connection_update_complete_get_conn_latency(packet);
    conn_timeout = hci_subevent_le_connection_update_complete_get_supervision_timeout(packet);

    log_info("c = %d, i = %d, l = %d, t = %d\n", conn_handle, conn_interval, conn_latency, conn_timeout);
}

static void set_ble_work_state(ble_state_e state)
{
    if (state != ble_work_state) {
        log_info("ble_work_st:%x->%x\n", ble_work_state, state);
        ble_work_state = state;
        if (app_ble_state_callback) {
            app_ble_state_callback(NULL, state);
        }
    }
}

static ble_state_e get_ble_work_state(void)
{
    return ble_work_state;
}

static void can_send_now_wakeup(void)
{
    /* putchar('E'); */
    log_d("[blee] -------------------------now can send datas -----------------------------\r\n");
    if (ble_resume_send_wakeup) {
        ble_resume_send_wakeup();
    }
}

static void set_connection_data_length(u16 tx_octets, u16 tx_time)
{
    if (server_con_handle != INVALID_HANDLE) {
        ble_op_set_data_length(server_con_handle, tx_octets, tx_time);
    }
}

static void set_connection_data_phy(u8 tx_phy, u8 rx_phy)
{
    if (INVALID_HANDLE == server_con_handle) {
        return;
    }

    u8 all_phys = 0;
    u16 phy_options = CONN_SET_PHY_OPTIONS_S8;

    ble_op_set_ext_phy(server_con_handle, all_phys, tx_phy, rx_phy, phy_options);
}

static void server_profile_start(u16 con_handle)
{
    ble_op_att_send_init(con_handle, att_ram_buffer, ATT_RAM_BUFSIZE, ATT_LOCAL_MTU_SIZE);
    set_ble_work_state(BLE_ST_CONNECT);

    /* set_connection_data_phy(CONN_SET_CODED_PHY, CONN_SET_CODED_PHY); */
}

static int get_buffer_vaild_len()
{
    u32 vaild_len = 0;
    ble_op_multi_att_get_remain(server_con_handle, &vaild_len);
    return vaild_len;
}

static int app_send_user_data_do(u8 *data, u16 len)
{
    return app_send_user_data(server_con_handle, ATT_CHARACTERISTIC_0000fff2_0000_1000_8000_00805f9b34fb_01_VALUE_HANDLE, data, len, ATT_OP_AUTO_READ_CCC);
}

static int app_send_user_data_check(u16 len)
{
    u32 buf_space = get_buffer_vaild_len();
    if (len <= buf_space) {
        return 1;
    }
    return 0;
}

_WEAK_
u8 ble_update_get_ready_jump_flag(void)
{
    return 0;
}



//=BLE send==============================================================================================
// BLE send
//=BLE send==============================================================================================

/*
 * @section Packet Handler
 *
 * @text The packet handler is used to:
 *        - stop the counter after a disconnect
 *        - send a notification when the requested ATT_EVENT_CAN_SEND_NOW is received
 */

/* LISTING_START(packetHandler): Packet Handler */
static void server_cbk_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size)
{
    int mtu;
    u32 tmp;
    u8 status;

    switch (packet_type) {
    case HCI_EVENT_PACKET:
        switch (hci_event_packet_get_type(packet)) {

        case ATT_EVENT_HANDLE_VALUE_INDICATION_COMPLETE:
            log_info("ATT_EVENT_HANDLE_VALUE_INDICATION_COMPLETE\n");
        case ATT_EVENT_CAN_SEND_NOW:
            // 允许发送，协议栈内部有一个数据发送缓冲，可能在协商连接参数等操作时，协议栈处于不允许发送数据的状态
            // 此时所有要发送数据会缓存在内部的发送缓冲中，可能在协商连接参数完成后，状态回到允许发送数据的状态
            // 此时触发CAN_SEND_NOW 同时协议栈会开始处理发送缓冲中的数据
            can_send_now_wakeup();
            break;

        case HCI_EVENT_LE_META:
            switch (hci_event_le_meta_get_subevent_code(packet)) {
            case HCI_SUBEVENT_LE_ENHANCED_CONNECTION_COMPLETE:
                status = hci_subevent_le_enhanced_connection_complete_get_status(packet);
                if (status) {
                    // 开启高占空比定向回连广播时，若在持续的1.28秒内没有成功回连，则会进入此判断分支
                    log_info("LE_SLAVE CONNECTION FAIL!!! %0x\n", status);
                    set_ble_work_state(BLE_ST_DISCONN);
                    bt_ble_adv_enable(1);
                    break;
                }
                server_con_handle = hci_subevent_le_enhanced_connection_complete_get_connection_handle(packet);
                log_info("HCI_SUBEVENT_LE_ENHANCED_CONNECTION_COMPLETE : %0x\n", server_con_handle);
                log_info("[blee-ble] BLE connection successful, handle: 0x%04x\n", server_con_handle);

                connection_update_cnt = 0;
                ble_op_multi_att_send_conn_handle(server_con_handle, 0, MULTI_ROLE_SERVER);
                log_info("i = %d, l = %d, t = %d\n", hci_subevent_le_enhanced_connection_complete_get_conn_interval(packet)
                        , hci_subevent_le_enhanced_connection_complete_get_conn_latency(packet)
                        , hci_subevent_le_enhanced_connection_complete_get_supervision_timeout(packet));
                server_profile_start(server_con_handle);
                break;

            case HCI_SUBEVENT_LE_CONNECTION_COMPLETE:
                status = hci_subevent_le_connection_complete_get_status(packet);
                if (status) {
                    // 开启高占空比定向回连广播时，若在持续的1.28秒内没有成功回连，则会进入此判断分支
                    log_info("LE_SLAVE CONNECTION FAIL!!! %0x\n", status);
                    set_ble_work_state(BLE_ST_DISCONN);
                    bt_ble_adv_enable(1);
                    break;
                }
                server_con_handle = hci_subevent_le_connection_complete_get_connection_handle(packet);
                log_info("HCI_SUBEVENT_LE_CONNECTION_COMPLETE: %0x\n", server_con_handle);
                log_info("[blee-ble] BLE connection successful, handle: 0x%04x\n", server_con_handle);

                connection_update_cnt = 0;
                ble_op_multi_att_send_conn_handle(server_con_handle, 0, MULTI_ROLE_SERVER);
                connection_update_complete_success(server_con_handle, packet + 8);
                server_profile_start(server_con_handle);
                break;

            case HCI_SUBEVENT_LE_CONNECTION_UPDATE_COMPLETE: {
                // 连接参数更新事件
                u16 tmp_handle = hci_subevent_le_connection_update_complete_get_connection_handle(packet);
                if (tmp_handle == server_con_handle) {
                    connection_update_complete_success(tmp_handle, packet);
                }
            }   break;

            case HCI_SUBEVENT_LE_DATA_LENGTH_CHANGE:
                log_info("APP HCI_SUBEVENT_LE_DATA_LENGTH_CHANGE\n");
                /* set_connection_data_phy(CONN_SET_CODED_PHY, CONN_SET_CODED_PHY); */
                break;

            case HCI_SUBEVENT_LE_PHY_UPDATE_COMPLETE:
                // PHY更新事件
                log_info("APP HCI_SUBEVENT_LE_PHY_UPDATE %s\n", hci_event_le_meta_get_phy_update_complete_status(packet) ? "Fail" : "Succ");
                log_info("Tx PHY: %s\n", phy_result[hci_event_le_meta_get_phy_update_complete_tx_phy(packet)]);
                log_info("Rx PHY: %s\n", phy_result[hci_event_le_meta_get_phy_update_complete_rx_phy(packet)]);
                break;
            }
            break;

        case HCI_EVENT_DISCONNECTION_COMPLETE: {
            u16 tmp_handle = hci_event_disconnection_complete_get_connection_handle(packet);
            if (tmp_handle != server_con_handle) {
                log_info("unknown_handle:%04x\n", tmp_handle);
                break;
            }

            log_info("HCI_EVENT_DISCONNECTION_COMPLETE: %0x\n", packet[5]);
            multi_att_clear_ccc_config(tmp_handle);
            server_con_handle = INVALID_HANDLE;
            // 重置初始列表推送标志，下次连接时可以再次推送
            initial_wifi_list_pushed = false;

            set_ble_work_state(BLE_ST_DISCONN);
            ble_op_multi_att_send_conn_handle(0, 0, MULTI_ROLE_SERVER);

            if( WIFI_CONNECT_SUCC != get_wifi_connect_status(NULL) ) {
                clear_key_press_time_ms();
                ble_simple_server_name_enable(0); //重新广播BLE
            }
            ///bt_ble_adv_enable(1);
        }   break;

        case ATT_EVENT_MTU_EXCHANGE_COMPLETE: {
            // MTU 更新事件
            u16 tmp_handle = att_event_mtu_exchange_complete_get_handle(packet);
            if (tmp_handle == server_con_handle) {
                mtu = att_event_mtu_exchange_complete_get_MTU(packet) - 3;
                log_d("[blee] ATT MTU = %u\n", mtu);
                ble_op_multi_att_set_send_mtu(tmp_handle, mtu);
                /* set_connection_data_length(251, 2120); */
            }
        }   break;

        case HCI_EVENT_VENDOR_REMOTE_TEST:
            log_info("--- HCI_EVENT_VENDOR_REMOTE_TEST\n");
            break;

        case L2CAP_EVENT_CONNECTION_PARAMETER_UPDATE_RESPONSE: {
            u16 tmp_handle = l2cap_event_connection_parameter_update_response_get_handle(packet);
            if (tmp_handle == server_con_handle) {
                tmp = l2cap_event_connection_parameter_update_response_get_result(packet);
                log_info("-update_rsp: %02x\n", tmp);
                if (tmp) {
                    connection_update_cnt++;
                    log_info("remoter reject!!!\n");
                    check_connetion_updata_deal();
                } else {
                    connection_update_cnt = CONN_PARAM_TABLE_CNT;
                }
            }
        }   break;

        case HCI_EVENT_ENCRYPTION_CHANGE:
            log_info("HCI_EVENT_ENCRYPTION_CHANGE= %d\n", packet[2]);
            break;
        }
        break;
    }
}

/* LISTING_END */

/*
 * @section ATT Read
 *
 * @text The ATT Server handles all reads to constant data. For dynamic data like the custom characteristic, the registered
 * att_read_callback is called. To handle long characteristics and long reads, the att_read_callback is first called
 * with buffer == NULL, to request the total value length. Then it will be called again requesting a chunk of the value.
 * See Listing attRead.
 */

/* LISTING_START(attRead): ATT Read */

// ATT Client Read Callback for Dynamic Data
// - if buffer == NULL, don't copy data, just return size of value
// - if buffer != NULL, copy data and return number bytes copied
// @param offset defines start of attribute value
static uint16_t att_read_callback(hci_con_handle_t connection_handle, uint16_t att_handle, uint16_t offset, uint8_t *buffer, uint16_t buffer_size)
{
    uint16_t  att_value_len = 0;
    uint16_t handle = att_handle;

    log_info("read_callback, handle= 0x%04x,buffer= %08x\n", handle, (u32)buffer);

    switch (handle) {
    case ATT_CHARACTERISTIC_0000fff2_0000_1000_8000_00805f9b34fb_01_VALUE_HANDLE:
        break;
    case ATT_CHARACTERISTIC_0000fff2_0000_1000_8000_00805f9b34fb_01_CLIENT_CONFIGURATION_HANDLE:
        break;
    case ATT_CHARACTERISTIC_0000fff1_0000_1000_8000_00805f9b34fb_01_VALUE_HANDLE:
        break;
    
    default:
        break;
    }

    log_info("att_value_len= %d\n", att_value_len);

    return att_value_len;
}

/* LISTING_END */
/*
 * @section ATT Write
 *
 * @text The only valid ATT write in this example is to the Client Characteristic Configuration, which configures notification
 * and indication. If the ATT handle matches the client configuration handle, the new configuration value is stored and used
 * in the heartbeat handler to decide if a new value should be sent. See Listing attWrite.
 */

/* LISTING_START(attWrite): ATT Write */
#include "bt_ble/ble_data.h"


char json_data[BUFFER_SIZE] = {0};
char return_json_data[BUFFER_SIZE] = {0};
static int att_write_callback(hci_con_handle_t connection_handle, uint16_t att_handle, uint16_t transaction_mode, uint16_t offset, uint8_t *buffer, uint16_t buffer_size)
{
    int ret = 0;
    int result = 0;
    u16 tmp16;
    u16 handle = att_handle;

    log_info("write_callback, handle= 0x%04x,size = %d\n", handle, buffer_size);
    log_info("write_callback, buffer= %s\n",buffer);

    switch (handle) {
    case ATT_CHARACTERISTIC_0000fff2_0000_1000_8000_00805f9b34fb_01_VALUE_HANDLE:
        break;
    case ATT_CHARACTERISTIC_0000fff2_0000_1000_8000_00805f9b34fb_01_CLIENT_CONFIGURATION_HANDLE:
    
        log_info("\n------write ccc:%04x,%02x\n", handle, buffer[0]);
        multi_att_set_ccc_config(connection_handle, handle, buffer[0]);
        
        // 当CCC被启用时（buffer[0] != 0），立即推送当前WiFi缓冲列表
        // 这是连接后首次推送，让小程序获取初始列表
        if (buffer[0] != 0 && !initial_wifi_list_pushed)
        {
            log_info("CCC enabled, pushing initial WiFi list to app\n");
            initial_wifi_list_pushed = true;
            // 延迟一小段时间确保通知已完全启用
            os_time_dly(1);  // 延迟约10ms
            notify_app_wifi_list_update_force(NULL);
        }
        break;
    case ATT_CHARACTERISTIC_0000fff1_0000_1000_8000_00805f9b34fb_01_VALUE_HANDLE:
        ret = parse_and_assemble(buffer, buffer_size, json_data);
        if (0 == ret) {
            ret = parse_and_dispatch_json(json_data,return_json_data);
            if (0 == ret) {
                memset(json_data, 0, BUFFER_SIZE);
            }
            log_info("return_json_data: %s\n",return_json_data);
            ret = send_return_json_data(return_json_data);
            if (0 == ret) {
                memset(return_json_data, 0, BUFFER_SIZE);
            } else {
                memset(return_json_data, 0, BUFFER_SIZE);
            }
            log_info("return_json_data: %s\n",return_json_data);
        }
        break;
    default:
        break;
    }

    return 0;
}

//=BLE ATT send======================================================================================================
// BLE ATT send
//=BLE ATT send======================================================================================================
static int app_send_user_data(u16 conn_handle, u16 handle, u8 *data, u16 len, u8 handle_type)
{
    u32 ret = APP_BLE_NO_ERROR;

    if (!conn_handle) {
        return APP_BLE_OPERATION_ERROR;
    }

    if (!multi_att_get_ccc_config(conn_handle, handle + 1)) {
        log_info("fail,no write ccc!!!,%04x\n", handle + 1);
        return APP_BLE_NO_WRITE_CCC;
    }

    ret = ble_op_multi_att_send_data(conn_handle, handle, data, len, handle_type);

    if (ret == BLE_BUFFER_FULL) {
        ret = APP_BLE_BUFF_FULL;
    }

    if (ret) {
        log_info("app_send_fail:%d !!!\n", ret);
    }
    return ret;
}


static int send_return_json_data(const uint8_t * return_json_data)
{

    if (return_json_data == NULL || return_json_data[0] == '\0' || strnlen((const char *)return_json_data, BUFFER_SIZE + 1) > BUFFER_SIZE) {
        log_info("Error: Empty or NULL return_json_data or return_json_data_size over maxlen\n");
        return -1;
    }

    const size_t data_length = strnlen((const char *)return_json_data, BUFFER_SIZE);
    const size_t payload_size = PAYLOAD_SIZE;
    const size_t total_packet_size = MAX_PACKET_SIZE;
    const size_t max_packets = MAX_PACKETS;

    // 计算总分包数
    size_t num_packets = (data_length + payload_size - 1) / payload_size;
    if (num_packets > max_packets) {
        log_info("Error: Too much data to fit in %d packets\n", max_packets);
        return -1;
    }

    // 构建并发送每个分包
    for (size_t i = 0; i < num_packets; ++i) {
        unsigned char packet[total_packet_size] = {0};

        // 设置当前分包号，格式化为两位字符（例如：01, 08, 11）
        snprintf((char *)packet, 3, "%02d", (int)(i + 1));
        
        // 设置总分包数，格式化为两位字符（例如：01, 08, 11）
        snprintf((char *)&packet[2], 3, "%02d", (int)num_packets);

        // 确保当前分包号和总分包数正确设置
        if (strlen((const char *)packet) != 4) {
            log_info("Error: Failed to format packet numbers correctly\n");
            return -1;
        }

        // 填充真实数据，并确保每次分包的真实数据部分是16字节
        size_t copy_size = (i == num_packets - 1) ? (data_length - i * payload_size) : payload_size;
        memcpy(&packet[4], &return_json_data[i * payload_size], copy_size);

        // 如果是最后一包且数据不足16字节，用0x00填充
        if (i == num_packets - 1 && copy_size < payload_size) {
            memset(&packet[4 + copy_size], 0x00, payload_size - copy_size);
        }
        
        log_info("send_packet: %s\r\n", packet);
        // if ((i % 15) == 0)
        // {
        //     ble_op_att_clear_send_data();   
        // }
        // 发送完整的分包数据
        if (app_send_user_data_do(packet, total_packet_size) != 0) {
            log_info("Error: Failed to send packet %d\n", i + 1);
            return -1;
        }
    }
    return 0;
}
//=BLE ADV===============================================================================================
// BLE ADV
//=BLE ADV===============================================================================================
static int make_set_adv_data(void)
{
    u8 offset = 0;
    u8 *buf = adv_data;

    /* offset += make_eir_packet_val(&buf[offset], offset, HCI_EIR_DATATYPE_FLAGS, 0x18, 1); */
    /* offset += make_eir_packet_val(&buf[offset], offset, HCI_EIR_DATATYPE_FLAGS, 0x1A, 1); */
    offset += make_eir_packet_val(&buf[offset], offset, HCI_EIR_DATATYPE_FLAGS, 0x06, 1);
    // 把 UUID 字段放在前面，确保最后 1 字节是 t/m 标记（给小程序 advertisData 取“最后一字节”用）
    offset += make_eir_packet_val(&buf[offset], offset, HCI_EIR_DATATYPE_COMPLETE_16BIT_SERVICE_UUIDS, 0xAF30, 2);
    /*
    * Put SN into manufacturer specific data (AD type 0xFF) so the mini-program can read it from advertisData.
    *
    * Format (matches scanning side in lwl_eend_device.c):
    *   [0] 0xE3
    *   [1] 0xFA
    *   [2] ctrl/state (1 byte, reserved; set to 0 for now)
    *   [3..18] SN (16 bytes: 15 ASCII chars + '\0')
    *   [19] t/m flag (t=0, m=1)
    */
    {
        char sn[16] = {0};
        // 即便 SN 获取失败也照样广播结构，避免小程序解析缺字段
        get_SN(sn);
        u8 mfg[3 + 16 + 1] = {0};
        mfg[0] = 0xE3;
        mfg[1] = 0xFA;
        mfg[2] = 0x00;
        memcpy(&mfg[3], sn, 16);
        mfg[3 + 16] = adv_tm_flag; // 末尾追加：t=0, m=1
        offset += make_eir_packet_data(&buf[offset], offset,
                                        HCI_EIR_DATATYPE_MANUFACTURER_SPECIFIC_DATA,
                                        mfg, sizeof(mfg));
    }

    if (offset > ADV_RSP_PACKET_MAX) {
        log_info("***adv_data overflow!!!!!!\n");
        return -1;
    }
    log_info("adv_data(%d):", offset);
    log_info_hexdump(buf, offset);
    adv_data_len = offset;
    ble_op_set_adv_data(offset, buf);
    return 0;
}

static int make_set_rsp_data(void)
{
    u8 offset = 0;
    u8 *buf = scan_rsp_data;

    u8 name_len = gap_device_name_len;
    u8 vaild_len = ADV_RSP_PACKET_MAX - (offset + 2);
    if (name_len > vaild_len) {
        name_len = vaild_len;
    }
    offset += make_eir_packet_data(&buf[offset], offset, HCI_EIR_DATATYPE_COMPLETE_LOCAL_NAME, (void *)gap_device_name, name_len);

    if (offset > ADV_RSP_PACKET_MAX) {
        log_info("***rsp_data overflow!!!!!!\n");
        return -1;
    }

    log_info("rsp_data(%d):", offset);
    log_info_hexdump(buf, offset);
    scan_rsp_data_len = offset;
    ble_op_set_rsp_data(offset, buf);
    return 0;
}

//广播参数设置
static void advertisements_setup_init()
{
    uint8_t adv_type = ADV_IND;
    uint8_t adv_channel = ADV_CHANNEL_ALL;
    int   ret = 0;

    ble_op_set_adv_param(ADV_INTERVAL_MIN, adv_type, adv_channel);

    ret |= make_set_adv_data();
    ret |= make_set_rsp_data();

    if (ret) {
        log_info("advertisements_setup_init fail !!!!!!\n");
    }
}

static int set_adv_enable(void *priv, u32 en)
{
    ble_state_e next_state, cur_state;

    // if (!adv_ctrl_en && en) {
    //     return APP_BLE_OPERATION_ERROR;
    // }

    if (server_con_handle != INVALID_HANDLE) {
        return APP_BLE_OPERATION_ERROR;
    }

    if (en) {
        next_state = BLE_ST_ADV;
    } else {
        next_state = BLE_ST_IDLE;
    }

    cur_state = get_ble_work_state();

    switch (cur_state) {
    case BLE_ST_ADV:
    case BLE_ST_IDLE:
    case BLE_ST_INIT_OK:
    case BLE_ST_NULL:
    case BLE_ST_DISCONN:
        break;
    default:
        return APP_BLE_OPERATION_ERROR;
        break;
    }

    if (cur_state == next_state) {
        return APP_BLE_NO_ERROR;
    }
    log_info("adv_en:%d\n", en);
    set_ble_work_state(next_state);

    if (en) {
        advertisements_setup_init();
    }
    ble_op_adv_enable(en);

    return APP_BLE_NO_ERROR;
}

void bt_ble_adv_enable(u8 enable)
{
    set_adv_enable(0, enable);
}

//=BLE CONN===============================================================================================
// BLE CONN
//=BLE CONN===============================================================================================
static int server_disconnect()
{
    if (server_con_handle != INVALID_HANDLE) {
        if (BLE_ST_SEND_DISCONN != get_ble_work_state()) {
            log_info(">>>ble send disconnect\n");
            set_ble_work_state(BLE_ST_SEND_DISCONN);
            ble_op_disconnect(server_con_handle);
        } else {
            log_info(">>>ble wait disconnect...\n");
        }
        return APP_BLE_NO_ERROR;
    } else {
        return APP_BLE_OPERATION_ERROR;
    }
}

//=SM====================================================================================================
// SM
//=SM====================================================================================================
#define PASSKEY_ENTER_ENABLE      0 //输入passkey使能，可修改passkey
//重设passkey回调函数，在这里可以重新设置passkey
//passkey为6个数字组成，十万位、万位。。。。个位 各表示一个数字 高位不够为0
void server_deal_passkey_input(u32 *key)
{
#if 1
    u32 newkey = rand32();//获取随机数

    newkey &= 0xfffff;
    if (newkey > 999999) {
        newkey = newkey - 999999; //不能大于999999
    }
    *key = newkey; //小于或等于六位数
    log_info("set new_key= %06u\n", *key);
#else
    *key = 123456; //for debug
#endif
}

void server_cbk_sm_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size)
{
    sm_just_event_t *event = (void *)packet;
    u32 tmp32;

    switch (packet_type) {
    case HCI_EVENT_PACKET:
        switch (hci_event_packet_get_type(packet)) {
        case SM_EVENT_JUST_WORKS_REQUEST:
            sm_just_works_confirm(sm_event_just_works_request_get_handle(packet));
            log_info("Just Works Confirmed.\n");
            break;

        case SM_EVENT_PASSKEY_DISPLAY_NUMBER:
            log_info_hexdump(packet, size);
            memcpy(&tmp32, event->data, 4);
            log_info("Passkey display: %06u.\n", tmp32);
            break;

        case SM_EVENT_PASSKEY_INPUT_NUMBER:
            /*IO_CAPABILITY_KEYBOARD_ONLY 方式*/
            server_deal_passkey_input(&tmp32);
            log_info("%04x->Passkey input: %06u.\n", event->con_handle, tmp32);
            sm_passkey_input(event->con_handle, tmp32); /*update passkey*/
            break;

        case SM_EVENT_PAIR_PROCESS:
            log_info("======PAIR_PROCESS: %02x\n", event->data[0]);
            log_info_hexdump(event->data, 4);
            break;

        }
        break;
    }
}

//------------------------------------------------------------------------------
//------------------------------------------------------------------------------
//------------------------------------------------------------------------------
//------------------------------------------------------------------------------

//=BLE init==============================================================================================
// BLE init
//=BLE init==============================================================================================
void server_profile_init(void)
{
    log_info("***** wifimgr ble profile init******\n");
    /* setup ATT server */
    att_server_init(profile_data, att_read_callback, att_write_callback);
    att_server_register_packet_handler(server_cbk_packet_handler);
}

void ble_simple_server_disconnect(void)
{
    int count = 150;
    if (server_con_handle != INVALID_HANDLE) {
        server_disconnect();
        while (count-- && (server_con_handle != INVALID_HANDLE)) {
            os_time_dly(1);
            putchar('w');
        }
    }
}

// void ble_simple_server_adv_enable(u8 en)
// {
//     bt_ble_adv_enable(en);
// }

void ble_simple_server_module_enable(u8 en)
{
    os_mutex_pend(&mutex, 0);
    log_info("mode_en:%d\n", en);
    if (en) {
        adv_ctrl_en = 1;
        bt_ble_adv_enable(1);
    } else {
        adv_ctrl_en = 0;
        ble_simple_server_disconnect();
        bt_ble_adv_enable(0);
    }
    os_mutex_post(&mutex);
}

void ble_simple_server_adv_enable(u8 en)
{
    os_mutex_pend(&mutex, 0);
    log_info("mode_en:%d\n", en);
    if (en) {
        adv_ctrl_en = 1;
        bt_ble_adv_enable(1);
    } else {
        adv_ctrl_en = 0;
        bt_ble_adv_enable(0);
    }
    os_mutex_post(&mutex);
}

void ble_simple_server_name_enable(u8 idle_busy)
{
    log_info("***** wifimgr_ble_name_enable******\n");
    assert(os_mutex_valid(&mutex));
    os_mutex_pend(&mutex, 0);
    ble_simple_server_module_enable(0);
    char *name_p = bt_get_local_name();
    const char* ext_name = idle_busy ? ble_ext_busy : ble_ext_idle;
    u8 ext_name_len = 3;
    // 同步广播末尾标记：t=0, m=1
    adv_tm_flag = idle_busy ? 1 : 0;

    memset(gap_device_name, 0, BT_NAME_LEN_MAX+1);
    
    //先把name_p开头的JL-AC79XX-这几个字符替换成KQQ，无需验证JL-AC79XX-
    //然后复制到gap_device_name中
    if (strncmp(name_p, "JL-AC79XX-", 9) == 0) {
        memcpy(gap_device_name, "KQQ", 3);
        char sn[SN_SIZE+1];
        memset(sn, 0, SN_SIZE+1);
        get_SN(sn);
        memcpy(&gap_device_name[3], sn+2, SN_SIZE-2); //跳过前两位KC
        gap_device_name_len = strlen(gap_device_name);
        memcpy(&gap_device_name[gap_device_name_len], LOCAL_VERSION_CODE,  strlen(LOCAL_VERSION_CODE));
    } else {
        memcpy(gap_device_name, name_p, strlen(name_p));
    }
    gap_device_name_len = strlen(gap_device_name);
    memcpy(&gap_device_name[gap_device_name_len], ext_name, ext_name_len);
    gap_device_name_len +=  ext_name_len;
    gap_device_name[gap_device_name_len] = 0;//结束符

    
    log_info("ble name(%d): %s \n", gap_device_name_len, gap_device_name);
    set_ble_work_state(BLE_ST_INIT_OK);
    ble_simple_server_module_enable(1);
    os_mutex_post(&mutex);
}

void ble_simple_server_init(void)
{
    log_info("***** wifimgr_ble_init******\n");
    log_i("【debug】蓝牙初始化: ble_simple_server_init");

    if (!os_mutex_valid(&mutex)) {
        os_mutex_create(&mutex);
    }
    ble_simple_server_name_enable(0);
}

void ble_simple_server_exit(void)
{
    log_info("***** ble_exit******\n");

    ble_simple_server_module_enable(0);
}


/* 获取ble与小程序是否断开 */
int get_ble_app_disconnect_flag(void)
{
    if (server_con_handle == INVALID_HANDLE) {
        return 1;
    }
    else if (server_con_handle != INVALID_HANDLE) {
        return 0;
    }
}

/* 通知 APP（小程序） 相关 函数*/
int notify_app_wifi_ip_succ(void *pri)
{
    int ret = 0;
    char cmd_name[32] = {0};
    char sta_localip[WIFI_IP_LEN] = {0};
    char return_errstr[32] = {0};
    char return_json_data[BUFFER_SIZE] = {0};

    get_wifi_ip_info(sta_localip);

    // memcpy(return_errstr, "MCU_GET_IP_SUCC", strlen("MCU_GET_IP_SUCC"));
    // memcpy(cmd_name, "mcu_notify_app_ip", strlen("mcu_notify_app_ip"));

    memcpy(return_errstr, "WIFI_CONNECT_CODE_SUCCESS", strlen("WIFI_CONNECT_CODE_SUCCESS"));
    memcpy(cmd_name, "wifi_connect", strlen("wifi_connect"));

    // 封装return_json_data
    cJSON *root = cJSON_CreateObject();
    if (root == NULL)
    {
        ret = 1;
        log_info("Error: Failed to create cJSON object\n");
        goto cleanup;
    }
    char sn[SN_SIZE+1];
    get_SN(sn);

    cJSON_AddStringToObject(root, "cmd_name", cmd_name);
    cJSON_AddNumberToObject(root, "return_code", ret); // 添加返回码
    cJSON_AddStringToObject(root, "return_errstr", return_errstr);
    cJSON_AddStringToObject(root, "sn", sn);
    cJSON_AddStringToObject(root, "ip_address", sta_localip);

    char *return_json_str = cJSON_PrintUnformatted(root);
    if (return_json_str == NULL)
    {
        ret = 1;
        log_info("Error: Failed to print cJSON object\n");
        cJSON_Delete(root);
        goto cleanup;
    }

    memcpy(return_json_data, return_json_str, strlen(return_json_str));
    send_return_json_data(return_json_data);

    free(return_json_str);
    log_info("notify  app  the wifi ip is ok   return_json_str:%s\r\n",return_json_str);
cleanup:
    return ret;
}
 
int notify_app_wifi_connect_fail(void *pri)
{
    int ret = 0;
    char cmd_name[32] = {0};
    char return_errstr[64] = {0};
    char return_json_data[BUFFER_SIZE] = {0};
    WIFI_connect_STATUES connect_status;

    // 检查BLE连接状态
    if (get_ble_app_disconnect_flag() != 0) {
        log_info("BLE not connected, skip wifi connect fail notification\n");
        return 0;
    }
    // 获取当前WiFi连接状态和命令状态
    connect_status = get_wifi_connect_status(NULL);
    // 设置命令名称
    memcpy(cmd_name, "wifi_connect", strlen("wifi_connect"));

    // 只区分密码错误
    if (connect_status == WIFI_CONNECT_ASSOCIAT_FAIL) {
        // 密码错误
        memcpy(return_errstr, "WIFI_CONNECT_CODE_PASSWORD_ERROR", strlen("WIFI_CONNECT_CODE_PASSWORD_ERROR"));
        ret = 1;
    }else {
        // 如果不是错误状态，不发送通知
        log_info("Not an error status, skip notification: %d\n", connect_status);
        return 0;
    }

    // 封装return_json_data
    cJSON *root = cJSON_CreateObject();
    if (root == NULL)
    {
        ret = 1;
        log_info("Error: Failed to create cJSON object\n");
        goto cleanup;
    }

    cJSON_AddStringToObject(root, "cmd_name", cmd_name);
    cJSON_AddNumberToObject(root, "return_code", ret);
    cJSON_AddStringToObject(root, "return_errstr", return_errstr);

    char *return_json_str = cJSON_PrintUnformatted(root);
    if (return_json_str == NULL)
    {
        ret = 1;
        log_info("Error: Failed to print cJSON object\n");
        cJSON_Delete(root);
        goto cleanup;
    }

    memcpy(return_json_data, return_json_str, strlen(return_json_str));
    send_return_json_data(return_json_data);

    free(return_json_str);
    log_info("notify app wifi connect fail: %s, return_json_str:%s\r\n", return_errstr, return_json_data);
cleanup:
    if (root != NULL) {
        cJSON_Delete(root);
    }
    return ret;
}

int notify_app_wifi_mqtt_succ(void *pri)
{
    int ret = 0;
    char cmd_name[32] = {0};
    char return_errstr[32] = {0};
    char return_json_data[BUFFER_SIZE] = {0};

    memcpy(return_errstr, "MCU_WIFI_TASK_SUCC", strlen("MCU_WIFI_TASK_SUCC"));
    memcpy(cmd_name, "mcu_notify_app_mqtt", strlen("mcu_notify_app_mqtt"));


    // 封装return_json_data
    cJSON *root = cJSON_CreateObject();
    if (root == NULL)
    {
        ret = 1;
        log_info("Error: Failed to create cJSON object\n");
        goto cleanup;
    }

    cJSON_AddStringToObject(root, "cmd_name", cmd_name);
    cJSON_AddStringToObject(root, "return_errstr", return_errstr);

    char *return_json_str = cJSON_PrintUnformatted(root);
    if (return_json_str == NULL)
    {
        ret = 1;
        log_info("Error: Failed to print cJSON object\n");
        cJSON_Delete(root);
        goto cleanup;
    }
    memcpy(return_json_data, return_json_str, strlen(return_json_str));
    send_return_json_data(return_json_data);

    free(return_json_str);
    log_info("notify app the wifi task ok ---- return_json_str:%s\r\n",return_json_data);

    msleep(500);
    /* 发送完毕，使其断开连接 */
    server_disconnect();
cleanup:
    return ret;
}

// 内部实现函数，支持强制推送参数
static int notify_app_wifi_list_update_internal(void *pri, bool force_push);

int notify_app_wifi_list_update(void *pri)
{
    return notify_app_wifi_list_update_internal(pri, false);
}

// 内部实现函数，支持强制推送参数
static int notify_app_wifi_list_update_internal(void *pri, bool force_push)
{
    int ret = 0;
    char cmd_name[32] = {0};
    char return_errstr[32] = {0};
    char return_json_data[BUFFER_SIZE] = {0};

    log_info("notify_app_wifi_list_update called (force_push=%d)\n", force_push ? 1 : 0);

    // 检查BLE连接状态
    int disconnect_flag = get_ble_app_disconnect_flag();
    log_info("BLE disconnect flag=%d\n", disconnect_flag);
    if (disconnect_flag != 0)
    {
        log_info("BLE not connected, skip wifi list update notification\n");
        return -1;
    }

    // 如果不是强制推送，检查是否在扫描状态（同时检查命令状态和扫描状态）
    if (!force_push)
    {
        WIFI_CMD_STATUES cmd_status = get_wifi_cmd_status(NULL);
        WIFI_SCAN_STATUES scan_status = get_wifi_scan_status(NULL);
        log_info("WiFi cmd_status=%d (WIFI_CMD_SCANNING=%d), scan_status=%d (WIFI_SCAN_RUNNING=%d)\n", 
                 cmd_status, WIFI_CMD_SCANNING, scan_status, WIFI_SCAN_RUNNING);
        
        // 如果命令状态是扫描中，或者扫描状态是运行中，都发送通知
        if (cmd_status != WIFI_CMD_SCANNING && scan_status != WIFI_SCAN_RUNNING)
        {
            log_info("WiFi not scanning (cmd=%d, scan=%d), skip wifi list update notification\n", 
                     cmd_status, scan_status);
            return -1;
        }
    }
    else
    {
        log_info("Force push mode: skipping scan status check\n");
    }

    log_info("All checks passed, sending wifi_list_update notification\n");

    memcpy(cmd_name, "wifi_list_update", strlen("wifi_list_update"));

    // 封装return_json_data
    cJSON *root = cJSON_CreateObject();
    if (root == NULL)
    {
        ret = 1;
        log_info("Error: Failed to create cJSON object\n");
        goto cleanup;
    }

    cJSON_AddStringToObject(root, "cmd_name", cmd_name);

    // 使用公共函数构建WiFi列表（使用DETAILS_FULL获取完整信息）
    int is_empty = 0;
    ret = build_wifi_list_json(root, DETAILS_FULL, &is_empty);
    if (ret != 0)
    {
        log_info("Error: Failed to build wifi list json\n");
        cJSON_Delete(root);
        ret = 1;
        goto cleanup;
    }

    // 根据列表是否为空设置错误码
    if (is_empty)
    {
        memcpy(return_errstr, "WIFI_LIST_UPDATE_CODE_EMPTY", strlen("WIFI_LIST_UPDATE_CODE_EMPTY"));
    }
    else
    {
        memcpy(return_errstr, "WIFI_LIST_UPDATE_CODE_SUCCESS", strlen("WIFI_LIST_UPDATE_CODE_SUCCESS"));
    }

    cJSON_AddNumberToObject(root, "return_code", ret);
    cJSON_AddStringToObject(root, "return_errstr", return_errstr);

    char *return_json_str = cJSON_PrintUnformatted(root);
    if (return_json_str == NULL)
    {
        ret = 1;
        log_info("Error: Failed to print cJSON object\n");
        cJSON_Delete(root);
        goto cleanup;
    }

    memcpy(return_json_data, return_json_str, strlen(return_json_str));
    int send_ret = send_return_json_data(return_json_data);
    if (send_ret != 0) {
        ret = send_ret;
        log_info("Error: Failed to send wifi list update notification\n");
    }

    free(return_json_str);
    log_info("notify app wifi list update ---- return_json_str:%s\r\n", return_json_data);
    
    // 如果是强制推送（首次推送）且成功，记录日志
    if (force_push && ret == 0) {
        log_info("[blee-ble] Initial WiFi list push successful\n");
    }

cleanup:
    return ret;
}

// 强制推送WiFi列表（连接后立即推送，跳过扫描状态检查）
int notify_app_wifi_list_update_force(void *pri)
{
    return notify_app_wifi_list_update_internal(pri, true);
}
