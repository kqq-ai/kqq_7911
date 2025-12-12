#include "c_uart_p.h"
#include "cpu.h"
#include "device/device.h"
#include "app_mode/lwl_app_core.h"
#include "gpio.h"                 // for gpio_direction_output

extern g_dev_uart_hdl;

// 注册回调函数
void handle_packet(uint8_t cmd, uint8_t *payload, uint16_t len)
{
    switch (cmd)
    {
    // case UART_CMD_NORMAL_KEY:{ // 普通按键控制
    //     log_i("the UART_CMD_NORMAL_KEY !\r\n");
    //     lwl_event_t e;
    //     e.event_type = LWL_EVENT_TYPE_UART_IO_KEY_SHORT_CLICK;
    //     lwl_event_notify(&e);
    // }   break;
    // case UART_CMD_TOUCH_KEY:{ // 触摸按键控制
    //     log_i("the UART_CMD_TOUCH_KEY !\r\n");
    //     lwl_event_t e;
    //     e.event_type = LWL_EVENT_TYPE_UART_TOUCH_KEY_DOWN;
    //     e.uart_cmd_touch.touch_key_event_type = payload[1];
    //     lwl_event_notify(&e);
    // }   break;
    case UART_CMD_SYNCH_STATUS:{ // 同步状态
        log_d("the UART_CMD_SYNCH_STATUS !\r\n");
        lwl_event_t e;
        e.event_type = LWL_EVENT_TYPE_UART_SYNC;
        e.uart_cmd_sync.battery = payload[1];
        e.uart_cmd_sync.charging = payload[2];
        lwl_event_notify(&e);
    }   break;
    // case UART_CMD_POWER_OFF:{ // 关机控制
    //     log_i("**********************************************************************!\r\n");
    //     log_i("the UART_CMD_POWER_OFF !\r\n");
    //     lwl_event_t e;
    //     e.event_type = LWL_EVENT_TYPE_UART_SHUTDOWN;
    //     lwl_event_notify(&e);
    // }   break;
    case UART_CMD_SET_BLESN:{ // 设置蓝牙SN
        log_i("the UART_CMD_SET_BLESN !\r\n");
        // ignore
    }   break;
    case UART_CMD_MOVE_DETECT_EN:{ // 使能移动检测
        log_i("the UART_CMD_MOVE_DETECT_EN !\r\n");
        // ignore
    }   break;
    // case UART_CMD_MOVE_OVER_A_METER:{ // 通知检测距离大于一米
    //     log_i("the UART_CMD_MOVE_OVER_A_METER !\r\n");
    //     lwl_event_t e;
    //     e.event_type = LWL_EVENT_TYPE_UART_MOVE_MORE_THAN_ONE_METER;
    //     lwl_event_notify(&e);
    // }   break;
    case UART_CMD_CLEAR_WIFI_SSID:{ // 清除WIFI SSID
        log_i("the UART_CMD_CLEAR_WIFI_SSID !\r\n");
        lwl_event_t e;
        e.event_type = LWL_EVENT_TYPE_UART_CLEAR_WIFI_SSID;
        lwl_event_notify(&e);
    }   break;
    // case UART_CMD_NOTIFY_GSENSOR_OFF:{ // Gsensor 关闭
    //     log_i("the UART_CMD_NOTIFY_GSENSOR_OFF !\r\n");
    //     lwl_event_t e;
    //     e.event_type = LWL_EVENT_TYPE_UART_NOTIFY_GSENSOR_OFF;
    //     lwl_event_notify(&e);
    // }   break;
    }
}

void handle_resp_packet(uint8_t cmd, uint8_t code, uint8_t *payload, uint16_t len)
{
    switch (cmd)
    {
    case UART_CMD_SYNCH_STATUS: // 同步状态
        log_d("the UART_CMD_SYNCH_STATUS !\r\n");
        // ignore
        break;
    }
}

void handle_event(uart_event_type_t event)
{
    switch (event)
    {
    case UART_EVENT_PACKET_RECEIVED:
        log_d("the UART_EVENT_PACKET_RECEIVED !\r\n");
        break;
    case UART_EVENT_TIMEOUT:
        log_d("the UART_EVENT_TIMEOUT !\r\n");
        break;
    case UART_EVENT_CRC_ERROR:
        log_d("the UART_EVENT_CRC_ERROR !\r\n");
        break;
    case UART_EVENT_INVALID_CMD:
        log_d("the UART_EVENT_INVALID_CMD !\r\n");
        break;
    case UART_EVENT_OVERFLOW:
        log_d("the UART_EVENT_OVERFLOW !\r\n");
        break;
    }
}

