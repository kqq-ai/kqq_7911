#include "ble_combined.h"

#include "app_config.h"

#include "btstack/btstack_task.h"
#include "btstack/bluetooth.h"
#include "btstack/btstack_event.h"
#include "le_common.h"
#include "os/os_api.h"

#include "server/ble_simple_server.h"
#include "client/ble_simple_client.h"

// Serialize ADV/SCAN enable/disable operations across tasks.
// Without this, concurrent calls to ble_combined_apply() can race and issue
// duplicate controller enable/disable commands, which may leak "BLE HW" slots
// and trigger btctrler asserts ("BLE HW NUM limit", "LL Init already destroy").
static OS_MUTEX g_ble_combined_mutex;

static void ble_combined_lock_init_once(void)
{
    if (!os_mutex_valid(&g_ble_combined_mutex)) {
        os_mutex_create(&g_ble_combined_mutex);
    }
}

// LOG 暂时采用printf 而不是log.h的函数
#if 1
extern void printf_buf(u8 *buf, u32 len);
#define log_info          printf
#define log_info_hexdump  printf_buf
#else
#define log_info(...)
#define log_info_hexdump(...)
#endif

#define PASSKEY_ENTER_ENABLE      0 //输入passkey使能，可修改passkey
//----------------------------------------------------------------------------------------
#define MULTI_ATT_MTU_SIZE              (517) //ATT MTU的值
//ATT发送的包长,    note: 20 <= need >= MTU
#define MULTI_ATT_LOCAL_PAYLOAD_SIZE    (MULTI_ATT_MTU_SIZE)                   //
//ATT缓存的buffer大小,  note: need >= 20,可修改
#define MULTI_ATT_SEND_CBUF_SIZE        (512*3)                 //

//共配置的RAM
#define MULTI_ATT_RAM_BUFSIZE           (ATT_CTRL_BLOCK_SIZE + MULTI_ATT_LOCAL_PAYLOAD_SIZE + MULTI_ATT_SEND_CBUF_SIZE)                   //note:
static u8 multi_att_ram_buffer[MULTI_ATT_RAM_BUFSIZE] __attribute__((aligned(4)));

u16 server_con_handle = INVALID_HANDLE;
u16 client_con_handle = INVALID_HANDLE;

// Scheme B: centralized radio (ADV/SCAN) ownership.
static u8 g_want_adv;
static u8 g_want_scan;

//----------------------------------------------------------------------------------------
extern void reset_PK_cb_register_ext(void (*reset_pk)(u32 *, u16));


u8 mul_dev_get_handle_role(u16 handle) {
    return client_con_handle == handle ? MULTI_ROLE_CLIENT : MULTI_ROLE_SERVER;
}

