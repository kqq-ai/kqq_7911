#ifndef __LWL_EEND_DEVICE_H__
#define __LWL_EEND_DEVICE_H__

#include "cpu.h"

#include "btstack/le/le_common_define.h"
#include "bt_ble/client/ble_att_util.h"

#define SN_SIZE 16 /**< Size of the serial number buffer */

/**
 * @brief Structure representing an LWL EEND device
 */
typedef struct {
    u8 sn[SN_SIZE];                     /**< Serial number of the device */
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
