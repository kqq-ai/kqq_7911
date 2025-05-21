#include "SC7A20/sc7a20.h"


int iic_write_one_byte(void *iic_hdl, unsigned char addr, unsigned char data)
{
    int ret = 0;
    dev_ioctl(iic_hdl, IIC_IOCTL_START, 0); // IIC 锁定
    if (dev_ioctl(iic_hdl, IIC_IOCTL_TX_WITH_START_BIT, IIC_WRCMD))
    {
        ret = -1;
        printf("iic write err in IIC_IOCTL_TX_WITH_START_BIT \n");
        goto exit;
    }
    delay(1000);
    if (dev_ioctl(iic_hdl, IIC_IOCTL_TX, addr))
    {
        ret = -1;
        printf("iic write err in IIC_IOCTL_TX\n");
        goto exit;
    }
    delay(1000);
    if (dev_ioctl(iic_hdl, IIC_IOCTL_TX_WITH_STOP_BIT, data))
    {
        ret = -1;
        printf("iic write err in IIC_IOCTL_TX_WITH_STOP_BIT\n");
        goto exit;
    }
    delay(1000);

exit:
    dev_ioctl(iic_hdl, IIC_IOCTL_STOP, 0); // IIC 解锁
    return ret;
}
int iic_read_one_byte(void *iic_hdl, unsigned char addr, unsigned char *data)
{
    int ret = 0;
    dev_ioctl(iic_hdl, IIC_IOCTL_START, 0); // IIC 锁定
    if (dev_ioctl(iic_hdl, IIC_IOCTL_TX_WITH_START_BIT, IIC_WRCMD))
    {
        ret = -1;
        printf("iic write err in IIC_IOCTL_TX_WITH_START_BIT\n");
        goto exit;
    }
    delay(1000);
    if (dev_ioctl(iic_hdl, IIC_IOCTL_TX_WITH_STOP_BIT, addr))
    {
        ret = -1;
        printf("iic write err IIC_IOCTL_TX_WITH_STOP_BIT\n");
        goto exit;
    }
    delay(1000);
    if (dev_ioctl(iic_hdl, IIC_IOCTL_TX_WITH_START_BIT, IIC_RDCMD))
    {
        ret = -1;
        printf("iic write err IIC_IOCTL_TX_WITH_START_BIT\n");
        goto exit;
    }
    delay(1000);

    dev_ioctl(iic_hdl, IIC_IOCTL_RX_WITH_STOP_BIT, (u32)data); // 读数据 + 结束信号

exit:
    dev_ioctl(iic_hdl, IIC_IOCTL_STOP, 0); // IIC 解锁
    return ret;
}
int read_accel_data(void *iic_hdl, int16_t *x, int16_t *y, int16_t *z)
{
    uint8_t status;
    uint8_t data[6];

    // 检查是否有新数据
    if (iic_read_one_byte(iic_hdl, SC7A20H_STATUS_REG, &status) != 0)
    {
        printf("Failed to read status register\n");
        return -1;
    }

    if (!(status & 0x08))
    { // 检查 ZYXDA 位
        printf("No new data available\n");
        return -1;
    }

    // 读取 X、Y、Z 轴数据
    if (iic_read_one_byte(iic_hdl, SC7A20H_OUT_X_L, &data[0]) != 0 ||
        iic_read_one_byte(iic_hdl, SC7A20H_OUT_X_H, &data[1]) != 0 ||
        iic_read_one_byte(iic_hdl, SC7A20H_OUT_Y_L, &data[2]) != 0 ||
        iic_read_one_byte(iic_hdl, SC7A20H_OUT_Y_H, &data[3]) != 0 ||
        iic_read_one_byte(iic_hdl, SC7A20H_OUT_Z_L, &data[4]) != 0 ||
        iic_read_one_byte(iic_hdl, SC7A20H_OUT_Z_H, &data[5]) != 0)
    {
        printf("Failed to read acceleration data\n");
        return -1;
    }

    // 将数据拼接为 16 位整数
    *x = (int16_t)((data[1] << 8) | data[0]);
    *y = (int16_t)((data[3] << 8) | data[2]);
    *z = (int16_t)((data[5] << 8) | data[4]);

    return 0;
}

 /* iic name */
