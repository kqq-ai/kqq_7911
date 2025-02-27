#include "app_uart_task.h"
#include "app_config.h"
#include "system/includes.h"
#include "device/device.h"
#include "gpio.h"
#include "uart.h"

#include "app_crc16.h"
#include "c_uart_p.h"

// #include "crc16.h"  // 需实现CRC-16/MODBUS

static u8 dev_recv_buf[UART_RECV_BUF_SIZE] __attribute__((aligned(32)));
static u8 g_recv_buf[UART_RECV_BUF_SIZE] __attribute__((aligned(32)));
static u8 packet_buf[MAX_PAYLOAD_LEN + 16]; // 完整数据包缓存
static volatile u16 packet_idx = 0;
void *g_dev_uart_hdl = NULL;

// 回调函数指针
static uart_packet_handler_t packet_handler = NULL;
static uart_resp_packet_handler_t resp_packet_handler = NULL;
static uart_event_handler_t event_handler = NULL;

// 注册回调函数
void uart_register_packet_handler(uart_packet_handler_t handler)
{
    packet_handler = handler;
}

void uart_register_resp_packet_handler(uart_resp_packet_handler_t handler)
{
    resp_packet_handler = handler;
}

void uart_register_event_handler(uart_event_handler_t handler)
{
    event_handler = handler;
}

// 数据包解析
static void parse_uart_packet(uint8_t *data, uint16_t len)
{
    printf("===============start parse the data ! ===================\r\n");
    // 校验包头
    if (data[0] == PACKET_HEADER_CMD)
    {
        // 提取LEN字段（小端序）
        uint16_t payload_len = (data[2] << 8) | data[1];
        uint16_t packet_len = 1 + 2 + payload_len + 2 + 1; // HEADER(1) + LEN(2) + PAYLOAD(LEN) + CRC(2) + TRAILER(1)

        printf("================payload_len len:%d===============\r\n", payload_len);
        printf("================packet_len len:%d===============\r\n", packet_len);
        // 检查数据包是否完整
        if (len < packet_len)
        {
            printf("uart_packet_handler: packet is incomplete\r\n");
            if (event_handler)
                event_handler(UART_EVENT_TIMEOUT);
            return;
        }

        // 校验包尾
        if (data[packet_len - 1] != PACKET_TRAILER_CMD)
        {
            printf("uart_packet_handler: packet trailer error\r\n");
            if (event_handler)
                event_handler(UART_EVENT_INVALID_CMD);
            return;
        }

        // CRC校验
        if (!verify_crc(data, packet_len))
        {
            if (event_handler)
                event_handler(UART_EVENT_CRC_ERROR);
            return;
        }

        // 提取CMD和Payload
        uint8_t cmd = data[3];
        printf("cmd : %02x\r\n", cmd);
        // uint8_t temppayload_len = data[2];
        printf("the payload data : \r\n");
        printf_buf(data + 3, payload_len);

        if (packet_handler)
        {
            packet_handler(cmd, data + 3, payload_len);
        }
    }
    else if (data[0] == PACKET_HEADER_RESP)
    {
        // 提取LEN字段（小端序）
        // 提取LEN字段
        uint16_t resp_payload_len = (packet_buf[4] << 8) | packet_buf[3];
        uint16_t resp_packet_len = 1 + 2 + resp_payload_len + 2 + 1; // HEADER(1) + LEN(2) + PAYLOAD(LEN) + CRC(2) + TRAILER(1)

        printf("================ resp payload_len len:%d===============\r\n", resp_payload_len);
        printf("================ resp packet_len len:%d===============\r\n", resp_packet_len);
        // 检查数据包是否完整
        if (len < resp_packet_len)
        {
            printf("resp uart_packet_handler: packet is incomplete\r\n");
            if (event_handler)
                event_handler(UART_EVENT_TIMEOUT);
            return;
        }

        // 校验包尾
        if (data[resp_packet_len - 1] != PACKET_TRAILER_RESP)
        {
            printf("resp uart_packet_handler: packet trailer error\r\n");
            if (event_handler)
                event_handler(UART_EVENT_INVALID_CMD);
            return;
        }

        // CRC校验
        if (!verify_crc(data, resp_packet_len))
        {
            if (event_handler)
                event_handler(UART_EVENT_CRC_ERROR);
            return;
        }

        // 提取CMD和Payload
        uint8_t cmd = data[1];
        printf("cmd : %02x\r\n", cmd);
        // uint8_t temppayload_len = data[2];
        // printf("the payload data : \r\n");
        // printf_buf(data + 3, payload_len);
        
        uint8_t code_with_payload_len = 1 + resp_payload_len;
        uint8_t code_with_payload_data[code_with_payload_len];
        code_with_payload_data[0] = data[2];
        memcpy(code_with_payload_data + 1, data + 5, resp_payload_len);

        printf("resp the code_with_payload_data : \r\n");
        printf_buf(code_with_payload_data, code_with_payload_len); 

        if (resp_packet_handler)
        {
            resp_packet_handler(cmd, code_with_payload_data, code_with_payload_len);
        }
    }
    else
    {
        printf("================ invalid packet header !===============\r\n");
        if (event_handler)
            event_handler(UART_EVENT_INVALID_CMD);
        return;
    }
}