/* LISTING_START(packetHandler): Packet Handler */
static void cbk_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size)
{
    u8 role = 0xff;
    switch (packet_type) {
    case HCI_EVENT_PACKET:
        switch (hci_event_packet_get_type(packet)) {

        case ATT_EVENT_CAN_SEND_NOW:
            if (0 == packet[1]) {
                role = MULTI_ROLE_SERVER;
            } else {
                role = MULTI_ROLE_CLIENT;
            }
            break;

        case ATT_EVENT_HANDLE_VALUE_INDICATION_COMPLETE:
            role = MULTI_ROLE_SERVER;
            break;

        case ATT_EVENT_MTU_EXCHANGE_COMPLETE:
            role = mul_dev_get_handle_role(little_endian_read_16(packet, 2));
            break;

        case GAP_EVENT_ADVERTISING_REPORT:
            role = MULTI_ROLE_CLIENT;
            break;

        case HCI_EVENT_LE_META:
            switch (hci_event_le_meta_get_subevent_code(packet)) {
            case HCI_SUBEVENT_LE_ENHANCED_CONNECTION_COMPLETE:
                if (hci_subevent_le_enhanced_connection_complete_get_role(packet)) {
                    role = MULTI_ROLE_SERVER;
                } else {
                    role = MULTI_ROLE_CLIENT;
                }
                break;

            case HCI_SUBEVENT_LE_CONNECTION_COMPLETE:
                if (hci_subevent_le_connection_complete_get_role(packet)) {
                    role = MULTI_ROLE_SERVER;
                } else {
                    role = MULTI_ROLE_CLIENT;
                }
                break;

            //参数更新完成
            case HCI_SUBEVENT_LE_CONNECTION_UPDATE_COMPLETE:
                role = mul_dev_get_handle_role(little_endian_read_16(packet, 4));
                break;

            case HCI_SUBEVENT_LE_DATA_LENGTH_CHANGE:
                role = mul_dev_get_handle_role(little_endian_read_16(packet, 3));
                break;

            case HCI_SUBEVENT_LE_PHY_UPDATE_COMPLETE:
                role = mul_dev_get_handle_role(little_endian_read_16(packet, 4));
                break;

            }
            break;

        case HCI_EVENT_DISCONNECTION_COMPLETE:
            role = mul_dev_get_handle_role(little_endian_read_16(packet, 3));
            break;

        case HCI_EVENT_VENDOR_REMOTE_TEST:
            log_info("HCI_EVENT_VENDOR_REMOTE_TEST: %d,%d\n", packet[1], packet[2]);
            break;

        case L2CAP_EVENT_CONNECTION_PARAMETER_UPDATE_RESPONSE:
            role = mul_dev_get_handle_role(little_endian_read_16(packet, 2));
            break;

        case HCI_EVENT_ENCRYPTION_CHANGE:
            role = mul_dev_get_handle_role(little_endian_read_16(packet, 3));
            break;
        }
        break;

    default:
        break;
    }

    if (role == 0xff || role == MULTI_ROLE_SERVER) {
        server_cbk_packet_handler(packet_type, channel, packet, size);
    }

    if (role == 0xff || role == MULTI_ROLE_CLIENT) {
        client_cbk_packet_handler(packet_type, channel, packet, size);
    }
}

/* #define SM_EVENT_PAIR_PROCESS        0xDF */
void cbk_sm_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size)
{
    sm_just_event_t *event = (void *)packet;
    u32 tmp32;

    switch (packet_type) {
    case HCI_EVENT_PACKET:
        switch (hci_event_packet_get_type(packet)) {
        case SM_EVENT_JUST_WORKS_REQUEST:
        case SM_EVENT_PASSKEY_DISPLAY_NUMBER:
        case SM_EVENT_PASSKEY_INPUT_NUMBER:
        case SM_EVENT_PAIR_PROCESS:
            if (mul_dev_get_handle_role(event->con_handle) == MULTI_ROLE_SERVER) {
                server_cbk_sm_packet_handler(packet_type, channel, packet, size);
            }

            if (mul_dev_get_handle_role(event->con_handle) == MULTI_ROLE_CLIENT) {
                client_cbk_sm_packet_handler(packet_type, channel, packet, size);
            }
            break;
        default:
            break;
        }
    }
}

//重设passkey回调函数，在这里可以重新设置passkey
//passkey为6个数字组成，十万位、万位。。。。个位 各表示一个数字 高位不够为0
/* static void reset_passkey_cb(u32 *key) */
static void ble_cbk_passkey_input(u32 *key, u16 conn_handle)
{
    if (mul_dev_get_handle_role(conn_handle) == MULTI_ROLE_SERVER) {
        server_deal_passkey_input(key);
    }

    if (mul_dev_get_handle_role(conn_handle) == MULTI_ROLE_CLIENT) {
        client_deal_passkey_input(key);
    }
}

