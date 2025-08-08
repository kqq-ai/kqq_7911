#include "c_uart_p.h"
#include "cpu.h"
#include "device/device.h"
#include "app_config.h"          // for CONFIG_VOICE_PROMPT_FILE_PATH
#include "app_mode/lwl_app_core.h"
#include "gpio.h"                 // for gpio_direction_output

#ifndef CONFIG_VOICE_PROMPT_FILE_PATH
// Fallback to a reasonable default if config path is not defined by profile
#define CONFIG_VOICE_PROMPT_FILE_PATH       "mnt/sdfile/res/audlogo/"
#endif

extern g_dev_uart_hdl;

static void set_led_power_off(void)      { uart_send_set_led_color(COLOR_BLACK); } /// off LED before power Off


// 关机提示音播放完成后的回调：播放结束再触发关机事件
static void power_off_prompt_end_cb(void *priv)
{
    log_i("-----------------------------------------------\r\n");
    log_i("PowerOff prompt finished, notify shutdown\r\n");
    // 播放完成后再灭灯，防止对侧熄灯动作顺带关闭功放导致听不到提示音
    set_led_power_off();
    lwl_event_t e;
    e.event_type = LWL_EVENT_TYPE_UART_SHUTDOWN;
    lwl_event_notify(&e);
}

// 注册回调函数
void handle_packet(uint8_t cmd, uint8_t *payload, uint16_t len)
{
    switch (cmd)
    {
    case UART_CMD_NORMAL_KEY:{ // 普通按键控制
        log_i("the UART_CMD_NORMAL_KEY !\r\n");
        lwl_event_t e;
        e.event_type = LWL_EVENT_TYPE_UART_IO_KEY_SHORT_CLICK;
        lwl_event_notify(&e);
    }   break;
    case UART_CMD_TOUCH_KEY:{ // 触摸按键控制
        log_i("the UART_CMD_TOUCH_KEY !\r\n");
        lwl_event_t e;
        
        // 根据payload[1]判断是按下还是抬起
        // 0: touch_key_down, 1: touch_key_up
        if (payload[1] == 0) {
            e.event_type = LWL_EVENT_TYPE_UART_TOUCH_KEY_DOWN;
            log_i("Touch key PRESSED\r\n");
        } else if (payload[1] == 1) {
            e.event_type = LWL_EVENT_TYPE_UART_TOUCH_KEY_UP;
            log_i("Touch key RELEASED\r\n");
        } else {
            log_w("Unknown touch key event type: %d\r\n", payload[1]);
            e.event_type = LWL_EVENT_TYPE_UART_TOUCH_KEY_DOWN; // 默认作为按下处理
        }
        
        e.uart_cmd_touch.touch_key_event_type = payload[1];
        lwl_event_notify(&e);
    }   break;
    case UART_CMD_SYNCH_STATUS:{ // 同步状态
        log_d("the UART_CMD_SYNCH_STATUS !\r\n");
        lwl_event_t e;
        e.event_type = LWL_EVENT_TYPE_UART_SYNC;
        e.uart_cmd_sync.battery = payload[1];
        e.uart_cmd_sync.charging = payload[2];
        lwl_event_notify(&e);
    }   break;
    case UART_CMD_POWER_OFF:{ // 关机控制
        log_i("**********************************************************************!\r\n");
        log_i("the UART_CMD_POWER_OFF !\r\n");
        extern int OTA_download_running;
        if( OTA_download_running > 0){
            break; /// reset after OTA
        }
        // 播放关机提示音，播放完成后再关机
        int err = -1;
        lwl_app_context_t *context = lwl_app_core_context_get_instance();
        if (context && context->music_player) {
            log_i("Try play PowerOff prompt before shutdown\r\n");
           gpio_direction_output(IO_PORTA_00, 1);  // 设置C出声
            err = app_music_play_local_music_with_callback(
                    context->music_player,
                    CONFIG_VOICE_PROMPT_FILE_PATH"PowerOff.mp3",
                    power_off_prompt_end_cb,
                    NULL);
        }

        if (err < 0) {
            // 没有播放器或播放失败，直接进入关机流程
            log_w("PowerOff prompt play failed or unavailable, shutdown immediately\r\n");
            lwl_event_t e;
            e.event_type = LWL_EVENT_TYPE_UART_SHUTDOWN;
            lwl_event_notify(&e);
        }
    }   break;
    case UART_CMD_SET_BLESN:{ // 设置蓝牙SN
        log_i("the UART_CMD_SET_BLESN !\r\n");
        // ignore
    }   break;
    case UART_CMD_MOVE_DETECT_EN:{ // 使能移动检测
        log_i("the UART_CMD_MOVE_DETECT_EN !\r\n");
        // ignore
    }   break;
    case UART_CMD_MOVE_OVER_A_METER:{ // 通知检测距离大于一米
        log_i("the UART_CMD_MOVE_OVER_A_METER !\r\n");
        lwl_event_t e;
        e.event_type = LWL_EVENT_TYPE_UART_MOVE_MORE_THAN_ONE_METER;
        lwl_event_notify(&e);
    }   break;
    case UART_CMD_CLEAR_WIFI_SSID:{ // 清除WIFI SSID
        log_i("the UART_CMD_CLEAR_WIFI_SSID !\r\n");
        lwl_event_t e;
        e.event_type = LWL_EVENT_TYPE_UART_CLEAR_WIFI_SSID;
        lwl_event_notify(&e);
    }   break;
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
int uart_send_set_led_color(uint16_t r, uint16_t g, uint16_t b)
{
    uint8_t send_led_payload[7];
    memset(send_led_payload, 0, sizeof(send_led_payload));
    send_led_payload[0] = UART_CMD_SET_LED_COLOR;   
    /* 小端序 */
    send_led_payload[1] = r & 0xFF;        // r 低字节
    send_led_payload[2] = (r >> 8) & 0xFF; // r 高字节
    send_led_payload[3] = g & 0xFF;        // g 低字节
    send_led_payload[4] = (g >> 8) & 0xFF; // g 高字节
    send_led_payload[5] = b & 0xFF;        // b 低字节
    send_led_payload[6] = (b >> 8) & 0xFF; // b 高字节

    return uart_send_inquiry(send_led_payload, sizeof(send_led_payload));
}

void set_led_off(void)          { uart_send_set_led_color(COLOR_BLACK); }   //结束对话时灯光关闭

int uart_send_open_le_audio(uint8_t *mac)
{
    uint8_t payload[1 + 6] = {0};
    payload[0] = UART_CMD_OPEN_LE_AUDIO;
    memcpy(payload + 1, mac, 6);
    return uart_send_inquiry(payload, sizeof(payload));
}
