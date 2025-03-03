#include "app_crc16.h"

#include "stdint.h"
#include "cpu.h"

// CRC-16/MODBUS 计算函数
uint16_t crc16_modbus(const uint8_t *data, uint16_t len)
{
    uint16_t crc = 0xFFFF; // 初始值
    for (uint16_t i = 0; i < len; i++)
    {
        crc ^= (uint16_t)data[i]; // 异或当前字节
        for (uint8_t j = 0; j < 8; j++)
        {
            if (crc & 0x0001) // 检查最低位
            {
                crc >>= 1;
                crc ^= 0xA001; // 多项式 0x8005 的反转
            }
            else
            {
                crc >>= 1;
            }
        }
    }
    return crc;
}


// 计算数据包的 CRC 校验值
uint16_t calculate_crc(const uint8_t *data, uint16_t len)
{
    return crc16_modbus(data, len);
}

// 校验数据包的 CRC
bool verify_crc(const uint8_t *data, uint16_t len)
{
    // 提取接收到的 CRC 值（小端序）
    uint16_t crc_received = (data[len - 2] << 8) | data[len - 3];

    log_d("crc_received: %04x\n", crc_received);

    // 计算 CRC 校验值（HEADER + LEN + PAYLOAD）
    uint16_t crc_calculated = crc16_modbus(data, len - 3);

    log_d("crc_calculated: %04x\n", crc_calculated);

    // 比较接收到的 CRC 和计算得到的 CRC
    return (crc_received == crc_calculated);
}