void ble_sm_setup_init(io_capability_t io_type, u8 auth_req, uint8_t min_key_size, u8 security_en)
{
    //setup SM: Display only
    sm_init();
    sm_set_io_capabilities(io_type);

    if (io_type == IO_CAPABILITY_DISPLAY_ONLY) {
        sm_set_master_io_capabilities(IO_CAPABILITY_KEYBOARD_ONLY);
    }

    sm_set_authentication_requirements(auth_req);
    sm_set_encryption_key_size_range(min_key_size, 16);

    // note: 主机收到从机发送的安全请求时，主机应发送配对请求
    sm_set_request_security(security_en);
    sm_set_master_request_pair(security_en);

    sm_event_callback_set(&cbk_sm_packet_handler);

    if (io_type == IO_CAPABILITY_DISPLAY_ONLY) {
        /* reset_PK_cb_register(reset_passkey_cb); */
        reset_PK_cb_register_ext(ble_cbk_passkey_input);
    }
}


void ble_profile_init(void)
{
    // 主从一体
    ble_stack_gatt_role(2);

    ble_vendor_set_default_att_mtu(MULTI_ATT_MTU_SIZE);

    le_device_db_init();
#if PASSKEY_ENTER_ENABLE
    ble_sm_setup_init(IO_CAPABILITY_DISPLAY_ONLY, SM_AUTHREQ_MITM_PROTECTION | SM_AUTHREQ_BONDING, 7, TCFG_BLE_SECURITY_EN);
#else
    ble_sm_setup_init(IO_CAPABILITY_NO_INPUT_NO_OUTPUT, SM_AUTHREQ_MITM_PROTECTION | SM_AUTHREQ_BONDING, 7, TCFG_BLE_SECURITY_EN);
#endif

    server_profile_init();

    client_profile_init();

    // register for HCI events
    hci_event_callback_set(&cbk_packet_handler);
    /* ble_l2cap_register_packet_handler(packet_cbk); */
    /* sm_event_packet_handler_register(packet_cbk); */
    le_l2cap_register_packet_handler(&cbk_packet_handler);

    log_info("ble_profile_init over\n");

    ble_op_multi_att_send_init(multi_att_ram_buffer, MULTI_ATT_RAM_BUFSIZE, MULTI_ATT_LOCAL_PAYLOAD_SIZE);
}

void ble_combined_request_adv(u8 en)
{
    ble_combined_lock_init_once();
    os_mutex_pend(&g_ble_combined_mutex, 0);
    g_want_adv = en ? 1 : 0;
    os_mutex_post(&g_ble_combined_mutex);
}

void ble_combined_request_scan(u8 en)
{
    ble_combined_lock_init_once();
    os_mutex_pend(&g_ble_combined_mutex, 0);
    g_want_scan = en ? 1 : 0;
    os_mutex_post(&g_ble_combined_mutex);
}

void ble_combined_apply(void)
{
    ble_combined_lock_init_once();
    os_mutex_pend(&g_ble_combined_mutex, 0);

    // If server updated adv/rsp content (e.g. name), restart adv (only if desired).
    if (ble_simple_server_take_adv_restart_pending()) {
        if (g_want_adv) {
            if (ble_simple_server_adv_is_enabled()) {
                ble_simple_server_adv_default(0);
            }
            ble_simple_server_adv_default(1);
        }
    }

    // Apply ADV desired state.
    if (g_want_adv) {
        if (!ble_simple_server_adv_is_enabled()) {
            ble_simple_server_adv_default(1);
        }
    } else {
        if (ble_simple_server_adv_is_enabled()) {
            ble_simple_server_adv_default(0);
        }
    }

    // Apply SCAN desired state.
    if (g_want_scan) {
        if (!ble_simple_client_scan_is_enabled()) {
            ble_simple_client_scan_default(1);
        }
    } else {
        if (ble_simple_client_scan_is_enabled()) {
            ble_simple_client_scan_default(0);
        }
    }

    os_mutex_post(&g_ble_combined_mutex);
}

//统一接口，关闭模块
void ble_module_enable(u8 en)
{
    log_info("mode_en:%d\n", en);
    ble_simple_server_module_enable(en);
    ble_simple_client_module_enable(en);
}

void ble_app_disconnect(void)
{
    ble_simple_server_disconnect();
    ble_simple_client_disconnect();
}

void bt_ble_exit(void)
{
    ble_simple_server_exit();
    ble_simple_client_exit();
}

