#ifndef __LWL_EEND_DEVICE_H__
#define __LWL_EEND_DEVICE_H__

#include "cpu.h"

#include "os/os_api.h"
#include "util/cqueue.h"
#include "btstack/le/le_common_define.h"
#include "bt_ble/client/ble_att_util.h"

#define SN_SIZE 16 /**< Size of the serial number buffer */

#define MAX_SAMPLES 1000 /**< Size of the RSSI sample array */

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

typedef struct {
    bool need_handshake;   //!< 是否需要进行握手通信
    bool alive;
    bool handshake_flag;    // 握手标志，0表示没有握手，1表示已握手
    uint8_t alive_hs_fail_cnt;  // 保活判断中，连续握手失败的次数
    uint8_t address_type;  //冗余 用于连接 对方地址类型：0--public address,1--random address 
    uint8_t address[6];    //冗余 用于连接 peer_address 

    MyBlockingQueue cmd_buffer; // queue<bka_cmd_t>
#if 1
    uint8_t cache_buf[(sizeof(bka_cmd_t) * KEEP_ALIVE_CMD_NUM)]__attribute__((aligned(4)));
#else
    bka_cmd_t cache_buf[KEEP_ALIVE_CMD_NUM]__attribute__((aligned(4)));
#endif
    OS_MUTEX mutex;
} bka_eend_dev_handshake_info_t;

/**
 * @brief Structure representing an LWL EEND device
 */
typedef struct {
    u8 sn[SN_SIZE];                               //!< Serial number of the device

    u8 bat_level;                                 //!< E端设备电量

    u8 power_on_state;                            //!< E端开机状态

    u8 bound_mac[6];                              //!< E端绑定的C端MAC

    distance_info_t distance_info;                //!< Distance information

    bka_eend_dev_handshake_info_t handshake_info; //!< Handshake information

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
 * @brief Initialize an LWL EEND device structure
 * @param sn Serial number to assign to the device
 * @param dev Pointer to the device structure to initialize
 * @note This function sets up the default callbacks (adv_match_impl and get_string_impl)
 */
void lwl_eend_dev_init(const u8 *sn, lwl_eend_dev_t *dev);



/*===================================================================================================
 *  
 *===================================================================================================*/

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

/**
 * @brief 往指定设备的控制指令队列中添加一条开机指令
 * 
 * @param[in]  p_eend_dev 设备引用
 * @param[in]  cb 函数指针，控制指令传输完成时的回调
 * @param[in]  priv 用户私有数据
 * @param[out] cmd_id_out 指令ID
 * 
 * @return BKACODE_Success       : 指令添加成功
 *         BKACODE_Cmd_Queue_Full: 对应设备的控制指令队列已满
 * 
 * @note 执行cb时可能会持有部分锁
 *       - p_eend_dev->handshake_info.mutex
 */
int bka_e_turn_on(lwl_eend_dev_t *p_eend_dev, bka_cmd_cb_funp_t cb, void *priv, uint32_t *cmd_id_out);

/**
 * @brief 往指定设备的控制指令队列中添加一条关机指令
 * 
 * @param[in]  p_eend_dev 设备引用
 * @param[in]  cb 函数指针，控制指令传输完成时的回调
 * @param[in]  priv 用户私有数据
 * @param[out] cmd_id_out 指令ID
 * 
 * @return BKACODE_Success       : 指令添加成功
 *         BKACODE_Cmd_Queue_Full: 对应设备的控制指令队列已满
 * 
 * @note 执行cb时可能会持有部分锁
 *       - p_eend_dev->handshake_info.mutex
 */
int bka_e_turn_off(lwl_eend_dev_t *p_eend_dev, bka_cmd_cb_funp_t cb, void *priv, uint32_t *cmd_id_out);

/**
 * @brief 删除指定设备的控制指令队列中的指定指令
 * 
 * @param[in] p_eend_dev 指定设备引用
 * @param[in] cmd_id 指定指令ID
 * 
 * @return BKACODE_Success : 删除成功
 *         BKACODE_Cmd_Not_Found : 设备的控制指令队列中未找到指定指令ID的指令
 */
int bka_cmd_remove(lwl_eend_dev_t *p_eend_dev, uint32_t cmd_id);

#endif
