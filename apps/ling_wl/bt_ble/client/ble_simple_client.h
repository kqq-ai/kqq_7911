#ifndef __BLE_SIMPLE_CLIENT_H__
#define __BLE_SIMPLE_CLIENT_H__

#include "cpu.h"

void client_profile_init(void);
void client_deal_passkey_input(u32 *key);
void client_cbk_sm_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size);
void client_cbk_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size);

void ble_simple_client_disconnect(void);
void ble_simple_client_module_enable(u8 en);
void ble_simple_client_init(void);
void ble_simple_client_exit(void);
void ble_simple_client_send_conn_param_update(void);

void set_scan_adv_callback(void (*cb)(void *priv, char *dev_name, u16 dev_name_len, s8 rssi), void *priv);

#endif