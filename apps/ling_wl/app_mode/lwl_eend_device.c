#include "lwl_eend_device.h"

bool adv_pre_match_impl(const u8 *adv_data, const u16 adv_len)
{
    uint8_t buffer[32];
    int r = ble_adv_report_get_data(adv_data, adv_len, HCI_EIR_DATATYPE_MANUFACTURER_SPECIFIC_DATA, buffer);
    return (r == 18) && (buffer[0] == 0xE3) && (buffer[1] == 0xFA);
}

bool adv_match_impl(const void *__this, const u8 *adv_data, const u16 adv_len)
{
    uint8_t buffer[32];
    int r = ble_adv_report_get_data(adv_data, adv_len, HCI_EIR_DATATYPE_MANUFACTURER_SPECIFIC_DATA, buffer);
    return memcmp(((lwl_eend_dev_t *)__this)->sn, buffer + 2, SN_SIZE) == 0;
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

    os_mutex_create(&dev->distance_info.mutex);
    dev->distance_info.rssi = -128;
    dev->distance_info.rssi_sample_count = 0;
    memset(dev->distance_info.rssi_samples, -128, sizeof(dev->distance_info.rssi_samples));

    os_mutex_create(&dev->handshake_info.mutex);
    dev->handshake_info.alive = 0;
    dev->handshake_info.alive_hs_fail_cnt = 0;
    dev->handshake_info.handshake_flag = 0;
    memset(&dev->handshake_info.cache_buf, 0, (sizeof(bka_cmd_t) * KEEP_ALIVE_CMD_NUM));
    
    my_blocking_queue_init_static(&dev->handshake_info.cmd_buffer, dev->handshake_info.cache_buf, sizeof(bka_cmd_t), KEEP_ALIVE_CMD_NUM);
}
