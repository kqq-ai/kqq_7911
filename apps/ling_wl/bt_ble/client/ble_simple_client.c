#include "ble_simple_client.h"

#include "app_config.h"

#include "btstack/btstack_task.h"
#include "btstack/bluetooth.h"
#include "bt_common.h"

#include "le_common.h"
#include "ble_user.h"
#include "syscfg/syscfg_id.h"

#include "bt_ble/ble_combined.h"

#define SECURITY_EN 1

#define SUPPORT_TEST_BOX_BLE_MASTER_TEST_EN	1
#define SHOW_RX_DATA_RATE           0
#define EXT_ADV_MODE_EN             0

// LOG 暂时采用printf 而不是log.h的函数
#if 1
extern void printf_buf(u8 *buf, u32 len);
#define log_info          printf
#define log_info_hexdump  printf_buf
#else
#define log_info(...)
#define log_info_hexdump(...)
#endif

//---------------
#define SEARCH_PROFILE_BUFSIZE    (512)                   //note:
static u8 search_ram_buffer[SEARCH_PROFILE_BUFSIZE] __attribute__((aligned(4)));
#define scan_buffer search_ram_buffer
//---------------
#define BASE_INTERVAL_MIN         (6)//最小的interval
#if (SUPPORT_MAX_CLIENT > 2)
#define BASE_INTERVAL_VALUE       (BASE_INTERVAL_MIN*4)
#else
#define BASE_INTERVAL_VALUE       (BASE_INTERVAL_MIN)
#endif

//搜索类型
#define SET_SCAN_TYPE       SCAN_ACTIVE
//搜索 周期大小
#define SET_SCAN_INTERVAL   ADV_SCAN_MS(24) // unit: ms
#define SET_SCAN_WINDOW     ADV_SCAN_MS(8) // unit: ms
//连接周期
#define SET_CONN_INTERVAL   (BASE_INTERVAL_MIN*8) //(unit:1.25ms)
//连接latency
#define SET_CONN_LATENCY    0  //(unit:conn_interval)
//连接超时
#define SET_CONN_TIMEOUT    400 //(unit:10ms)

// 仅用于日志打印
static const char *const phy_result[] = {
    "None",
    "1M",
    "2M",
    "Coded",
};


//----------------------------------------------------------------------------
static u8 scan_ctrl_en;
static u8 ble_work_state = BLE_ST_NULL;
static void (*app_ble_state_callback)(void *priv, ble_state_e state) = NULL;

static u8 sm_master_pair_redo = 0;//keymiss,是否重新执行配对
//-------------------------------------------------------------------------------

//记录handle 使用

static void bt_ble_create_connection(u8 *conn_addr, u8 addr_type);
static int bt_ble_scan_enable(void *priv, u32 en);
static int client_operation_send(u16 handle, u8 *data, u16 len, u8 att_op_type);
int ble_enable_new_dev_scan(void);
//---------------------------------------------------------------------------

static const struct conn_update_param_t connection_param_table[] = {
    {16, 24, 0, 600},//11
    {12, 28, 0, 600},//3.7
    {8,  20, 0, 600},
    {50, 60, 0, 600},
};

static int client_create_connect_api(u8 *addr, u8 addr_type);
static int client_create_cannel_api(void);

//------------------------------------------------------------
static void set_ble_work_state(ble_state_e state)
{
    if (state != ble_work_state) {
        log_info("ble_client_work_st:%x->%x\n", ble_work_state, state);
        ble_work_state = state;
        if (app_ble_state_callback) {
            app_ble_state_callback(NULL, state);
        }
    }
}
//------------------------------------------------------------
static ble_state_e get_ble_work_state()
{
    return ble_work_state;
}

//协议栈内部调用
//return: 0--accept,1--reject
int l2cap_connection_update_request_just(u8 *packet, hci_con_handle_t handle)
{
    log_info("slave request conn_update:\n-conn_handle= %04x\n-interval_min= %d,\n-interval_max= %d,\n-latency= %d,\n-timeout= %d\n",
             handle,
             little_endian_read_16(packet, 0), little_endian_read_16(packet, 2),
             little_endian_read_16(packet, 4), little_endian_read_16(packet, 6));

    //change param
    /* little_endian_store_16(packet, 4,0);//disable latency */
    /* little_endian_store_16(packet, 6,400);//change timeout */
    return 0;
    /* return 1; */
}

