#ifndef __BLE_SIMPLE_SERVER_H__
#define __BLE_SIMPLE_SERVER_H__

#include <stdint.h>
#include "cpu.h"

void server_profile_init(void);
void server_deal_passkey_input(u32 *key);
void server_cbk_sm_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size);
void server_cbk_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size);

void ble_simple_server_init(void);
void ble_simple_server_exit(void);
void ble_simple_server_app_disconnect(void);

#endif