// 发送响应包
int uart_send_response(uint8_t cmd, uint8_t code, uint8_t *payload, uint16_t len)
{
    // 构造 HEADER + CMD + CODE + LEN + PAYLOAD
    uint8_t header_cmd_code_len_payload[5 + len];
    header_cmd_code_len_payload[0] = PACKET_HEADER_RESP; // HEADER
    header_cmd_code_len_payload[1] = cmd;                // CMD
    header_cmd_code_len_payload[2] = code;               // CODE
    header_cmd_code_len_payload[3] = len & 0xFF;         // LEN (低字节)
    header_cmd_code_len_payload[4] = (len >> 8) & 0xFF;  // LEN (高字节)
    if (len > 0)
    {
        memcpy(header_cmd_code_len_payload + 5, payload, len); // PAYLOAD
    }

    // 计算 CRC 校验值（HEADER + CMD + CODE + LEN + PAYLOAD）
    uint16_t crc = crc16_modbus(header_cmd_code_len_payload, 5 + len);

    // 构造完整的响应包
    uint8_t resp_packet[8 + len];                              // HEADER(1) + CMD(1) + CODE(1) + LEN(2) + PAYLOAD(LEN) + CRC(2) + TRAILER(1)
    memcpy(resp_packet, header_cmd_code_len_payload, 5 + len); // 复制 HEADER + CMD + CODE + LEN + PAYLOAD
    resp_packet[5 + len] = crc & 0xFF;                         // CRC (低字节)
    resp_packet[6 + len] = (crc >> 8) & 0xFF;                  // CRC (高字节)
    resp_packet[7 + len] = PACKET_TRAILER_RESP;                // TRAILER

    log_d("uart_send_response: \r\n");
    log_d_buf(resp_packet, 8 + len);
    // 发送响应包
    return dev_write(g_dev_uart_hdl, resp_packet, 8 + len);
}

/// @brief  调节音量
/// @param busy_status 通常为 1
/// @param volume 音量，范围 0-16
/// @return 
int uart_response_synch_status(uint8_t busy_status, uint8_t volume)
{
    uint8_t busy_status_payload[2] = {busy_status, volume};
    return uart_send_response(UART_CMD_SYNCH_STATUS, CPCODE_SUCCESS, busy_status_payload, 2);
}

int uart_response_clear_wifi_ssid(void)
{
    uint8_t normal_data = 0x00;
    return uart_send_response(UART_CMD_CLEAR_WIFI_SSID, CPCODE_SUCCESS, &normal_data, 1);
}

/* 1. 关闭加速度传感器的响应函数 */
int uart_response_notify_gsensor_off(void)
{
    uint8_t normal_data = 0x00;
    return uart_send_response(UART_CMD_NOTIFY_GSENSOR_OFF, CPCODE_SUCCESS, &normal_data, 1);
}

// 发送事件包
int uart_send_inquiry(uint8_t *payload, uint16_t len)
{
    int ret = 0;
    // 构造 HEADER + LEN + DATA(CMD + PAYLOAD) + CRC + PAYLOAD
    uint8_t send_packet[6 + len];
    send_packet[0] = PACKET_HEADER_CMD;
    send_packet[1] = len & 0xFF; //低字节
    send_packet[2] = (len >> 8) & 0xFF; //高字节
    memcpy(&send_packet[3], payload, len);
    
    // 计算 CRC
    uint16_t crc = calculate_crc(send_packet, 3 + len);
    send_packet[3 + len] = crc & 0xFF;
    send_packet[4 + len] = (crc >> 8) & 0xFF;

    send_packet[5 + len] = PACKET_TRAILER_CMD;

    log_d("uart_send_inquiry: \r\n");
    log_d_buf(send_packet, 6 + len);

    if (g_dev_uart_hdl == NULL) 
        return -1;
    // 发送响应包
    return dev_write(g_dev_uart_hdl, send_packet, 6 + len);
}