/*************************************************************************************************/
/*!
 *  \brief      协议栈回调搜索descriptor结果
 *
 *  \param      [in]    搜索结果
 *
 *  \return
 *
 *  \note      搜索charactc包含descriptor
 */
/*************************************************************************************************/
void user_client_report_descriptor_result(charact_descriptor_t *result_descriptor)
{
    log_info("report_descriptor,handle= %04x ,uuid16: %04x\n", result_descriptor->handle, result_descriptor->uuid16);
    if (result_descriptor->uuid16 == 0) {
        log_info("uuid128:");
        log_info_hexdump(result_descriptor->uuid128, 16);
    }
}

/*************************************************************************************************/
/*!
 *  \brief      协议栈回调搜索 service & charactc的结果
 *
 *  \param      [in]    搜索结果
 *
 *  \return
 *
 *  \note      每搜索到一个server 或 charactc uuid 都会调用,直到搜索结束
 */
/*************************************************************************************************/
//协议栈内部调用
void user_client_report_search_result(search_result_t *result_info)
{
    if (result_info == (void *) -1) {
        log_info("client_report_search_result finish!!!\n");
        return;
    }

    log_info("\n*** services, uuid16:%04x,index=%d ***\n", result_info->services.uuid16, result_info->service_index);
    log_info("{charactc, uuid16:%04x,index=%d,handle:%04x~%04x,value_handle=%04x}\n",
             result_info->characteristic.uuid16, result_info->characteristic_index,
             result_info->characteristic.start_handle, result_info->characteristic.end_handle,
             result_info->characteristic.value_handle
            );

    if (!result_info->services.uuid16) {
        log_info("######services_uuid128:");
        log_info_hexdump(result_info->services.uuid128, 16);
    }

    if (!result_info->characteristic.uuid16) {
        log_info("######charact_uuid128:");
        log_info_hexdump(result_info->characteristic.uuid128, 16);
    }
}

//协议栈内部调用
void user_client_report_data_callback(att_data_report_t *report_data)
{
    /* log_info("\n-report_data:type %02x,handle %04x,offset %d,len %d:",report_data->packet_type, */
    /* 		report_data->value_handle,report_data->value_offset,report_data->blob_length); */
    /* log_info_hexdump(report_data->blob,report_data->blob_length); */

    if (report_data->conn_handle != client_con_handle) {
        log_info("unknown_handle:%04x\n", report_data->conn_handle);
        log_info_hexdump(report_data->blob, report_data->blob_length);
        return;
    }

    switch (report_data->packet_type) {
    case GATT_EVENT_NOTIFICATION://notify
//        log_info("\n-notify_rx(%d):",report_data->blob_length);
    case GATT_EVENT_INDICATION://indicate
    case GATT_EVENT_CHARACTERISTIC_VALUE_QUERY_RESULT://read
    case GATT_EVENT_LONG_CHARACTERISTIC_VALUE_QUERY_RESULT://read long
        break;
    default:
        break;
    }
}

static void client_search_profile_start(u16 conn_handle)
{
    if (conn_handle != client_con_handle) {
        log_info("error search_cid!!! :%04x\n", conn_handle);
        return;
    }

    user_client_init(client_con_handle, search_ram_buffer, SEARCH_PROFILE_BUFSIZE);

    log_info("start search_profile_all:%04x\n", client_con_handle);
    ble_op_search_profile_all();
}

