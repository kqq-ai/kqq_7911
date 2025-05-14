#ifndef __BLE_KEEP_ALIVE_H__
#define __BLE_KEEP_ALIVE_H__

#include "cpu.h"
#include "app_mode/lwl_eend_device.h"

#define KEEP_ALIVE_HANDSHAKE_LOST_CNT 10

typedef enum {
    BKACODE_Success = 0,
    BKACODE_Dev_Not_Found = -1,
    BKACODE_Cmd_Queue_Full = -2,
    BKACODE_Cmd_Not_Found = -3,
} bka_ret_code_e;

typedef enum {
    BKA_CMD_CODE_COMPL,
    BKA_CMD_CODE_DEVLOST,
    BKA_CMD_CODE_BUFFER_FULL,
    BKA_CMD_CODE_BIG_CONNECTION_TIMEOUT,
    BKA_CMD_CODE_UNEXPECTED,
} bka_cmd_code_e;

void ble_keep_alive_init(void);

/**
 * 
 * @brief 往指定设备的控制指令队列中添加一条指令
 * 
 * @param[in]  p_eend_dev 设备引用
 * @param[in]  cmd_buffer 控制指令数据
 * @param[in]  size 控制指令数据长度
 * @param[in]  cb 函数指针，控制指令传输完成时的回调
 * @param[in]  priv 用户私有数据
 * @param[out] cmd_id_out 指令ID
 * 
 * @return BKACODE_Success       : 指令添加成功, 
 *         BKACODE_Not_Found     : 设备列表中未找到指定sn的设备, 
 *         BKACODE_Cmd_Queue_Full: 对应设备的控制指令队列已满
 */
int ble_keep_alive_device_push_cmd(lwl_eend_dev_t *p_eend_dev, uint8_t *cmd_buffer, uint32_t size, bka_cmd_cb_funp_t cb, void *priv, uint32_t *cmd_id_out);

/**
 * @brief 删除指定设备的控制指令队列中的指定指令
 * 
 * @param[in] p_eend_dev 指定设备引用
 * @param[in] cmd_id 指定指令ID
 * 
 * @return BKACODE_Success : 删除成功
 *         BKACODE_Dev_Not_Found : 设备列表中未找到指定设备引用
 *         BKACODE_Cmd_Not_Found : 设备的控制指令队列中未找到指定指令ID的指令
 */
int ble_keep_alive_device_remove_cmd(lwl_eend_dev_t *p_eend_dev, uint32_t cmd_id);

#endif