int uart_send_set_sn(char *sn)
{
    uint8_t busy_status_payload[1 + SN_SIZE] = {0};
    busy_status_payload[0] = UART_CMD_SET_BLESN;
    if (strcmp(sn, "WECNC4H06CPLJE7") == 0) {
        memcpy(busy_status_payload + 1, "WCCN24H28FEEEFA", SN_SIZE);
    } else {
        memcpy(busy_status_payload + 1, sn, SN_SIZE);
    }
    return uart_send_inquiry(busy_status_payload, sizeof(busy_status_payload));
}

int uart_send_move_detect_en(uint8_t en)
{
    uint8_t send_payload[2];
    memset(send_payload, 0, sizeof(send_payload));
    send_payload[0] = UART_CMD_MOVE_DETECT_EN;
    send_payload[1] = en;
    return uart_send_inquiry(send_payload, 2);
}

/* 4. 设置LED颜色 的发送函数 */
// int set_rgb_color(uint16_t r, uint16_t g, uint16_t b)
// {
//     set_rgb_color(10000 - r, 10000 - g, 10000 - b); // 本地也设置LED颜色
//     // uint8_t send_led_payload[7];
//     // memset(send_led_payload, 0, sizeof(send_led_payload));
//     // send_led_payload[0] = UART_CMD_SET_LED_COLOR;   
//     // /* 小端序 */
//     // send_led_payload[1] = r & 0xFF;        // r 低字节
//     // send_led_payload[2] = (r >> 8) & 0xFF; // r 高字节
//     // send_led_payload[3] = g & 0xFF;        // g 低字节
//     // send_led_payload[4] = (g >> 8) & 0xFF; // g 高字节
//     // send_led_payload[5] = b & 0xFF;        // b 低字节
//     // send_led_payload[6] = (b >> 8) & 0xFF; // b 高字节

//     // return uart_send_inquiry(send_led_payload, sizeof(send_led_payload));
// }

/* 5. 7911申请软关机 发送函数 */
int uart_send_apply_soft_poweroff(void)
{
    uint8_t send_payload[1];
    memset(send_payload, 0, sizeof(send_payload));
    send_payload[0] = UART_CMD_APPLY_SOFT_POWEROFF;
    return uart_send_inquiry(send_payload, 1);
}

int uart_send_open_le_audio(uint8_t *mac)
{
    // uint8_t payload[1 + 6] = {0};
    // payload[0] = UART_CMD_OPEN_LE_AUDIO;
    // memcpy(payload + 1, mac, 6);
        /* test */
    uint8_t payload[1 + 16] = {0};
    payload[0] = UART_CMD_OPEN_LE_AUDIO;
    memcpy(payload + 1, mac, 16);
    return uart_send_inquiry(payload, sizeof(payload));
}

/* 7. 指定广播配对码重新打开Audio广播 */
int uart_send_switch_audio_bc(uint8_t *broadcast_code, uint8_t ctrl)
{
    uint8_t payload[1 + 7] = {0};
    payload[0] = UART_CMD_SWITCH_AUDIO_BC;
    memcpy(payload + 1, broadcast_code, 6);
    payload[7] = ctrl;
    return uart_send_inquiry(payload, sizeof(payload));
}

/* 8. 指定广播配对码重新打开Audio广播 */
int uart_send_set_ctrl(uint8_t ctrl)
{
    uint8_t busy_status_payload[1 + 1] = {0};
    busy_status_payload[0] = UART_CMD_SET_CTRL;
    busy_status_payload[1] = ctrl;
    return uart_send_inquiry(busy_status_payload, sizeof(busy_status_payload));
}

/* 6. 通知7016进入 播放模式 的发送函数 */
int uart_send_audio_dev_play(void)
{
    uint8_t payload[2] = {0};
    payload[0] = UART_CMD_AUDIO_DEV_PLAY;
    payload[1] = 0x00;
    return uart_send_inquiry(payload, sizeof(payload));
}

/* 7. 通知7016进入 OTA模式 的发送函数 */
int uart_send_audio_dev_ota(void)
{
    uint8_t payload[2] = {0};
    payload[0] = UART_CMD_AUDIO_DEV_OTA;
    payload[1] = 0x00;
    return uart_send_inquiry(payload, sizeof(payload));
}

/* 8. 通知7016进入 关闭模式 的发送函数 */
int uart_send_audio_dev_close(void)
{
    uint8_t payload[2] = {0};
    payload[0] = UART_CMD_AUDIO_DEV_CLOSE;
    payload[1] = 0x00;
    return uart_send_inquiry(payload, sizeof(payload));
}