//------------------------------------------------------------
static bool resolve_adv_report(u8 *adv_address, u8 data_length, u8 *data, s8 rssi)
{
    u8 i, lenght, ad_type;
    u8 *adv_data_pt;
    u8 find_remoter = 0;
    /* u8 tmp_addr[6]; */
    u32 tmp32;

    adv_data_pt = data;
    for (i = 0; i < data_length;) {
        if (*adv_data_pt == 0) {
            /* log_info("analyze end\n"); */
            break;
        }

        lenght = *adv_data_pt++;

        if (lenght >= data_length || (lenght + i) >= data_length) {
            /*过滤非标准包格式*/
            printf("!!!error_adv_packet:");
            put_buf(data, data_length);
            break;
        }

        ad_type = *adv_data_pt++;
        i += (lenght + 1);

        switch (ad_type) {
        case HCI_EIR_DATATYPE_FLAGS:
            /* log_info("flags:%02x\n",adv_data_pt[0]); */
            break;

        case HCI_EIR_DATATYPE_MORE_16BIT_SERVICE_UUIDS:
        case HCI_EIR_DATATYPE_COMPLETE_16BIT_SERVICE_UUIDS:
        case HCI_EIR_DATATYPE_MORE_32BIT_SERVICE_UUIDS:
        case HCI_EIR_DATATYPE_COMPLETE_32BIT_SERVICE_UUIDS:
        case HCI_EIR_DATATYPE_MORE_128BIT_SERVICE_UUIDS:
        case HCI_EIR_DATATYPE_COMPLETE_128BIT_SERVICE_UUIDS:
            /* log_info("service uuid:"); */
            /* log_info_hexdump(adv_data_pt, lenght - 1); */
            break;

        case HCI_EIR_DATATYPE_COMPLETE_LOCAL_NAME:
        case HCI_EIR_DATATYPE_SHORTENED_LOCAL_NAME:
            tmp32 = adv_data_pt[lenght - 1];
            adv_data_pt[lenght - 1] = 0;;
            log_info("ble remoter_name: %s,rssi:%d\n", adv_data_pt, rssi);
            /* log_info_hexdump(adv_address, 6); */
            adv_data_pt[lenght - 1] = tmp32;
            break;

        case HCI_EIR_DATATYPE_MANUFACTURER_SPECIFIC_DATA:
            break;

        case HCI_EIR_DATATYPE_APPEARANCE_DATA:
            break;
        default:
            break;
        }

        if (find_remoter) {
            log_info_hexdump(adv_data_pt, lenght - 1);
        }
        adv_data_pt += (lenght - 1);
    }

    return find_remoter;
}

static void client_report_adv_data(adv_report_t *report_pt, u16 len)
{
    bool find_tag = resolve_adv_report(report_pt->address, report_pt->length, report_pt->data, report_pt->rssi);
    if (find_tag) {
        log_info("rssi:%d\n", report_pt->rssi);
        log_info("\n*********create_connection***********\n");
        log_info("***remote type %d,addr:", report_pt->address_type);
        log_info_hexdump(report_pt->address, 6);
        bt_ble_scan_enable(0, 0);
        client_create_connect_api(report_pt->address, report_pt->address_type);
        log_info("*create_finish\n");
    }
}

static void client_create_connection(u8 *conn_addr, u8 addr_type)
{
    struct create_conn_param_t *create_conn_par = (struct create_conn_param_t *)scan_buffer;
    if (get_ble_work_state() == BLE_ST_CREATE_CONN) {
        log_info("already create conn:!!!\n");
        return;
    }

    create_conn_par->conn_interval = SET_CONN_INTERVAL;
    create_conn_par->conn_latency = SET_CONN_LATENCY;
    create_conn_par->supervision_timeout = SET_CONN_TIMEOUT;

    memcpy(create_conn_par->peer_address, conn_addr, 6);
    create_conn_par->peer_address_type = addr_type;

    set_ble_work_state(BLE_ST_CREATE_CONN);
    ble_op_create_connection(create_conn_par);
}

static void bt_ble_create_connection(u8 *conn_addr, u8 addr_type)
{
    client_create_connection(conn_addr, addr_type);
}

static void client_create_connection_cannel(void)
{
    if (get_ble_work_state() == BLE_ST_CREATE_CONN) {
        set_ble_work_state(BLE_ST_SEND_CREATE_CONN_CANNEL);
        ble_op_create_connection_cancel();
    }
}

