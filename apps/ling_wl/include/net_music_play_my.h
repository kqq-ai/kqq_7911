#ifndef __NET_MUSIC_PLAY_MY__
#define __NET_MUSIC_PLAY_MY__

#include "cpu.h"
#include "server/audio_server.h"

typedef struct _music_player music_player;
extern music_player;

struct music_dec_ops {
    int (*dec_file)(music_player *player, void *file, int, void *, int);
    // int (*dec_play_pause)(music_player *player, u8 notify);
    // int (*dec_breakpoint)(music_player *player, int);
    int (*dec_stop)(music_player *player, int);
    // int (*dec_volume)(music_player *player, int);
    // int (*dec_progress)(music_player *player, int);
    int (*dec_seek)(music_player *player, int);
    // int (*dec_status)(music_player *player, int);
};

struct net_breakpoint {
    u32 fptr;
    u16 url_len;
    char *url;
    const char *ai_name;
    struct audio_dec_breakpoint dec_bp;
};

struct local_breakpoint {
#ifdef CONFIG_RESUME_LOCAL_PLAY_FILE
    u32 sclust;
    u32 fsize;
    u8 data[BREAKPOINT_DATA_LEN];
#endif
    struct audio_dec_breakpoint local_bp;
};

typedef struct _music_player {
    u8 wait_download_suspend : 1;
    int download_ready;
    void *net_file;
    u16 wait_download;
    const struct music_dec_ops *dec_ops;
    struct net_breakpoint net_bp;
    const char *url;
    u8 breakpoint;  // wait net_music_dec_file

    u8 play_voice_prompt : 1;
    u8 key_disable : 2;
    int seek_step;
    char volume;
    void *cmp_file;
    int play_time;
    int total_time;
    FILE *local_bp_file;
    FILE *file;
    int dec_end_args[4];
    void *dec_end_file;
    void *dec_end_handler;
    struct server *dec_server;
    struct local_breakpoint lbp;
} music_player;

int music_player_init(music_player *player);

void local_music_dec_local_play(music_player *player, const char *fname);

void net_music_dec_local_play(music_player *player, const char *url);

void net_music_dec_eend_play(music_player *player, const char *url, char *adv_name, u16 adv_name_len);

int app_music_play_net_music(music_player *player, void *url);

int local_music_dec_file(music_player *player, void *file, int breakpoint, void *handler, int arg);

#endif