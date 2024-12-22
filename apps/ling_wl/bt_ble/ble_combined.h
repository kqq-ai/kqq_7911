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


#endif