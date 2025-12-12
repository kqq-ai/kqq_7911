#include "lwl_eend_device.h"
#include "lwl_app_core.h"

bool adv_pre_match_impl(const u8 *adv_data, const u16 adv_len)
{
    uint8_t buffer[32];
    int r = ble_adv_report_get_data(adv_data, adv_len, HCI_EIR_DATATYPE_MANUFACTURER_SPECIFIC_DATA, buffer);
    return (r > 18) && (buffer[0] == 0xE3) && (buffer[1] == 0xFA);
}

bool adv_match_impl(const void *__this, const u8 *adv_data, const u16 adv_len)
{
    uint8_t buffer[32];
    int r = ble_adv_report_get_data(adv_data, adv_len, HCI_EIR_DATATYPE_MANUFACTURER_SPECIFIC_DATA, buffer);
    return memcmp(((lwl_eend_dev_t *)__this)->sn, buffer + 3, SN_SIZE) == 0;
}

void get_string_impl(const void *__this, char *buffer, const int size)
{
    lwl_eend_dev_t * that = (lwl_eend_dev_t *)__this;
    u8 *sn = that->sn;
#if 0
    snprintf(buffer, size, "%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X", sn[0], sn[1], sn[2], sn[3], sn[4], sn[5], sn[6], sn[7], sn[8], sn[9], sn[10], sn[11], sn[12], sn[13], sn[14]);
#else
    snprintf(buffer, size, "%s", sn);
#endif
}

void set_ctrl_impl(void *__this, uint8_t on, uint8_t play)
{
    lwl_eend_dev_t * that = (lwl_eend_dev_t *)__this;
    uint8_t ctrl = 0;
    if (on) {
        ctrl |= BDPCTRL_TURNON_FLAG;
    }
    if (play) {
        ctrl |= BDPCTRL_PLAY_FLAG;
    }
    that->ctrl_mask_flags = ctrl;
    uart_send_set_ctrl(that->ctrl_mask_flags);
    log_i("[EEND]set_ctrl_impl %s: on=%d play=%d ctrl=0x%02X", that->sn, on, play, that->ctrl_mask_flags);
}

void lwl_eend_dev_init(const u8 *sn, lwl_eend_dev_t *dev)
{
    memcpy(dev->sn, sn, SN_SIZE);
    dev->adv_match = adv_match_impl;
    dev->get_string = get_string_impl;
    dev->set_ctrl = set_ctrl_impl;

    dev->bat_level = 0;
    dev->adv_state = BDPAS_OFF;
    dev->op_state = BDO_STATE_IDLE;
    dev->has_deadline = false;
    dev->deadline = 0;
    dev->ctrl_mask_flags = BDPCTRL_TURNON_FLAG | BDPCTRL_PLAY_FLAG;
    dev->active_time = 0;
    dev->max_rssi = -127;

    os_mutex_create(&dev->distance_info.mutex);
    dev->distance_info.distance = 5;
    dev->distance_info.rssi_sample_count = 0;
    dev->distance_info.test_count = 0;
    dev->distance_info.distance_type = DISTANCE_3;
    memset(dev->distance_info.rssi_samples, -128, sizeof(dev->distance_info.rssi_samples));
}
