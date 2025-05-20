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
    DISTANCE_0, // 小于2米
    DISTANCE_1, // 2米到5米
    DISTANCE_2, // 大于5米
    DISTANCE_3, // RSSI等于-128或没有检测到
};

typedef struct {
    int rssi_sample_count;
    s8 rssi_samples[MAX_SAMPLES];
    s8 rssi;
    int distance_type;
    int distance_type_last;
    OS_MUTEX mutex;
} distance_info_t;


typedef void (*bka_cmd_cb_funp_t)(int code, void *);

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
    u8 sn[SN_SIZE];                     /**< Serial number of the device */

    distance_info_t distance_info;      /**< Distance information */

    bka_eend_dev_handshake_info_t handshake_info; /**< Handshake information */

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

#endif
