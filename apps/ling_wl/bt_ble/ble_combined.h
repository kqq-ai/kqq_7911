#ifndef __BLE_COMBINED_H__
#define __BLE_COMBINED_H__

#include "cpu.h"

#define INVALID_HANDLE 0xFFFF

#define MULTI_ROLE_MASTER            1
#define MULTI_ROLE_SLAVE             0

#define MULTI_ROLE_CLIENT            MULTI_ROLE_MASTER
#define MULTI_ROLE_SERVER            MULTI_ROLE_SLAVE

extern u16 server_con_handle;
extern u16 client_con_handle;


//统一接口，关闭模块
void ble_module_enable(u8 en);
void ble_app_disconnect(void);
void bt_ble_exit(void);

// Scan/Adv 统一管理接口
void ble_combined_request_scan(u8 en);  // 请求 scan 状态 (0=关闭, 1=开启)
void ble_combined_apply(void);          // 应用请求的状态到实际硬件

#endif