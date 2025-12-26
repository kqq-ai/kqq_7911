#ifndef __MUSIC_PLAY_MY__
#define __MUSIC_PLAY_MY__

#include "cpu.h"
#include "server/audio_server.h"
#include "util/ring_queue.h"
#include "lfs.h"

// #define CONFIG_STORE_VOLUME
#define VOLUME_STEP 5                  /**< Volume adjustment step size */
#define MIN_VOLUME_VALUE	5          /**< Minimum volume value */
#define MAX_VOLUME_VALUE	100        /**< Maximum volume value */
#define INIT_VOLUME_VALUE   30         /**< Initial volume value */

#define DEC_BUF_LEN      2 * 1024     /**< Decoder buffer length (4KB) */
#define STREAM_BUF_LEN   200 * 1024    /**< Stream buffer length (200KB) */

/**
 * @brief Forward declaration of music player structure
 */
typedef struct _music_player music_player;
extern music_player;

struct music_dec_ops {
    int (*dec_file)(music_player *player, void *file);
    int (*dec_stop)(music_player *player);
    // int (*dec_volume)(music_player *player, int);
    // int (*dec_progress)(music_player *player, int);
    int (*dec_status)(music_player *player);
};

/**
 * @brief Callback type for music playback completion
 * @param priv Private data passed when callback was registered
 */
typedef void (*app_music_play_end_cb_t)(void *priv);

typedef struct _music_player {
    u8 wait_download_suspend : 1;       /**< Flag: suspend while waiting for download */
    int download_ready;                 /**< Download ready status flag */
    void *net_file;                     /**< Pointer to network file resource */
    u16 wait_download;                  /**< Download wait counter */
    const struct music_dec_ops *dec_ops; /**< Decoder operations */
    const char *url;                    /**< Current playback URL */

    // u8 play_voice_prompt : 1;
    char volume;                        /**< Current volume level (0-100) */
    void *cmp_file;                     /**< Pointer to compressed file */
    int play_time;                      /**< Current playback position (ms) */
    int total_time;                     /**< Total track duration (ms) */
 
    FILE *file;                         /**< File pointer */
    lfs_file_t *lfs_file;               /**< LittleFS file pointer */

    struct server *dec_server;          /**< Audio decoder server instance */

    u8 dec_with_cb;                     /**< Flag: decode with callback */
    app_music_play_end_cb_t dec_end_cb; /**< Playback completion callback */
    void *dec_end_cb_priv;              /**< Private data for completion callback */

    RingQueue queue;
    u8 queue_buffer[STREAM_BUF_LEN]__attribute__((aligned(4))); /**< Buffer for ring queue */
} music_player;

/**
 * @brief Initialize music player instance
 * @param player Pointer to music player structure to initialize
 * @return 0 on success, negative error code on failure
 */
int music_player_init(music_player *player);

/**
 * @brief Stop current music playback
 * @param player Pointer to music player instance
 * @return 0 on success, negative error code on failure
 */
int app_music_play_stop(music_player *player);

/**
 * @brief Get current playback status
 * @param player Pointer to music player instance
 * @return Status code (implementation specific)
 */
int app_music_play_get_status(music_player *player);

/**
 * @brief Adjust playback volume
 * @param player Pointer to music player instance
 * @param step Volume adjustment step (positive or negative)
 * @return New volume level, or error code if invalid
 */
int set_dec_volume_step(music_player *player, int step);

/**
 * @brief Play music from network URL
 * @param player Pointer to music player instance
 * @param url URL of music to play
 * @return 0 on success
 */
int set_dec_volume(music_player *player, int volume);

/**
 * @brief Play music from network URL
 * @param player Pointer to music player instance
 * @param url URL of music to play
 * @return 0 on success
 */
int app_music_play_net_music(music_player *player, void *url);

/**
 * @brief Play music from network URL with completion callback
 * @param player Pointer to music player instance
 * @param url URL of music to play
 * @param cb Callback to invoke when playback completes
 * @param priv Private data to pass to callback
 * @return 0 on success
 */
int app_music_play_net_music_with_callback(music_player *player, void *url, app_music_play_end_cb_t cb, void *priv);

/**
 * @brief Play music from LittleFS file with completion callback
 * @param player Pointer to music player instance
 * @param fname Filename in LittleFS
 * @param cb Callback to invoke when playback completes
 * @param priv Private data to pass to callback
 * @return 0 on success
 */
int app_music_play_lfs_music_with_callback(music_player *player, void *fname, app_music_play_end_cb_t cb, void *priv);

/**
 * @brief Play music from circuler buffer with completion callback
 * @param player Pointer to music player instance
 * @param ringbuf ringbuffer
 * @param cb Callback to invoke when playback completes
 * @param priv Private data to pass to callback
 * @return 0 on success
 */
int app_music_play_stream_music_with_callback(music_player *player, app_music_play_end_cb_t cb, void *priv);

/**
 * @brief Write data to the music player stream
 * @param player Pointer to music player instance
 * @param buf Pointer to data buffer
 * @param len Length of data to write
 * @return 0 on success
 */
int app_music_play_stream_write(music_player *player, void *buf, uint32_t len);

/**
 * @brief Play music from local filesystem(VM) with completion callback
 * @param player Pointer to music player instance
 * @param fname Local filename
 * @param cb Callback to invoke when playback completes
 * @param priv Private data to pass to callback
 * @return 0 on success, negative error code on failure
 */
int app_music_play_local_music_with_callback(music_player *player, void *fname, app_music_play_end_cb_t cb, void *priv);


#endif