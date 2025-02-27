#include "c_uart_p.h"
#include "cpu.h"
#include "device/device.h"

extern g_dev_uart_hdl;

// 注册回调函数
void handle_packet(uint8_t cmd, uint8_t *payload, uint16_t len)
{
    switch (cmd)
    {
    case UART_CMD_NORMAL_KEY: // 普通按键控制
        printf("the UART_CMD_NORMAL_KEY !\r\n");
        uint8_t resp_payload[4] = {0x01, 0x02, 0x03, 0x04};
        uart_send_response(cmd, 0x01, resp_payload, 4);
        // handle_led_command(payload);
        break;
    case UART_CMD_TOUCH_KEY: // 触摸按键控制
        // handle_touch_key_command(payload);
        printf("the UART_CMD_TOUCH_KEY !\r\n");
        break;
    case UART_CMD_SYNCH_STATUS: // 同步状态
        printf("the UART_CMD_SYNCH_STATUS !\r\n");
        int send_cnt = 0;
        send_cnt = uart_response_synch_status(0x01);
        printf("send_cnt = %d\r\n", send_cnt);
        // handle_sn_command(payload);
        break;
    case UART_CMD_POWER_OFF: // 关机控制
        printf("the UART_CMD_POWER_OFF !\r\n");
        // handle_touch_key_command(payload);
        break;
    case UART_CMD_SET_BLESN: // 设置蓝牙SN
        printf("the UART_CMD_SET_BLESN !\r\n");
        //handle_set_blesn_command(payload, len);
        break;
        // ...其他命令处理
    }
}

void handle_resp_packet(uint8_t cmd, uint8_t *code_with_payload, uint16_t len)
{
    switch (cmd)
    {
    case UART_CMD_SYNCH_STATUS: // 同步状态
        printf("the UART_CMD_SYNCH_STATUS !\r\n");
        // handle_sn_command(payload);
        break;
        // ...其他命令处理
    }
}

void handle_event(uart_event_type_t event)
{
    switch (event)
    {
    case UART_EVENT_PACKET_RECEIVED:
        printf("the UART_EVENT_PACKET_RECEIVED !\r\n");
        break;
    case UART_EVENT_TIMEOUT:
        printf("the UART_EVENT_TIMEOUT !\r\n");
        break;
    case UART_EVENT_CRC_ERROR:
        printf("the UART_EVENT_CRC_ERROR !\r\n");
        break;
    case UART_EVENT_INVALID_CMD:
        printf("the UART_EVENT_INVALID_CMD !\r\n");
        break;
    case UART_EVENT_OVERFLOW:
        printf("the UART_EVENT_OVERFLOW !\r\n");
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

    printf("uart_send_response: \r\n");
    printf_buf(resp_packet, 8 + len);
    // 发送响应包
    return dev_write(g_dev_uart_hdl, resp_packet, 8 + len);
}

int uart_response_synch_status(uint8_t busy_status)
{
    uint8_t busy_status_payload[1] = {busy_status};
    return uart_send_response(UART_CMD_SYNCH_STATUS, CPCODE_SUCCESS, busy_status_payload, 1);
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

    printf("uart_send_inquiry: \r\n");
    printf_buf(send_packet, 6 + len);
    // 发送响应包
    return dev_write(g_dev_uart_hdl, send_packet, 6 + len);
}
