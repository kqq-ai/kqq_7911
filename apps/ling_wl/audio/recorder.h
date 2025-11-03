#ifndef __RECORDER_H__
#define __RECORDER_H__

#include "fs/fs.h"
#include "generic/circular_buf.h"
#include "server/server_core.h"

#define VAD_START_THRESHOLD 100    //ms
#define VAD_STOP_THRESHOLD  120    //ms

/**
 * @brief Callback for receiving recorded audio data
 * @param priv Private data passed when callback was registered
 * @param data Pointer to audio data buffer
 * @param size Size of audio data in bytes
 */
typedef void (*recorder_data_cb_t)(void* priv, void *data, u32 size);

/**
 * @brief Voice Activity Detection (VAD) callback
 * @param priv Private data passed when callback was registered
 * @param type VAD event type (implementation specific)
 */
typedef void (*recorder_vad_cb_t)(void* priv, int type);

/**
 * @brief Recorder handle structure
 */
typedef struct recorder_hdl {
    FILE *fp;                        /**< File pointer for recording output */
    struct server *enc_server;       /**< Audio encoder server instance */
    void *cache_buf;                 /**< Audio data cache buffer */
    cbuffer_t save_cbuf;             /**< Circular buffer for saving audio */
    volatile u8 init_flag;           /**< Initialization flag */
    volatile u8 running_flag;        /**< Running state (1=running, 0=paused) */
    u8 gain;                         /**< Audio gain setting */
    u8 channel;                      /**< Audio channel configuration */
    u8 direct;                       /**< Recording direction/source */
    const char *sample_source;       /**< Audio sample source identifier */
    int sample_rate;                 /**< Audio sample rate in Hz */
    recorder_data_cb_t data_cb;      /**< Audio data callback */
    void *data_cb_priv;              /**< Private data for audio callback */
    recorder_vad_cb_t vad_cb;        /**< VAD detection callback */
    void *vad_cb_priv;               /**< Private data for VAD callback */
} recorder_hdl;

/**
 * @brief Recorder error codes
 */
typedef enum {
    RECERROR_Success = 0,            /**< Operation successful */
    RECERROR_Null_Pointer = -1,      /**< Null pointer encountered */
    RECERROR_Malloc_Failed = -2,     /**< Memory allocation failed */
    RECERROR_Api_Error = -3,         /**< API/library error occurred */
} RecorderErrorType;

/**
 * @brief Initialize recorder instance
 * @param recorder Pointer to recorder handle to initialize
 * @return RECERROR_Success on success, error code otherwise
 * @note Must be called before any other recorder functions
 */
int recorder_mode_init(recorder_hdl *recorder);

/**
 * @brief Set audio data callback
 * @param recorder Pointer to recorder handle
 * @param cb Callback function to set
 * @param priv Private data to pass to callback
 * @return RECERROR_Success on success, error code otherwise
 */
int recorder_set_data_cb(recorder_hdl *recorder, recorder_data_cb_t cb, void *priv);

/**
 * @brief Set Voice Activity Detection callback
 * @param recorder Pointer to recorder handle
 * @param cb VAD callback function to set
 * @param priv Private data to pass to callback
 * @return RECERROR_Success on success, error code otherwise
 */
int recorder_set_vad_cbk(recorder_hdl *recorder, recorder_vad_cb_t cb, void *priv);

/**
 * @brief Clean up recorder resources
 * @param recorder Pointer to recorder handle
 * @return RECERROR_Success on success, error code otherwise
 * @note Should be called when recorder is no longer needed
 */
int recorder_mode_exit(recorder_hdl *recorder);

/**
 * @brief Start audio recording
 * @param recorder Pointer to recorder handle
 * @return RECERROR_Success on success, error code otherwise
 */
int recorder_start(recorder_hdl *recorder);

/**
 * @brief Stop audio recording
 * @param recorder Pointer to recorder handle
 * @return RECERROR_Success on success, error code otherwise
 */
int recorder_stop(recorder_hdl *recorder);

/**
 * @brief Get current recorder state
 * @param recorder Pointer to recorder handle
 * @return 1 if recording, 0 if paused
 */
int recorder_get_play_pause_state(recorder_hdl *recorder);

#endif