#ifndef __LWL_APP_FRAMEWORK_H__
#define __LWL_APP_FRAMEWORK_H__

#include "cpu.h"
#include "app_core.h"

#define play_system_music(x) lwl_app_mode_play(CONFIG_CP_VOICE_FILE_PATH x);
void lwl_app_mode_play(const char *fname);
bool lwl_app_mode_playing();

/**
 * @brief Application mode enumeration
 */
enum {
    APP_POWER_ON_MODE           = 1, /**< Power-on initialization mode */
    APP_DEFAULT_MODE            = 2, /**< Default operational mode */
    APP_NET_CONNECTING_MODE     = 3, /**< Network connection in progress */
    APP_NET_CONNECTED_MODE      = 4, /**< Network successfully connected */
    APP_UPDATE_MODE             = 5, /**< Firmware update mode */
    APP_WSS_CONNECTED_MODE      = 6, /**< WebSocket successfully connected */
    APP_DEVICE_CHECK_MODE       = 7, /**< Check device and write SN */
};

#define MAX_LE_AUDIO_VOLUME 12

/**
 * @brief Get the current application mode
 * @return Current mode (from the mode enumeration)
 */
u8 lwl_app_mode_get_current_mode();

/**
 * @brief Switch application to a specific mode
 * @param app_task Target mode to switch to (from the mode enumeration)
 * @return 0 on success, negative value on error
 */
int lwl_app_mode_switch_to(u8 app_task);

/**
 * @brief Switch application back to previous mode
 * @return 0 on success, negative value on error
 * @note Uses an internal stack to track mode history
 */
int lwl_app_mode_switch_back();

/**
 * @brief Check if application is currently exiting a mode
 * @return Current exit status (non-zero if exiting, 0 otherwise)
 */
u8 lwl_app_mode_exitting();

/**
 * @brief Signal that mode exit has completed successfully
 * @note Should be called when cleanup for current mode is finished
 */
void lwl_app_mode_exit_ok();

/**
 * @brief Initialize the application framework
 * @param init_mode Initial mode to start in (from the mode enumeration)
 */
void lwl_app_init();

/**
 * @brief Main application mode loop processor
 * @note Should be called periodically from main application loop
 */
void lwl_app_mode_loop();

#endif