#include "app_config.h"
#include "system/includes.h"
#include "device/device.h"
#include "device/iic.h"
#include "gpio.h"

#define IIC_WRCMD 0x32 // I²C 写地址命令
#define IIC_RDCMD 0x33 // I²C 读地址命令

#define SC7A20H_ADDR 0x19 // I²C 地址（SDO接高电平） ---> 0x19

// SC7A20H 寄存器地址

// ----------------------------
// SC7A20H 控制寄存器
// ----------------------------
#define SC7A20H_CTRL_REG1 0x20 // 控制寄存器1（ODR、XYZ轴使能）
#define SC7A20H_CTRL_REG2 0x21 // 控制寄存器2（高通滤波）
#define SC7A20H_CTRL_REG3 0x22 // 控制寄存器3（中断配置）
#define SC7A20H_CTRL_REG4 0x23 // 控制寄存器4（量程、滤波器）
#define SC7A20H_CTRL_REG5 0x24 // 控制寄存器5（中断锁存）
#define SC7A20H_CTRL_REG6 0x25 // 控制寄存器6（中断引脚电平）

// ----------------------------
// SC7A20H 数据寄存器
// ----------------------------
#define SC7A20H_STATUS_REG 0x27 // 状态寄存器（数据就绪标志）
#define SC7A20H_OUT_X_L 0x28    // X 轴低字节
#define SC7A20H_OUT_X_H 0x29    // X 轴高字节
#define SC7A20H_OUT_Y_L 0x2A    // Y 轴低字节
#define SC7A20H_OUT_Y_H 0x2B    // Y 轴高字节
#define SC7A20H_OUT_Z_L 0x2C    // Z 轴低字节
#define SC7A20H_OUT_Z_H 0x2D    // Z 轴高字节

// ----------------------------
// SC7A20H 中断相关寄存器
// ----------------------------
#define SC7A20H_AOI1_CFG 0x30      // 中断1配置寄存器（方向检测模式）
#define SC7A20H_AOI1_SRC 0x31      // 中断1状态寄存器（中断触发标志）
#define SC7A20H_AOI1_THS 0x32      // 中断1阈值寄存器（触发阈值设置）
#define SC7A20H_AOI1_DURATION 0x33 // 中断1持续时间寄存器（触发持续时间）
#define SC7A20H_INT1_CFG 0x30      // 中断1配置寄存器（同 AOI1_CFG）
#define SC7A20H_INT1_SRC 0x31      // 中断1状态寄存器（同 AOI1_SRC）


int iic_write_one_byte(void *iic_hdl, unsigned char addr, unsigned char data);
int iic_read_one_byte(void *iic_hdl, unsigned char addr, unsigned char *data);
int read_accel_data(void *iic_hdl, int16_t *x, int16_t *y, int16_t *z);
void *sc7a20_init(const char *name); /* iic name */
void sc7a20_exit(void *iic_hdl);
/* 
    返回值 1 ：happend
    返回值 0 ：not happend
    返回值 -1 ：iic error
*/
int sc7a20_interrupt_detect(void *iic_hdl);

int sc7a20_clear_interrupt(void *iic_hdl);