#ifndef __LWL_EEND_DEVICE_H__
#define __LWL_EEND_DEVICE_H__

#include "cpu.h"

#include "os/os_api.h"
#include "util/cqueue.h"
#include "btstack/le/le_common_define.h"
#include "bt_ble/client/ble_att_util.h"

#define SN_SIZE 16 /**< Size of the serial number buffer */

#define MAX_SAMPLES 1000 /**< Size of the RSSI sample array */

typedef enum {
    BDO_STATE_IDLE              = 0x00,         //!< 无操作
    BDO_STATE_ACTIVATING        = 0x01,         //!< 正在激活中
    BDO_STATE_TURNNING_ON       = 0x02,         //!< 正在开机
    BDO_STATE_TURNNING_OFF      = 0x04,         //!< 正在关机
} bdo_state_e;

enum distance_type {
    DISTANCE_0,                                 //!< 小于2米
    DISTANCE_1,                                 //!< 2米到5米
    DISTANCE_2,                                 //!< 大于5米
    DISTANCE_3,                                 //!< RSSI等于-128或没有检测到
};


typedef enum {
    BKACODE_Success = 0,
    BKACODE_Null_Pointer = -1,
    BKACODE_Dev_Not_Found = -2,
    BKACODE_Cmd_Queue_Full = -3,
    BKACODE_Cmd_Not_Found = -4,
} bka_ret_code_e;

/**
 * @brief 握手通信指令执行状态
 */
typedef enum {
    BKA_CMD_CODE_COMPL,                         //!< 指令执行完成
    BKA_CMD_CODE_DEVLOST,                       //!< 握手通信设备丢失
    BKA_CMD_CODE_BUFFER_FULL,                   //!< 蓝牙发送缓冲满了
    BKA_CMD_CODE_SEARCH_PROFILE_TIMEOUT,        //!< 蓝牙查询GATT profile超时
    BKA_CMD_CODE_E_TURN_ON_TIMEOUT,             //!< 开机指令失败，回复超时
    BKA_CMD_CODE_E_TURN_ON_ALREADY_BOUND,       //!< 开机指令失败，E端已经被其他设备绑定
    BKA_CMD_CODE_UNEXPECTED,                    //!< 意外的错误
} bka_cmd_code_e;

/**
 * @brief C与E之间BLE协议的开机返回状态
 */
typedef enum {
    BKA_TURN_ON_SUCCESS               = 0x00,   //!< 开机成功
    BKA_TURN_ON_FAILED_ALREADY_BOUND  = 0x01,   //!< 开机失败，设备已被其他设备绑定
} bka_turn_on_resp_status_e;

typedef void (*bka_cmd_cb_funp_t)(bka_cmd_code_e code, void *);


typedef struct {
    int rssi_sample_count;
    s8 rssi_samples[MAX_SAMPLES];
    u32 rssi_timestamps[MAX_SAMPLES];
    s8 rssi;
    int distance_type;
    int distance_type_last;
    OS_MUTEX mutex;
} distance_info_t;

#define KEEP_ALIVE_CMD_DATA_SIZE 16
#define KEEP_ALIVE_CMD_NUM 32

typedef struct {
    uint32_t cmd_id;
    uint8_t data[KEEP_ALIVE_CMD_DATA_SIZE];
    uint8_t size;
    bka_cmd_cb_funp_t cb;
    void *priv;
} bka_cmd_t;

/**
 * @brief Structure representing an LWL EEND device
 */
typedef struct {
    u8 sn[SN_SIZE];                               //!< Serial number of the device

    u8 bat_level;                                 //!< E 端设备电量

    u8 adv_state;                                 //!< E 端 ADV 状态 详情见 bdp_adv_stat_e

    u8 op_state;                                  //!< 正在进行的操作状态 详情见 bdo_state_e

    u8 has_deadline;                              //!< E 端 ADV 是否包含广播配对码
    s32 deadline;                                 //!< E 端 激活状态截止时间

    u8 address_type;                              //!< 用于连接 对方地址类型：0--public address,1--random address 
    u8 address[6];                                //!< 用于连接 peer_address 

    u8 ctrl_mask_flags;                           //!< 广播自定义数据 E 端控制字节 详情见 bdp_ctrl_mask_flags_t 和 bdp_ctrl_mask_e

    uint32_t active_time;                         //!< 最近一次激活时间

    uint32_t update_time;                         //!< 最近一次更新广播时间

    distance_info_t distance_info;                //!< Distance information

    /**
     * @brief Callback to match advertisement data
     * @param __this Pointer to the device instance
     * @param adv_data Advertisement data buffer
     * @param adv_len Length of advertisement data
     * @return true if advertisement matches, false otherwise
     */
    bool (*adv_match)(void *__this, u8 *adv_data, u16 adv_len);
    /**
     * @brief Callback to get device information as string
     * @param __this Pointer to the device instance
     * @param buffer Output buffer for the string
     * @param size Size of the output buffer
     */
    void (*get_string)(void *__this, char *buffer, int size);
    /**
     * @brief Callback to set ctrl byte
     * @param __this Pointer to the device instance
     * @param on E-end on/off state
     * @param play E-end audio play state
     */
    void (*set_ctrl)(void *__this, uint8_t on, uint8_t play);
} lwl_eend_dev_t;

/**
 * @brief Preliminary advertisement data matching implementation
 * @param adv_data Pointer to advertisement data
 * @param adv_len Length of advertisement data
 * @return true if preliminary match succeeds, false otherwise
 * @note This is typically used for basic filtering before full match
 */
bool adv_pre_match_impl(const u8 *adv_data, const u16 adv_len);

/**
 * @brief Default advertisement data matching implementation
 * @param __this Pointer to the device instance
 * @param adv_data Pointer to advertisement data
 * @param adv_len Length of advertisement data
 * @return true if advertisement matches device criteria, false otherwise
 */
bool adv_match_impl(const void *__this, const u8 *adv_data, const u16 adv_len);

/**
 * @brief Default implementation to get device information as string
 * @param __this Pointer to the device instance
 * @param buffer Output buffer for the string
 * @param size Size of the output buffer
 * @note The string will be null-terminated
 */
void get_string_impl(const void *__this, char *buffer, const int size);


/**
 * @brief Default implementation to set ctrl byte
 * @param __this Pointer to the device instance
 * @param on E-end on/off state
 * @param play E-end audio play state
 */
void set_ctrl_impl(void *__this, uint8_t on, uint8_t play);

/**
 * @brief Initialize an LWL EEND device structure
 * @param sn Serial number to assign to the device
 * @param dev Pointer to the device structure to initialize
 * @note This function sets up the default callbacks (adv_match_impl and get_string_impl)
 */
void lwl_eend_dev_init(const u8 *sn, lwl_eend_dev_t *dev);


#endif