static int client_disconnect()
{
    if (client_con_handle) {
        if (BLE_ST_SEND_DISCONN != get_ble_work_state()) {
            log_info(">>>ble send disconnect\n");
            set_ble_work_state(BLE_ST_SEND_DISCONN);
            ble_op_disconnect(client_con_handle);
        } else {
            log_info(">>>ble wait disconnect...\n");
        }
        return APP_BLE_NO_ERROR;
    } else {
        return APP_BLE_OPERATION_ERROR;
    }
}

void ble_multi_client_disconnect(void)
{
    int count = 150;
    if (client_con_handle != INVALID_HANDLE) {
        client_disconnect();
        while (count-- && (client_con_handle != INVALID_HANDLE)) {
            os_time_dly(1);
            putchar('w');
        }
    }
}

static void connection_update_complete_success(u16 conn_handle, u8 *packet)
{
    int conn_interval, conn_latency, conn_timeout;

    conn_interval = hci_subevent_le_connection_update_complete_get_conn_interval(packet);
    conn_latency = hci_subevent_le_connection_update_complete_get_conn_latency(packet);
    conn_timeout = hci_subevent_le_connection_update_complete_get_supervision_timeout(packet);

    log_info("conn_handle = %04x\n", conn_handle);
    log_info("conn_interval = %d\n", conn_interval);
    log_info("conn_latency = %d\n", conn_latency);
    log_info("conn_timeout = %d\n", conn_timeout);
}

static void can_send_now_wakeup(void)
{
    /* putchar('W'); */
}

static void set_connection_data_length(u16 tx_octets, u16 tx_time)
{
    if (client_con_handle != INVALID_HANDLE) {
        ble_op_set_data_length(client_con_handle, tx_octets, tx_time);
    }
}

static void set_connection_data_phy(u8 tx_phy, u8 rx_phy)
{
    if (INVALID_HANDLE == client_con_handle) {
        return;
    }

    u8 all_phys = 0;
    u16 phy_options = 0;

    ble_op_set_ext_phy(client_con_handle, all_phys, tx_phy, rx_phy, phy_options);
}

static void client_profile_start(u16 conn_handle)
{
    set_ble_work_state(BLE_ST_CONNECT);

    client_search_profile_start(conn_handle);
}

void client_deal_passkey_input(u32 *key)
{
    //0~999999
    *key = 123456;
    log_info("new_key: %06u.\n", *key);
}

void client_cbk_sm_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size)
{
    sm_just_event_t *event = (void *)packet;
    u32 tmp32;
    //log_info("%s\n", __FUNCTION__);

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
            client_deal_passkey_input(&tmp32);
            log_info("%04x->Passkey input: %06u.\n", event->con_handle, tmp32);
            sm_passkey_input(event->con_handle, tmp32); /*update passkey*/
            break;

        case SM_EVENT_PAIR_PROCESS:
            log_info("======PAIR_PROCESS: %02x\n", event->data[0]);
            put_buf(event->data, 4);
            break;
        }
        break;
    }
}

