#ifndef __MUSIC_PLAY_MY__
#define __MUSIC_PLAY_MY__

#include "cpu.h"
#include "server/audio_server.h"
#include "lfs.h"

#define CONFIG_PLAY_PROMPT_DISABLE_KEY	1 //播放提示音时屏蔽按键
#define CONFIG_STORE_VOLUME
#define VOLUME_STEP 5
#define MIN_VOLUME_VALUE	5
#define MAX_VOLUME_VALUE	100
#define INIT_VOLUME_VALUE   30

#define DEC_BUF_LEN      12 * 1024

typedef struct _music_player music_player;
extern music_player;

struct music_dec_ops {
    int (*dec_file)(music_player *player, void *file);
    int (*dec_stop)(music_player *player);
    // int (*dec_volume)(music_player *player, int);
    // int (*dec_progress)(music_player *player, int);
    int (*dec_status)(music_player *player);
};

typedef void (*app_music_play_end_cb_t)(void *priv);

typedef struct _music_player {
    u8 wait_download_suspend : 1;
    int download_ready;
    void *net_file;
    u16 wait_download;
    const struct music_dec_ops *dec_ops;
    const char *url;

    // u8 play_voice_prompt : 1;
    char volume;
    void *cmp_file;
    int play_time;
    int total_time;

    FILE *file;
    lfs_file_t *lfs_file;
    
    struct server *dec_server;

    u8 dec_with_cb;
    app_music_play_end_cb_t dec_end_cb;
    void *dec_end_cb_priv;
} music_player;

int music_player_init(music_player *player);

int app_music_play_stop(music_player *player);
int set_dec_volume(music_player *player, int step);

int app_music_play_net_music(music_player *player, void *url);
int app_music_play_net_music_with_callback(music_player *player, void *url, app_music_play_end_cb_t cb, void *priv);
int app_music_play_lfs_music_with_callback(music_player *player, void *fname, app_music_play_end_cb_t cb, void *priv);
int app_music_play_local_music_with_callback(music_player *player, void *fname, app_music_play_end_cb_t cb, void *priv);


#endif