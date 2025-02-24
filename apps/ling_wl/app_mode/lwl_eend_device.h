#ifndef __LWL_EEND_DEVICE_H__
#define __LWL_EEND_DEVICE_H__

#include "cpu.h"

#include "btstack/le/le_common_define.h"
#include "bt_ble/client/ble_att_util.h"

#define SN_SIZE 16

typedef struct {
    u8 sn[SN_SIZE];
    bool (*adv_match)(void *__this, u8 *adv_data, u16 adv_len);
    void (*get_string)(void *__this, char *buffer, int size);
} lwl_eend_dev_t;

bool adv_pre_match_impl(const u8 *adv_data, const u16 adv_len);

bool adv_match_impl(const void *__this, const u8 *adv_data, const u16 adv_len);

void get_string_impl(const void *__this, char *buffer, const int size);

void lwl_eend_dev_init(const u8 *sn, lwl_eend_dev_t *dev);

#endif
