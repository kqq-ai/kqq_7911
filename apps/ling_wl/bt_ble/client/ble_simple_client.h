#ifndef __BLE_SIMPLE_CLIENT_H__
#define __BLE_SIMPLE_CLIENT_H__

#include "../ble_combined.h"
#include "cpu.h"
#include "ble_user.h"
#include "btstack/le/ble_api.h"
#include "btstack/le/att.h"

#include "le_common.h"

void client_profile_init(void);
void client_deal_passkey_input(u32 *key);
void client_cbk_sm_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size);
void client_cbk_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size);

// Raw packet handler hook registration
void ble_simple_client_set_raw_packet_handler(void (*handler)(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size));

void ble_simple_client_scan_enable(u8 en, int interval, int window);
// Default scan enable/disable (uses module default interval/window)
void ble_simple_client_scan_default(u8 en);
// Whether scan has been enabled at least once (for idempotent stop)
u8 ble_simple_client_scan_is_enabled(void);
void ble_simple_client_module_enable(u8 en);
void ble_simple_client_init(void);
void ble_simple_client_exit(void);

int  ble_simple_client_create_connection(u8 *addr, u8 addr_type);
int  ble_simple_client_create_cannel(void);
int  ble_simple_client_disconnect(void);
void ble_simple_client_disconnect_block(void);
void ble_simple_client_profile_start(void);

int  ble_simple_client_write_send(void *priv, uint16_t handle, u8 *data, u16 len);
int  ble_simple_client_write_without_respond_send(void *priv, uint16_t handle, u8 *data, u16 len);
int  ble_simple_client_read_value_send(void *priv, uint16_t handle);
int  ble_simple_client_read_long_value_send(void *priv, uint16_t handle);


void ble_conn_event_user_handler_add(void (*handler)(uint8_t *, void *), void *priv);
void ble_disconn_event_user_handler_add(void (*handler)(uint8_t *, void *), void *priv);
void ble_adv_report_event_user_handler_add(void (*handler)(adv_report_t *, u16, void *), void *priv);
void ble_client_report_data_event_user_handler_add(void (*handler)(att_data_report_t *, void *), void *priv);
void ble_search_completed_event_user_handler_add(void (*handler)(void *), void *priv);

void set_scan_adv_callback(void (*cb)(void *priv, char *dev_name, u16 dev_name_len, s8 rssi), void *priv);

#endif