/* LISTING_START(packetHandler): Packet Handler */
void client_cbk_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size)
{
    int mtu;
    u32 tmp;
    u8 status;

    switch (packet_type) {
    case HCI_EVENT_PACKET:
        switch (hci_event_packet_get_type(packet)) {

        /* case DAEMON_EVENT_HCI_PACKET_SENT: */
        /* break; */
        case ATT_EVENT_CAN_SEND_NOW:
            can_send_now_wakeup();
            break;

        case HCI_EVENT_LE_META:
            switch (hci_event_le_meta_get_subevent_code(packet)) {

            case HCI_SUBEVENT_LE_ENHANCED_CONNECTION_COMPLETE: {
                if (1 == hci_subevent_le_connection_complete_get_role(packet)) {
                    //connect is slave
                    break;
                }

                status = hci_subevent_le_enhanced_connection_complete_get_status(packet);
                if (status) {
                    log_info("LE_MASTER CREATE CONNECTION FAIL!!! %0x\n", status);
                    set_ble_work_state(BLE_ST_DISCONN);
                    break;
                }
                u16 handle = hci_subevent_le_enhanced_connection_complete_get_connection_handle(packet);
                log_info("HCI_SUBEVENT_LE_ENHANCED_CONNECTION_COMPLETE : 0x%0x\n", handle);

                if (client_con_handle != INVALID_HANDLE) {
                    ble_op_disconnect(handle);
                    break;
                }

                client_con_handle = handle;
                ble_op_multi_att_send_conn_handle(client_con_handle, 0, MULTI_ROLE_CLIENT);

                log_info("conn_interval = %d\n", hci_subevent_le_enhanced_connection_complete_get_conn_interval(packet));
                log_info("conn_latency = %d\n", hci_subevent_le_enhanced_connection_complete_get_conn_latency(packet));
                log_info("conn_timeout = %d\n", hci_subevent_le_enhanced_connection_complete_get_supervision_timeout(packet));
                client_profile_start(client_con_handle);
            }
            break;

            case HCI_SUBEVENT_LE_CONNECTION_COMPLETE: {
                if (1 == hci_subevent_le_connection_complete_get_role(packet)) {
                    //connect is slave
                    break;
                }

                if (packet[3]) {
                    log_info("LE_MASTER CREATE CONNECTION FAIL!!! %0x\n", packet[3]);
                    set_ble_work_state(BLE_ST_DISCONN);
                    //auto to do
                    bt_ble_scan_enable(0, 1);
                    break;
                }

                u16 handle = hci_subevent_le_connection_complete_get_connection_handle(packet);
                log_info("HCI_SUBEVENT_LE_CONNECTION_COMPLETE: %0x\n", handle);

                if (client_con_handle != INVALID_HANDLE) {
                    ble_op_disconnect(handle);
                    break;
                }

                client_con_handle = handle;
                ble_op_multi_att_send_conn_handle(client_con_handle, 0, MULTI_ROLE_CLIENT);

                connection_update_complete_success(handle, packet + 8);
                client_profile_start(client_con_handle);
            }
            break;

            case HCI_SUBEVENT_LE_CONNECTION_UPDATE_COMPLETE: {
                u16 handle = little_endian_read_16(packet, 4);
                if (client_con_handle == handle) {
                    connection_update_complete_success(handle, packet);
                }
            }
            break;

            case HCI_SUBEVENT_LE_DATA_LENGTH_CHANGE:
                log_info("APP HCI_SUBEVENT_LE_DATA_LENGTH_CHANGE");
                break;

            case HCI_SUBEVENT_LE_PHY_UPDATE_COMPLETE:
                log_info("APP HCI_SUBEVENT_LE_PHY_UPDATE %s\n", hci_event_le_meta_get_phy_update_complete_status(packet) ? "Fail" : "Succ");
                log_info("Tx PHY: %s\n", phy_result[hci_event_le_meta_get_phy_update_complete_tx_phy(packet)]);
                log_info("Rx PHY: %s\n", phy_result[hci_event_le_meta_get_phy_update_complete_rx_phy(packet)]);
                break;
            }
            break;

        case HCI_EVENT_DISCONNECTION_COMPLETE: {
            log_info("HCI_EVENT_DISCONNECTION_COMPLETE: %0x\n", packet[5]);

            u16 handle = little_endian_read_16(packet, 3);
            if (client_con_handle != handle) {
                log_info("unknown_handle:%04x\n", handle);
                break;
            }

            client_con_handle = INVALID_HANDLE;
            set_ble_work_state(BLE_ST_DISCONN);
            ble_op_multi_att_send_conn_handle(client_con_handle, 0, MULTI_ROLE_CLIENT);

            if (INVALID_HANDLE == client_con_handle) {
                //判断当前设备是否正在scan 或者 creat
                if (BLE_ST_SCAN == get_ble_work_state() || BLE_ST_CREATE_CONN == get_ble_work_state()) {
                    //由于连上一台和未连接的时候scan参数可能不一致，所以有链路断开时，则先关闭已有scan再打开;
                    log_info("doing\n");
                    bt_ble_scan_enable(NULL, 0);
                }
            }
            bt_ble_scan_enable(0, 1);
        }
        break;

        case ATT_EVENT_MTU_EXCHANGE_COMPLETE: {
            u16 handle = little_endian_read_16(packet, 2);
            if (client_con_handle == handle) {
                mtu = att_event_mtu_exchange_complete_get_MTU(packet) - 3;
                log_info("ATT MTU = %u\n", mtu);
                ble_op_multi_att_set_send_mtu(handle, mtu);
                /* set_connection_data_length(251, 2120); */
            }
        }
        break;

        case HCI_EVENT_VENDOR_REMOTE_TEST:
            log_info("--- HCI_EVENT_VENDOR_REMOTE_TEST\n");
            break;

        case L2CAP_EVENT_CONNECTION_PARAMETER_UPDATE_RESPONSE: {
            u16 handle = little_endian_read_16(packet, 2);
            if (client_con_handle == handle) {
                tmp = little_endian_read_16(packet, 4);
                log_info("-update_rsp:%04x, %02x\n", handle, tmp);
            }
        }
        break;

        case GAP_EVENT_ADVERTISING_REPORT:
            /* putchar('@'); */
            if (BLE_ST_SCAN == get_ble_work_state()) {
                client_report_adv_data((void *)&packet[2], packet[1]);
            } else {
                log_info("drop adv_report!!!%02x\n", get_ble_work_state());
            }
            break;

        case HCI_EVENT_ENCRYPTION_CHANGE: {
            u16 handle = little_endian_read_16(packet, 3);
            log_info("HCI_EVENT_ENCRYPTION_CHANGE= %d,handle=%04x\n", packet[2], handle);

            if (packet[2]) {
                log_info("fail,break\n");
                break;
            }

            if (client_con_handle == handle) {
                client_search_profile_start(handle);
            }
        }
        break;
        }
        break;
    }
}

