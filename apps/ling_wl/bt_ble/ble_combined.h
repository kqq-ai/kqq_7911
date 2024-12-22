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

#endif