// UART任务主循环
static int app_uart_task(void *arg)
{
    printf("==========================app_uart_task start ==================================\r\n");
    int ret = 0;
    g_dev_uart_hdl = app_uart_dev_init("uart0");
    memset(g_recv_buf, 0, UART_RECV_BUF_SIZE);
    // memset(g_send_buf, 0, UART_RECV_BUF_SIZE);
    memset(packet_buf, 0, MAX_PAYLOAD_LEN + 16);

    while (ENABLE_APP_UART_TASK)
    {
        // int len = dev_read(g_dev_uart_hdl, g_recv_buf, UART_RECV_BUF_SIZE);
        int len = dev_read(g_dev_uart_hdl, g_recv_buf, 256);
        printf("========uart recv len:%d =========\n", len);
        printf_buf(g_recv_buf, len);
        if (len > 0)
        {
            // 数据拼接到缓存
            memcpy(packet_buf + packet_idx, g_recv_buf, len);
            packet_idx += len;
            printf("================the packet_idx : %d ==============\r\n", packet_idx);

            // 检查是否超过最大缓冲区长度
            if (packet_idx >= UART_RECV_BUF_SIZE) {
                printf("Packet buffer overflow, clearing buffer.\n");
                packet_idx = 0;
                memset(packet_buf, 0, sizeof(packet_buf));
                continue; // 跳过本次循环，重新读取数据
            }


            // 搜索包头并解析数据包
            for (int i = 0; i < packet_idx; i++)
            {
                if (packet_buf[i] == PACKET_HEADER_CMD)
                {
                    printf("================ the header is ok ! ===================\r\n");
                    // 提取LEN字段
                    uint16_t payload_len = (packet_buf[i + 2] << 8) | packet_buf[i + 1];
                    uint16_t packet_len = 1 + 2 + payload_len + 2 + 1; // HEADER(1) + LEN(2) + PAYLOAD(LEN) + CRC(2) + TRAILER(1)
                    printf("============the payload_len : %d ===============\r\n", payload_len);
                    // 检查数据包是否完整
                    if (packet_idx >= i + packet_len)
                    {
                        printf(" ==================== the data is whole ! ==============\r\n");
                        /* 把要传入去解析的数据先重新存储 */
                        static u8 temp_packet_buf[MAX_PAYLOAD_LEN] = {0};
                        memcpy(temp_packet_buf, packet_buf + i, packet_len);
                        printf_buf(temp_packet_buf, packet_len);
                        parse_uart_packet(temp_packet_buf, packet_len);

                        // 移除已处理数据
                        packet_idx -= (i + packet_len);
                        memmove(packet_buf, packet_buf + i + packet_len, packet_idx);
                        i = -1; // 重置搜索位置
                    }
                }
                else if (packet_buf[i] == PACKET_HEADER_RESP)
                {
                    printf("============ resp the head: PACKET_HEADER_RESP=====================\r\n");
                    // 提取LEN字段
                    uint16_t resp_payload_len = (packet_buf[i + 4] << 8) | packet_buf[i + 3];
                    uint16_t resp_packt_len = 1 + 1 + 1 + 2 + resp_payload_len + 2 + 1; // HEADER(1) + CMD(1) + CODE(1) + LEN(2) + PAYLOAD(LEN) + CRC(2) + TRAILER(1)
                    printf("resp_payload_len: %d\r\n", resp_payload_len);

                    // 检查数据包是否完整
                    if (packet_idx >= i + resp_packt_len)
                    {
                        printf(" ==================== resp the data is whole ! ==============\r\n");
                        /* 把要传入去解析的数据先重新存储 */
                        static u8 temp_resp_packet_buf[MAX_PAYLOAD_LEN] = {0};
                        memcpy(temp_resp_packet_buf, packet_buf + i, resp_packt_len);
                        printf_buf(temp_resp_packet_buf, resp_packt_len);
                        parse_uart_packet(temp_resp_packet_buf, resp_packt_len);

                        // 移除已处理数据
                        packet_idx -= (i + resp_packt_len);
                        memmove(packet_buf, packet_buf + i + resp_packt_len, packet_idx);
                        i = -1; // 重置搜索位置
                    }
                }
            }
        }
        else if (len == UART_RECV_TIMEOUT)
        {
            if (event_handler)
                event_handler(UART_EVENT_TIMEOUT);
        }
        else if (len == UART_CIRCULAR_BUFFER_WRITE_OVERLAY)
        {
            if (event_handler)
                event_handler(UART_EVENT_OVERFLOW);
        }
    }
    return 0;
}

// UART设备初始化
void *app_uart_dev_init(const char *device_name)
{

    void *hdl = dev_open(device_name, NULL);
    if (!hdl)
    {
        printf("UART open failed!\n");
        return NULL;
    }

    uart_register_event_handler(handle_event);
    uart_register_packet_handler(handle_packet);
    uart_register_resp_packet_handler(handle_resp_packet);

    memset(dev_recv_buf, 0, sizeof(dev_recv_buf));
    dev_ioctl(hdl, UART_SET_CIRCULAR_BUFF_ADDR, (int)dev_recv_buf);
    dev_ioctl(hdl, UART_SET_CIRCULAR_BUFF_LENTH, sizeof(dev_recv_buf));
    dev_ioctl(hdl, UART_SET_RECV_BLOCK, UART_BLOCK_CONFG);
    dev_ioctl(hdl, UART_SET_RECV_TIMEOUT, UART_RECV_TIMEOUT);
    dev_ioctl(hdl, UART_START, 0);

    g_dev_uart_hdl = hdl;
    return hdl;
}

// 任务创建
static int uart_task_main(void)
{
    os_task_create(app_uart_task, NULL, 10, 1000, 0, "app_uart_task");
    return 0;
}
late_initcall(uart_task_main);