static int get_buffer_vaild_len(void *priv)
{
    u32 vaild_len = 0;
    ble_op_multi_att_get_remain(client_con_handle, &vaild_len);
    return vaild_len;
}

static int client_operation_send(u16 handle, u8 *data, u16 len, u8 att_op_type)
{
    int ret = APP_BLE_NO_ERROR;
    if (client_con_handle == INVALID_HANDLE) {
        return APP_BLE_OPERATION_ERROR;
    }

    if (!handle) {
        log_info("handle is null\n");
        return APP_BLE_OPERATION_ERROR;
    }

    if (get_buffer_vaild_len(0) < len) {
        log_info("opt_buff_full!!!\n");
        return APP_BLE_BUFF_FULL;
    }

    ret = ble_op_multi_att_send_data(client_con_handle, handle, data, len, att_op_type);
    if (ret == BLE_BUFFER_FULL) {
        ret = APP_BLE_BUFF_FULL;
    }
    return ret;
}

static int client_operation_send_ext(u16 conn_handle, u16 handle, u8 *data, u16 len, u8 att_op_type)
{
    int ret = APP_BLE_NO_ERROR;
    if (conn_handle == INVALID_HANDLE) {
        return APP_BLE_OPERATION_ERROR;
    }

    ret = ble_op_multi_att_send_data(conn_handle, handle, data, len, att_op_type);
    if (ret == BLE_BUFFER_FULL) {
        ret = APP_BLE_BUFF_FULL;
    }
    return ret;
}
//-----------------------------------------------
static int client_write_send(void *priv, uint16_t handle, u8 *data, u16 len)
{
    return client_operation_send(handle, data, len, ATT_OP_WRITE);
}

static int client_write_without_respond_send(void *priv, uint16_t handle, u8 *data, u16 len)
{
    return client_operation_send(handle, data, len, ATT_OP_WRITE_WITHOUT_RESPOND);
}

static int client_read_value_send(void *priv, uint16_t handle)
{
    u16 tmp_flag = 0x55A1;
    return client_operation_send(handle, (u8 *)&tmp_flag, 2, ATT_OP_READ);
}

