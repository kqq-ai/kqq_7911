#ifndef __BLE_DEV_TASK_SCHEDULER_H__
#define __BLE_DEV_TASK_SCHEDULER_H__

#include "app_mode/lwl_eend_device.h"

typedef enum {
    BDOCode_Success = 0,            //!< 操作成功通用返回码
    BDOCode_Skip,                   //!< 操作被跳过
    BDOCode_Unknown_Broadcast_Code, //!< 操作失败，广播码未知
    BDOCode_Null_Pointer,           //!< 操作失败，空指针错误
    BDOCode_BLE_Api_Error,          //!< 操作失败，BLE 接口调用失败
    BDOCode_BLE_Buffer_Full,        //!< 操作失败，BLE 缓冲满了
    BDOCode_Connection_Timeout,     //!< 操作失败，BLE 连接超时
    BDOCode_Disconnect_Timeout,     //!< 操作失败，BLE 断连超时
    BDOCode_Scan_Adv_Stat_Timeout,  //!< 操作失败，扫描ADV状态变化超时
    BDOCode_Max_Reconnection_Attempts, //!< 操作失败，超过最大重连次数
    BDOCode_Dev_Busy,               //!< 操作失败，设备繁忙
    BDOCode_Unexpected_Error,       //!< 意外错误
    BDOCode_Ready_to_Play,          //!< 开始播放
} bdo_code_e;

/**
 * @brief E 端设备操作执行完毕回调类型
 * @param code 操作执行情况. @ref bdo_code_e
 * @param priv 用户参数.
 */
typedef void (*ble_dev_op_cb_func_t)(bdo_code_e code, void *priv, const char *sn);

typedef struct {
    int conn_timeout;               //!< 连接超时时间，单位 ms
    uint8_t address_type;           //!< 用于连接 对方地址类型：0--public address,1--random address 
    uint8_t address[6];             //!< 用于连接 peer_address 
} bdo_active_t;

typedef struct {
    int conn_timeout;               //!< 连接超时时间，单位 ms
    int scan_timeout;               //!< 扫描ADV状态变化超时时间，单位 ms
    uint8_t address_type;           //!< 用于连接 对方地址类型：0--public address,1--random address 
    uint8_t address[6];             //!< 用于连接 peer_address 
    uint8_t broadcast_code[6];      //!< 广播配对码，快照
} bdo_turn_on_t;

typedef struct {
    int scan_timeout;               //!< 扫描ADV状态变化超时时间，单位 ms
} bdo_turn_off_t;

typedef struct {
    uint8_t sn[PI_SN_SIZE+1];            //!< 设备序列号
    uint8_t op_type;                //!< 操作码 详情见 bdp_dev_op_type_e
    union {
        bdo_active_t active;        //!< 激活操作参数
        bdo_turn_on_t turnon;       //!< 开机操作参数
        bdo_turn_off_t turnoff;     //!< 关机操作参数
    };
    ble_dev_op_cb_func_t cb;        //!< E 端设备操作执行完毕回调
    void *cb_priv;                  //!< 回调参数
} ble_dev_op_t;

/**
 * @brief E 端设备操作处理函数类型
 * @param op 操作信息结构
 */
typedef void (*ble_dev_op_handler_func_t)(ble_dev_op_t* op);

typedef struct {
    uint8_t op;                     //!< 操作码 详情见 bdp_dev_op_type_e
    ble_dev_op_handler_func_t handler;  //!< 操作具体处理函数
} ble_dev_op_handler_entry;

void bdo_disable(bool mute);
void bdo_enable();
int bdo_active(lwl_eend_dev_t *dev, int timeout, ble_dev_op_cb_func_t cb, void *priv);
int bdo_turn_on(lwl_eend_dev_t *dev, int conn_timeout, int scan_timeout, ble_dev_op_cb_func_t cb, void *priv);
int bdo_turn_off(lwl_eend_dev_t *dev);

void bdo_notify_e_conncted(char sn[]);
void bdo_notify_e_connecting(char sn[]);
void bdo_notify_e_disconncted(char sn[]);

void ble_dev_task_scheduler_init();
void ble_dev_task_scheduler_start();

#endif // __BLE_DEV_TASK_SCHEDULER_H__
