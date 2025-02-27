#ifndef _APP_CRC16_H_
#define _APP_CRC16_H_

#include "stdint.h"
#include "cpu.h"

/* !!!!!!!!!!!!!!!!!!!! 提供给 app_uart_task.c 使用 !!!!!!!!!!!!!!!!!!!! */

uint16_t crc16_modbus(const uint8_t *data, uint16_t len);

uint16_t crc16_modbus_respond(const uint8_t *data, uint16_t len);

uint16_t calculate_crc(const uint8_t *data, uint16_t len);

bool verify_crc(const uint8_t *data, uint16_t len);


#endif