void * sc7a20_init(const char *name)
{
    int ret = 0;
    // 1. 打开 I²C 设备
    void *iic_hdl = dev_open("iic0", NULL);
    if (!iic_hdl)
    {
        printf("Failed to open I²C device\n");
        goto exit;
    }

    // 2. 配置 SC7A20H
    // 设置输出数据率为 100Hz，XYZ轴使能
    if (iic_write_one_byte(iic_hdl, SC7A20H_CTRL_REG1, 0x47) != 0)
    {
        printf("Failed to configure CTRL_REG1\n");
        goto exit;
    }

    printf("iic_test_task start    CONFIG   4G=====================================\r\n");
    // 设置量程为 ±2g
    if (iic_write_one_byte(iic_hdl, SC7A20H_CTRL_REG4, 0x88) != 0)
    {
        printf("Failed to configure CTRL_REG4\n");
        goto exit;
    }
    // 3. 配置方向检测中断
    // 启用 AOI1 中断到 INT1
    if (iic_write_one_byte(iic_hdl, SC7A20H_CTRL_REG3, 0x40) != 0)
    {
        printf("Failed to configure CTRL_REG3\n");
        goto exit;
    }

    //  开启高通滤波
    if (iic_write_one_byte(iic_hdl, SC7A20H_CTRL_REG2, 0x31) != 0)
    {
        printf("Failed to configure CTRL_REG2\n");
        goto exit;
    }

    // 启用 6D 方向检测
    if (iic_write_one_byte(iic_hdl, SC7A20H_AOI1_CFG, 0x2a) != 0)
    {
        printf("Failed to configure AOI1_CFG\n");
        goto exit;
    }

    // 设置方向检测阈值为 32mg
    if (iic_write_one_byte(iic_hdl, SC7A20H_AOI1_THS, 0x05) != 0)
    {
        printf("Failed to configure AOI1_THS\n");
        goto exit;
    }

    // 设置方向检测持续时间为 2 个采样周期
    if (iic_write_one_byte(iic_hdl, SC7A20H_AOI1_DURATION, 0x04) != 0)
    {
        printf("Failed to configure AOI1_DURATION\n");
        goto exit;
    }

    // 启用 INT1 锁存
    if (iic_write_one_byte(iic_hdl, SC7A20H_CTRL_REG5, 0x08) != 0)
    {
        printf("Failed to configure CTRL_REG5\n");
        goto exit;
    }

    // 设置 INT1 为低电平触发
    if (iic_write_one_byte(iic_hdl, SC7A20H_CTRL_REG6, 0x02) != 0)
    {
        printf("Failed to configure CTRL_REG6\n");
        goto exit;
    }

    return iic_hdl;
exit:
    printf("config sc7a20 error !\r\n");
    if (iic_hdl)
    {
        dev_close(iic_hdl);
    }
    return NULL;
}
void sc7a20_exit(void * iic_hdl)
{
    if (iic_hdl)
    {
        dev_close(iic_hdl);
    }
}

int sc7a20_interrupt_detect(void * iic_hdl)
{
    uint8_t int_status;
    if (iic_read_one_byte(iic_hdl, SC7A20H_INT1_SRC, &int_status) == 0)
    {
        if (int_status & 0x40) { // 检查 IA 位
            printf("interrupt happend!\r\n");           
            return 1;
        } else {
            printf("not interrupt happend!\r\n");
            return 0;
        }
    }
    else {
        printf("Failed to read interrupt status\r\n");
        return -1;
    }
}
int sc7a20_clear_interrupt(void * iic_hdl)
{
    if (iic_write_one_byte(iic_hdl, SC7A20H_AOI1_SRC, 0x00) != 0)
    {
        printf("Failed to clear interrupt\r\n");
    }
}