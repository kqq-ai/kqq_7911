#ifndef __BLE_KEEP_ALIVE_H__
#define __BLE_KEEP_ALIVE_H__

#include "cpu.h"
#include "app_mode/lwl_eend_device.h"

#define KEEP_ALIVE_HANDSHAKE_LOST_CNT 10

void ble_keep_alive_init(void);

#endif