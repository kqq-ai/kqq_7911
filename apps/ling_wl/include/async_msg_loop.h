#ifndef __ASYNC_MSG_LOOP_H__
#define __ASYNC_MSG_LOOP_H__

#include "cpu.h"

void start_async_msg_loop();

enum async_task_code_e {
    CODE_EDEV_CMD_SUCCESS,
    CODE_EDEV_CMD_TIMEOUT,
    CODE_EDEV_CMD_UNEXPECTED_ERROR,

    CODE_PLAY_AUDIO_STATE_BEFORE_DEC_START,
    CODE_PLAY_AUDIO_STATE_AFTER_DEC_END,
    CODE_PLAY_AUDIO_STATE_CANCEL,
};

typedef void (*pl_switch_cb_func_t)(int code, void *priv);
typedef void (*pl_music_play_cb_func_t)(int code, void *priv);

/**
 * @name my_task_turn_on_edev
 * @brief This function is used to create a specific task message and enqueue it into a message queue.
 *
 * @param sn      Pointer to a character string indicating the E-end sequence number.
 * @param timeout Switch timeout, 等待超时（时钟节拍），取0时无限等待
 * @param cb      A pointer to a callback function, see TURN_ON_EDEV_CODE_.
 * @param priv    A pointer to user-defined private data.
 */
void my_task_turn_on_edev(char *sn, int timeout, pl_switch_cb_func_t *cb, void *priv);

/**
 * @name my_task_turn_off_edev
 * @brief This function is used to create a specific task message and enqueue it into a message queue.
 *
 * @param name    Pointer to a character string indicating the E-end sequence number.
 * @param timeout Switch timeout, 等待超时（时钟节拍），取0时无限等待
 * @param cb      A pointer to a callback function, see SWITCH_ONLY_CODE_.
 * @param priv    A pointer to user-defined private data.
 */
void my_task_turn_off_edev(char *sn, int timeout, pl_switch_cb_func_t *cb, void *priv);

/**
 * @name my_task_play_one
 * @brief This function is used to create a specific task message and enqueue it into a message queue.
 *
 * @param audio_url    Pointer to a character string indicating the audio source URL.
 * @param cb      A pointer to a callback function.
 * @param priv    A pointer to user-defined private data.
 */
void my_task_play_one(char* audio_url, pl_music_play_cb_func_t cb, void *priv);

/**
 * @name my_task_play_one_lingos
 * @brief This function is used to create a specific task message and enqueue it into a message queue.
 *
 * @param audio_messageId Audio messsage id (LingOS).
 * @param audio_url    Pointer to a character string indicating the audio source URL.
 * @param cb      A pointer to a callback function.
 * @param priv    A pointer to user-defined private data.
 */
void my_task_play_one_lingos(int audio_message_id, char *audio_url, pl_music_play_cb_func_t cb, void *priv);

/**
 * @name my_task_play_one_lfs
 * @brief This function is used to create a specific task message and enqueue it into a message queue.
 *
 * @param fname    Pointer to a character string indicating the audio file name.
 * @param priv    A pointer to user-defined private data.
 * @param cb      A pointer to a callback function.
 */
void my_task_play_one_lfs(char *fname, pl_music_play_cb_func_t cb, void *priv);

/**
 * @name disable_specific_audio_message
 * @brief Cancel the audio message with a specific audio message ID.
 *
 * @param audio_messageId Audio messsage id (LingOS).
 */
void disable_specific_audio_message(int audio_message_id);

/**
 * @name disable_all_audio_message
 * @brief Cancel all of the audio message.
 */
void disable_all_audio_message();

#endif