static int client_read_long_value_send(void *priv, uint16_t handle)
{
    u16 tmp_flag = 0x55A2;
    return client_operation_send(handle, (u8 *)&tmp_flag, 2, ATT_OP_READ_LONG);
}

//扫描数设置
static void scanning_setup_init(void)
{
    ble_op_set_scan_param(SET_SCAN_TYPE, SET_SCAN_INTERVAL, SET_SCAN_WINDOW);
}

static int bt_ble_scan_enable(void *priv, u32 en)
{
    ble_state_e next_state, cur_state;

    if (!scan_ctrl_en && en) {
        return 	APP_BLE_OPERATION_ERROR;
    }

    if (en) {
        next_state = BLE_ST_SCAN;
    } else {
        next_state = BLE_ST_IDLE;
    }

    cur_state =  get_ble_work_state();
    switch (cur_state) {
    case BLE_ST_SCAN:
    case BLE_ST_IDLE:
    case BLE_ST_INIT_OK:
    case BLE_ST_NULL:
    case BLE_ST_DISCONN:
    case BLE_ST_CONNECT_FAIL:
    case BLE_ST_SEND_CREATE_CONN_CANNEL:
        break;
    default:
        return APP_BLE_OPERATION_ERROR;
        break;
    }

    if (cur_state == next_state) {
        return APP_BLE_NO_ERROR;
    }

    log_info("scan_en:%d\n", en);
    set_ble_work_state(next_state);

    if (en) {
        scanning_setup_init();
    }
    ble_op_scan_enable2(en, 0);

    return APP_BLE_NO_ERROR;
}

static int client_create_connect_api(u8 *addr, u8 addr_type)
{
    u8 cur_state =  get_ble_work_state();

    switch (cur_state) {
    case BLE_ST_SCAN:
    case BLE_ST_IDLE:
    case BLE_ST_INIT_OK:
    case BLE_ST_NULL:
    case BLE_ST_DISCONN:
    case BLE_ST_CONNECT_FAIL:
    case BLE_ST_SEND_CREATE_CONN_CANNEL:
        break;
    default:
        return APP_BLE_OPERATION_ERROR;
        break;
    }

    if (cur_state == BLE_ST_SCAN) {
        log_info("stop scan\n");
        bt_ble_scan_enable(0, 0);
    }

    log_info("addr to creat!\n");
    log_info_hexdump(addr, 7);
    bt_ble_create_connection(addr, addr_type);
    return 0;
}

static int client_create_cannel_api(void)
{
    if (get_ble_work_state() == BLE_ST_CREATE_CONN) {
        client_create_connection_cannel();
        return 0;
    }
    return 1;
}

static int get_ble_work_state_api(void)
{
    return get_ble_work_state();
}

//模块开关
void ble_simple_client_module_enable(u8 en)
{
    log_info("mode_en:%d\n", en);

    if (en) {
        scan_ctrl_en = 1;
        bt_ble_scan_enable(0, 1);
    } else {
        scan_ctrl_en = 0;
        bt_ble_scan_enable(0, 0);
        ble_multi_client_disconnect();
    }
}

void client_profile_init(void)
{
    //setup GATT client
    gatt_client_init();
    gatt_client_register_packet_handler(client_cbk_packet_handler);

    sm_set_master_pair_redo(sm_master_pair_redo);
}

void ble_simple_client_init(void)
{
    ble_work_state = 0;
    client_con_handle = INVALID_HANDLE;

    set_ble_work_state(BLE_ST_INIT_OK);
    ble_simple_client_module_enable(1);
}

void ble_simple_client_exit(void)
{
    ble_client_module_enable(0);
}

//主机请求更新连接参数
void ble_simple_client_send_conn_param_update(void)
{
    struct conn_update_param_t *param = (void *)&connection_param_table;//for test
    log_info("client update param:-%d-%d-%d-%d-\n", param->interval_min, param->interval_max, param->latency, param->timeout);
    if (client_con_handle != INVALID_HANDLE) {
        ble_op_conn_param_update(client_con_handle, param);
    }
}
