#ifndef __ASYNC_MSG_LOOP_H__
#define __ASYNC_MSG_LOOP_H__

#include "cpu.h"
#include "wifi_app_task.h"

void start_async_msg_loop();

/**
 * @name my_task_switch_play
 * @brief This function is used to create a specific task message and enqueue it into a message queue.
 *
 * @param sn      Pointer to a character string indicating the E-end sequence number.
 * @param audio_url    Pointer to a character string indicating the audio source URL.
 * @param priv    A pointer to user-defined private data.
 * @param cb      A pointer to a callback function.
 */
void my_task_switch_play(char *sn, char* audio_url, void* priv, void* cb);

/**
 * @name my_task_switch_only
 * @brief This function is used to create a specific task message and enqueue it into a message queue.
 *
 * @param name    Pointer to a character string indicating the E-end sequence number.
 * @param priv    A pointer to user-defined private data.
 * @param cb      A pointer to a callback function.
 */
void my_task_switch_only(char *sn, void* priv, void* cb);

/**
 * @name my_task_switch_play
 * @brief This function is used to create a specific task message and enqueue it into a message queue.
 *
 * @param audio_messageId Audio messsage id (LingOS).
 * @param audio_url    Pointer to a character string indicating the audio source URL.
 * @param priv    A pointer to user-defined private data.
 * @param cb      A pointer to a callback function.
 */
void my_task_play_one(int audio_message_id, char* audio_url, void* priv, void* cb);

/**
 * @name my_task_play_one
 * @brief This function is used to create a specific task message and enqueue it into a message queue.
 *
 * @param rssi_cb_priv A pointer to user-defined private data.
 * @param rssi_cb      A pointer to a callback function.
 * @param priv    A pointer to user-defined private data.
 * @param cb      A pointer to a callback function.
 */
void my_task_scan_rssi(void *rssi_cb_priv, 
        void (*rssi_cb)(void *priv, char *dev_name, u16 dev_name_len, s8 rssi), void* priv, void* cb);

/**
 * @name disable_specific_audio_message
 * @brief Cancel the audio message with a specific audio message ID.
 *
 * @param audio_messageId Audio messsage id (LingOS).
 */
void disable_specific_audio_message(int audio_message_id);

/**
 * @name my_task_wifi_scan_start
 * @brief This function is used to create a specific task message and enqueue it into a message queue.
 *
 * @param priv    A pointer to user-defined private data.
 * @param cb      A pointer to a callback function.
 */
void my_task_wifi_scan_start(void* priv, void* cb);

/**
 * @name my_task_wifi_connect
 * @brief This function is used to create a specific task message and enqueue it into a message queue.
 *
 * @param priv    A pointer to user-defined private data.
 * @param cb      A pointer to a callback function.
 */
void my_task_wifi_connect(
    char *ssid,
    char *pwd,
    int timeout_s,
    wifi_connect_succ_cb connect_succ_func,
    wifi_connect_fail_cb connect_fail_func,
    void *priv,
    void *cb);


    /**
 * @name my_task_wifi_disconnect
 * @brief This function is used to create a specific task message and enqueue it into a message queue.
 *
 * @param priv    A pointer to user-defined private data.
 * @param cb      A pointer to a callback function.
 */
void my_task_wifi_disconnect(wifi_disconnect_cb disconnect_func,void *priv, void *cb);

#endif