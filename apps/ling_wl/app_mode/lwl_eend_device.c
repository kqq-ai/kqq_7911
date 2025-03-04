#include "lwl_eend_device.h"

bool adv_pre_match_impl(const u8 *adv_data, const u16 adv_len)
{
    uint8_t buffer[32];
    int r = ble_adv_report_get_data(adv_data, adv_len, HCI_EIR_DATATYPE_MANUFACTURER_SPECIFIC_DATA, buffer);
    return (r == 17) && (buffer[0] == 0xE3) && (buffer[1] == 0xFA);
}

bool adv_match_impl(const void *__this, const u8 *adv_data, const u16 adv_len)
{
    uint8_t buffer[32];
    int r = ble_adv_report_get_data(adv_data, adv_len, HCI_EIR_DATATYPE_MANUFACTURER_SPECIFIC_DATA, buffer);
    return memcmp(((lwl_eend_dev_t *)__this)->sn, buffer + 2, SN_SIZE - 1) == 0;
}

void get_string_impl(const void *__this, char *buffer, const int size)
{
    lwl_eend_dev_t * that = (lwl_eend_dev_t *)__this;
    u8 *sn = that->sn;
#if 0
    snprintf(buffer, size, "%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X", sn[0], sn[1], sn[2], sn[3], sn[4], sn[5], sn[6], sn[7], sn[8], sn[9], sn[10], sn[11], sn[12], sn[13], sn[14]);
#else
    snprintf(buffer, size, "[XXX_%02X]", sn[14]);
#endif
}

void lwl_eend_dev_init(const u8 *sn, lwl_eend_dev_t *dev)
{
    memcpy(dev->sn, sn, SN_SIZE);
    dev->adv_match = adv_match_impl;
    dev->get_string = get_string_impl;
}
