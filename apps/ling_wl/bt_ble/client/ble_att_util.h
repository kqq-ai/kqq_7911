#ifndef __BLE_ATT_UTIL_H__
#define __BLE_ATT_UTIL_H__

#include "cpu.h"

int ble_adv_report_get_data(u8 *data, u8 data_length, u8 ad_type, u8 *buffer);

#endif