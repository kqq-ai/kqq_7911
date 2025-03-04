#ifndef __BLE_SIMPLE_CLIENT_H__
#define __BLE_SIMPLE_CLIENT_H__

#include "../ble_combined.h"
#include "cpu.h"
#include "ble_user.h"
#include "btstack/le/ble_api.h"

void client_profile_init(void);
void client_deal_passkey_input(u32 *key);
void client_cbk_sm_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size);
void client_cbk_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size);

void ble_simple_client_module_enable(u8 en);
void ble_simple_client_init(void);
void ble_simple_client_exit(void);

int  ble_simple_client_create_connection(u8 *addr, u8 addr_type);
int  ble_simple_client_disconnect(void);
void ble_simple_client_disconnect_block(void);

int  ble_simple_client_write_send(void *priv, uint16_t handle, u8 *data, u16 len);
int  ble_simple_client_write_without_respond_send(void *priv, uint16_t handle, u8 *data, u16 len);
int  ble_simple_client_read_value_send(void *priv, uint16_t handle);
int  ble_simple_client_read_long_value_send(void *priv, uint16_t handle);


void ble_conn_event_user_handler_add(void (*handler)(uint8_t *, void *), void *priv);
void ble_disconn_event_user_handler_add(void (*handler)(uint8_t *, void *), void *priv);
void ble_adv_report_event_user_handler_add(void (*handler)(adv_report_t *, u16, void *), void *priv);

void set_scan_adv_callback(void (*cb)(void *priv, char *dev_name, u16 dev_name_len, s8 rssi), void *priv);

#endif