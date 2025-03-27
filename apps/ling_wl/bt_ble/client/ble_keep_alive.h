#ifndef __BLE_KEEP_ALIVE_H__
#define __BLE_KEEP_ALIVE_H__

#include "cpu.h"
#include "app_mode/lwl_eend_device.h"

typedef enum {
    BKA_CMD_CODE_COMPL,
    BKA_CMD_CODE_DEVLOST,
    BKA_CMD_CODE_UNEXPECTED,
} bka_cmd_code_e;

typedef void (*bka_cmd_cb_funp_t)(bka_cmd_code_e code, void *);

void ble_keep_alive_init(void);

/**
 * @brief 添加一个握手设备
 * @param[in] p_eend_dev 设备引用
 */
void ble_keep_alive_device_add(lwl_eend_dev_t *p_eend_dev);

/**
 * @brief 移除一个已添加的设备
 * @param[in] p_eend_dev 设备引用
 */
void ble_keep_alive_device_remove(lwl_eend_dev_t *p_eend_dev);

/**
 * 
 * @brief 往指定设备的控制指令队列中添加一条指令
 * 
 * @param[in] p_eend_dev 设备引用
 * @param[in] cmd_buffer 控制指令数据
 * @param[in] size 控制指令数据长度
 * @param[in] cb 函数指针，控制指令传输完成时的回调
 * @param[in] priv 用户私有数据
 * 
 * @return 0: 指令添加成功, 
 *         1: 设备列表中未找到指定sn的设备, 
 *         2: 对应设备的控制指令队列已满
 */
int ble_keep_alive_device_push_cmd(lwl_eend_dev_t *p_eend_dev, uint8_t *cmd_buffer, uint32_t size, bka_cmd_cb_funp_t cb, void *priv);

#endif