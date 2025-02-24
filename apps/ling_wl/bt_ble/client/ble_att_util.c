#include "ble_att_util.h"
#include "cpu.h"

int ble_adv_report_get_data(u8 *data, u8 data_length, u8 ad_type, u8 *buffer)
{
    u8 *adv_data_pt = data;

    for (u8 i = 0; i < data_length;) {
        if (adv_data_pt == 0) {
            break;
        }

        u8 length = *adv_data_pt++;
        if (length >= data_length || (length + i) >= data_length) {
            /*过滤非标准包格式*/
            printf("!!!error_adv_packet:");
            put_buf(data, data_length);
            break;
        }

        u8 type = *adv_data_pt++;
        i += (length + 1);
        
        if (type == ad_type) {
            memcpy(buffer, adv_data_pt, length - 1);
            return length - 1;
        } else {
            adv_data_pt += (length - 1);
        }
    }
    return -1;
}
