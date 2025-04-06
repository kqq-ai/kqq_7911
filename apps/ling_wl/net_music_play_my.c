#include "net_music_play_my.h"

#include "app_config.h"
#include "network_download/net_download.h"
#include "server/audio_server.h"
#include "server/server_core.h"
#include "asm/eq.h"
#include "wait.h"
#include "syscfg_id.h"
#include "log.h"
#include "errno.h"
#include "netdb.h"
#include "lfs.h"

static const struct music_dec_ops local_music_dec_ops;
static const struct music_dec_ops net_music_dec_ops;
static const struct music_dec_ops lfs_music_dec_ops;
// 定义文件系统的全局句柄
extern lfs_t g_lfs;

static void led_ui_post_msg(const char *msg, ...) {
    // nothing to do
}

static int app_music_shutdown(int priv) {
    sys_power_poweroff();
    return 0;
}

static int __get_dec_status(music_player *player)
{
    union audio_req req = {0};

    if (!player->dec_server) {
        return -EFAULT;
    }

    req.dec.cmd     = AUDIO_DEC_GET_STATUS;
    server_request(player->dec_server, AUDIO_REQ_DEC, &req);

    return req.dec.status;
}

static void dec_server_event_handler(void *priv, int argc, int *argv)
{
    music_player *player = (music_player *)priv;

    switch (argv[0]) {
    case AUDIO_SERVER_EVENT_ERR:
        log_i("app_music: AUDIO_SERVER_EVENT_ERR\n");
    case AUDIO_SERVER_EVENT_END:
        log_i("app_music: AUDIO_SERVER_EVENT_END\n");
        if (player->cmp_file != (void *)argv[1]) {
            printf("not the same file, may be closed! %p %p\n", player->cmp_file, (void *)argv[1]);
        }
        /* os_time_dly(3); */
        union audio_req r = {0};
        r.dec.cmd = AUDIO_DEC_PAUSE;
        if (player->dec_server) {
            server_request(player->dec_server, AUDIO_REQ_DEC, &r);
        }
        led_ui_post_msg("dec_end");

        // if (player->play_voice_prompt) {
        //     local_music_dec_ops.dec_stop(player);
        //     player->play_voice_prompt = 0;
        // }
        
        if (player->dec_ops && AUDIO_DEC_STOP != player->dec_ops->dec_status(player)) {
            // 停止解码
            player->dec_ops->dec_stop(player);
            // 执行回调
            if (player->dec_with_cb && player->dec_end_cb != NULL) {
                player->dec_end_cb(player->dec_end_cb_priv);
                player->dec_end_cb = NULL;
            }
        }
        /* player->play_time = 0; */
        break;
    case AUDIO_SERVER_EVENT_CURR_TIME:
        log_d("play_time: %d\n", argv[1]);
        player->play_time = argv[1];

        sys_power_auto_shutdown_clear();
        /* player->dec_ops->dec_progress(argv[1]); */
        break;
    }
}

/*======================================================================================================================
 * lfs music decoder
 *======================================================================================================================*/
int my_lfs_download_read(void *file, void *buf, u32 len)
{
    lfs_file_t *f = (lfs_file_t *)file;
    int ret = lfs_file_read(&g_lfs, f, buf, len);
    log_d("[DEC] lfs file read: len = %d, ret = %d\n", len, ret);
    /* 打印 file变量 地址*/
    return ret;
}
int my_lfs_download_seek(void *file, u32 offset, int origin)
{
    lfs_file_t *f = (lfs_file_t *)file;
    int ret = lfs_file_seek(&g_lfs, f, offset, origin);
    /* 打印 file变量 地址*/
    log_d("[DEC] lfs file seek: offset = %d, orig = %d, ret = %d\n", offset, origin, ret);
    return ret;
}
int my_lfs_download_get_file_len(void *file)
{
    lfs_file_t *f = (lfs_file_t *)file;
    int ret = lfs_file_size(&g_lfs, f);
    /* 打印 file变量 地址*/
    log_d("[DEC] lfs file size: %d\n", ret);
    return ret;
}

static const struct audio_vfs_ops lfs_audio_dec_vfs_ops = {
    .fread = my_lfs_download_read, // 使用文件系统的读取函数
    .fseek = my_lfs_download_seek, // 使用文件系统的定位函数
    .flen  = my_lfs_download_get_file_len,  // 使用文件系统的获取文件长度函数
};

static int lfs_music_dec_file(music_player *player, const char *file_path)
{
    int err;
    union audio_req req = {0};

    log_i("app_lfs_music_dec_lfs_file\n");

    if (player->dec_ops && AUDIO_DEC_STOP != player->dec_ops->dec_status(player)) {
        // 停止解码
        player->dec_ops->dec_stop(player);
        // 执行回调
        if (player->dec_with_cb && player->dec_end_cb != NULL) {
            player->dec_end_cb(player->dec_end_cb_priv);
            player->dec_end_cb = NULL;
        }
    }

    lfs_file_t *lfs_file = (lfs_file_t *)malloc(sizeof(lfs_file_t));
    // 打开文件系统中的MP3文件
    err = lfs_file_open(&g_lfs, lfs_file, file_path, LFS_O_RDONLY);
    if (err)
    {
        log_e("Failed to open file: %s, error: %d\n", file_path, err);
        return err;
    }
    /* 打印 file变量 地址*/
    log_d("================================file address: %p\n", lfs_file);
    
    req.dec.dec_type = "mp3";
    req.dec.cmd = AUDIO_DEC_OPEN;
    req.dec.volume = player->volume;
    req.dec.output_buf = NULL;
    req.dec.output_buf_len = DEC_BUF_LEN;
    req.dec.file = (void *)lfs_file; // 传递文件句柄
    req.dec.channel = 0;
    req.dec.sample_rate = 0;
    req.dec.priority = 1;
    req.dec.vfs_ops = &lfs_audio_dec_vfs_ops; // 使用文件系统的vfs_ops
    req.dec.sample_source = "dac";

#if TCFG_EQ_ENABLE
#if defined EQ_CORE_V1
    req.dec.attr |= AUDIO_ATTR_EQ_EN;
#if TCFG_LIMITER_ENABLE
    req.dec.attr |= AUDIO_ATTR_EQ32BIT_EN;
#endif
#if TCFG_DRC_ENABLE
    req.dec.attr |= AUDIO_ATTR_DRC_EN;
#endif
#else
    struct eq_s_attr eq_attr = {0};
    extern void set_eq_req_attr_parm(struct eq_s_attr * eq_attr);
    set_eq_req_attr_parm(&eq_attr);
    if (!player->play_voice_prompt)
    {
        req.dec.eq_attr = &eq_attr;
        req.dec.eq_hdl = player->eq_hdl;
    }
#endif
#endif

    err = server_request(player->dec_server, AUDIO_REQ_DEC, &req);
    if (err)
    {
        log_e("Failed to start playback: %d\n", err);
        lfs_file_close(&g_lfs, lfs_file);
        return err;
    }

    player->play_time = req.dec.play_time;
    player->total_time = req.dec.total_time;

    req.dec.cmd = AUDIO_DEC_START;

    err = server_request(player->dec_server, AUDIO_REQ_DEC, &req);
    if (err)
    {
        log_e("Failed to start playback: %d\n", err);
        lfs_file_close(&g_lfs, lfs_file);
        return err;
    }
    player->lfs_file = lfs_file;
    player->cmp_file = lfs_file;

    // if (!player->play_voice_prompt)
    // {
    //     player->dec_ops = &lfs_music_dec_ops;
    // }
    player->dec_ops = &lfs_music_dec_ops;

    led_ui_post_msg("dec_start");
    puts("play_music_file: suss\n");
    return 0;
}

static int lfs_music_dec_stop(music_player *player)
{
    int err = 0;
    union audio_req req;

    int resume_play = 0;

    if (!player->lfs_file) {
        return 0;
    }

    log_i("lfs_music_dec_stop\n");

    if (player->dec_server)
    {
        req.dec.cmd = AUDIO_DEC_STOP;
        server_request(player->dec_server, AUDIO_REQ_DEC, &req);
    }

    int argv[2];
    argv[0] = AUDIO_SERVER_EVENT_END;
    argv[1] = (int)player->lfs_file;
    server_event_handler_del(player->dec_server, 2, argv);

    /* 关闭文件 */
    printf("lfs_music_dec_stop: close file\n");
    lfs_file_close(&g_lfs, player->lfs_file);
    printf("lfs_music_dec_stop: suss\n");

    player->lfs_file = NULL;

    return 0;
}

static const struct music_dec_ops lfs_music_dec_ops = {
    .dec_file = lfs_music_dec_file,
    // .dec_volume     = lfs_music_dec_volume,
    // .dec_progress   = lfs_musci_dec_progress,
    .dec_stop = lfs_music_dec_stop,
    .dec_status = __get_dec_status,
};

/*======================================================================================================================
 * local music decoder
 *======================================================================================================================*/
static int local_music_dec_file(music_player *player, void *fname)
{
    int err;
    union audio_req req = {0};

    log_i("app_music_dec_local_file\n");

    if (player->dec_ops && AUDIO_DEC_STOP != player->dec_ops->dec_status(player)) {
        // 停止解码
        player->dec_ops->dec_stop(player);
        // 执行回调
        if (player->dec_with_cb && player->dec_end_cb != NULL) {
            player->dec_end_cb(player->dec_end_cb_priv);
            player->dec_end_cb = NULL;
        }
    }

    FILE *file = fopen(fname, "r");
    if (!file) {
        return -1;
    }

    req.dec.cmd             = AUDIO_DEC_OPEN;
    req.dec.volume          = player->volume;
    req.dec.output_buf      = NULL;
    req.dec.output_buf_len  = DEC_BUF_LEN;
    req.dec.file            = (FILE *)file;
    req.dec.channel         = 0;
    req.dec.sample_rate     = 0;
    req.dec.priority        = 1;
    req.dec.vfs_ops         = NULL;
    req.dec.sample_source   = "dac";

#if TCFG_EQ_ENABLE
#if defined EQ_CORE_V1
    req.dec.attr |= AUDIO_ATTR_EQ_EN;
#if TCFG_LIMITER_ENABLE
    req.dec.attr |= AUDIO_ATTR_EQ32BIT_EN;
#endif
#if TCFG_DRC_ENABLE
    req.dec.attr |= AUDIO_ATTR_DRC_EN;
#endif
#else
    struct eq_s_attr eq_attr = {0};
    extern void set_eq_req_attr_parm(struct eq_s_attr * eq_attr);
    set_eq_req_attr_parm(&eq_attr);
    if (!player->play_voice_prompt) {
        req.dec.eq_attr = &eq_attr;
        req.dec.eq_hdl = player->eq_hdl;
    }
#endif
#endif

    err = server_request(player->dec_server, AUDIO_REQ_DEC, &req);
    if (err) {
        log_e("audio_dec_open: err = %d\n", err);
        fclose((FILE *)file);
        return err;
    }

    player->play_time = req.dec.play_time;
    player->total_time = req.dec.total_time;

    req.dec.cmd = AUDIO_DEC_START;

    err = server_request(player->dec_server, AUDIO_REQ_DEC, &req);
    if (err) {
        log_e("audio_dec_start: err = %d\n", err);
        fclose((FILE *)file);
        return err;
    }
    player->file = (FILE *)file;
    player->cmp_file = (void *)file;

    // if (!player->play_voice_prompt) {
    //     player->dec_ops = &local_music_dec_ops;
    // }
    player->dec_ops = &local_music_dec_ops;

    led_ui_post_msg("dec_start");
    puts("play_music_file: suss\n");

    return 0;
}

static int local_music_dec_stop(music_player *player)
{
    int err = 0;
    union audio_req req;

    int resume_play = 0;

    if (!player->file) {
        return 0;
    }

    log_i("local_music_dec_stop\n");

    if (player->dec_server) {
        req.dec.cmd = AUDIO_DEC_STOP;
        server_request(player->dec_server, AUDIO_REQ_DEC, &req);
    }

    int argv[2];
    argv[0] = AUDIO_SERVER_EVENT_END;
    argv[1] = (int)player->file;
    server_event_handler_del(player->dec_server, 2, argv);

    fclose((FILE *)player->file);
    player->file = NULL;

    return 0;
}

static const struct music_dec_ops local_music_dec_ops = {
    .dec_file       = local_music_dec_file,
    // .dec_volume     = local_music_dec_volume,
    // .dec_progress   = local_musci_dec_progress,
    .dec_stop       = local_music_dec_stop,
    .dec_status     = __get_dec_status,
};

/*======================================================================================================================
 * net music decoder
 *======================================================================================================================*/
int my_net_download_read(void *priv, void *buf, u32 len) 
{
    int ret = net_download_read(priv, buf, len);
    log_d("[DEC] net file read: len = %d, ret = %d\n", len, ret);
    return ret;
}
int my_net_download_seek(void *priv, u32 offset, int orig)
{
    int ret = net_download_seek(priv, offset, orig);
    log_d("[DEC] net file seek: offset = %d, orig = %d, ret = %d\n", offset, orig, ret);

    int tmp_size = net_download_get_tmp_data_size(priv);
    log_d("[DEC] net file tmp data size: %d\n", tmp_size);
    return ret;
}
int my_net_download_get_file_len(void *priv)
{
    int ret = net_download_get_file_len(priv);
    log_d("[DEC] net file size: %d\n", ret);
    return ret;
}

static const struct audio_vfs_ops net_audio_dec_vfs_ops = {
    .fread = my_net_download_read,
    .fseek = my_net_download_seek,
    .flen  = my_net_download_get_file_len,
};

static int net_music_dec_stop(music_player *player)
{
    int err;
    union audio_req r = {0};

    log_i("net_music_dec_stop\n");

    if (!player->net_file) {
        return 0;
    }

    net_download_buf_inactive(player->net_file);

    if (player->wait_download) {
        /*
         * 歌曲还未播放，删除wait
         */
        /* sys_timer_del(player->wait_download); */
        wait_completion_del(player->wait_download);
        player->wait_download = 0;
        player->wait_download_suspend = 1;
    } else {

        if (player->dec_server) {
            r.dec.cmd = AUDIO_DEC_STOP;
            server_request(player->dec_server, AUDIO_REQ_DEC, &r);
        }

        int argv[2];
        argv[0] = AUDIO_SERVER_EVENT_END;
        argv[1] = (int)player->net_file;
        server_event_handler_del(player->dec_server, 2, argv);
    }

    canceladdrinfo();
    net_download_close(player->net_file);
    player->net_file = NULL;

    return 0;
}

static int __net_download_ready(void *priv)
{
    music_player *player = (music_player *)priv;
    player->download_ready = net_download_check_ready(player->net_file);
    if (player->download_ready) {
        return 1;
    }
    return 0;
}

static int __net_music_dec_file(void *priv)
{
    int err;
    union audio_req req = {0};

    music_player *player = (music_player *)priv;

    player->wait_download = 0;
    player->wait_download_suspend = 0;

    if (player->download_ready < 0) {
        /* 网络下载失败 */
        goto __err;
    }
    // if (player->play_voice_prompt) {
    //     set_dec_end_handler(player, player->file, __net_music_dec_file, 0, -1);
    //     return 0;
    // }

    req.dec.dec_type = net_download_get_media_type(player->net_file);
    if (req.dec.dec_type == NULL) {
        goto __err;
    }
    printf("urc_file_type: %s\n", req.dec.dec_type);

    net_download_set_read_timeout(player->net_file, 1000);

    log_i("volume: %d\n", player->volume)

    req.dec.cmd             = AUDIO_DEC_OPEN;
    req.dec.volume          = player->volume;
    req.dec.output_buf      = NULL;
    req.dec.output_buf_len  = DEC_BUF_LEN;
    req.dec.file            = (FILE *)player->net_file;
    req.dec.channel         = 0;
    req.dec.sample_rate     = 0;
    req.dec.priority        = 1;
    req.dec.vfs_ops         = &net_audio_dec_vfs_ops;
    req.dec.sample_source   = "dac";

#if TCFG_EQ_ENABLE
#if defined EQ_CORE_V1
    req.dec.attr |= AUDIO_ATTR_EQ_EN;
#if TCFG_LIMITER_ENABLE
    req.dec.attr |= AUDIO_ATTR_EQ32BIT_EN;
#endif
#if TCFG_DRC_ENABLE
    req.dec.attr |= AUDIO_ATTR_DRC_EN;
#endif
#else
    struct eq_s_attr eq_attr = {0};
    extern void set_eq_req_attr_parm(struct eq_s_attr * eq_attr);
    set_eq_req_attr_parm(&eq_attr);
    req.dec.eq_attr = &eq_attr;
    req.dec.eq_hdl = player->eq_hdl;
#endif
#endif

    err = server_request(player->dec_server, AUDIO_REQ_DEC, &req);
    if (err) {
        goto __err;
    }

    player->cmp_file = player->net_file;
    player->play_time = req.dec.play_time;
    player->total_time = req.dec.total_time;

    net_download_set_read_timeout(player->net_file, 0);

    req.dec.cmd = AUDIO_DEC_START;

    server_request(player->dec_server, AUDIO_REQ_DEC, &req);

    net_download_set_pp(player->net_file, 0);

    net_download_set_tmp_data_threshold(player->net_file, 80 * 1024);

    return 0;

__err:
    puts("play_net_music_faild\n");

    net_download_buf_inactive(player->net_file);

    if (player->dec_server) {
        req.dec.cmd = AUDIO_DEC_STOP;
        server_request(player->dec_server, AUDIO_REQ_DEC, &req);
    }
    // 执行回调
    if (player->dec_with_cb && player->dec_end_cb != NULL) {
        player->dec_end_cb(player->dec_end_cb_priv);
        player->dec_end_cb = NULL;
    }

    net_download_close(player->net_file);
    player->net_file = NULL;

    return -EFAULT;
}

static int __set_dec_volume(music_player *player, int step)
{
    union audio_req req = {0};

    int volume = player->volume + step;
    if (volume < MIN_VOLUME_VALUE) {
        volume = MIN_VOLUME_VALUE;
    } else if (volume > MAX_VOLUME_VALUE) {
// TODO: play mix file
// #ifdef CONFIG_VOICE_PROMPT_FILE_PATH
//         app_music_play_mix_file(CONFIG_VOICE_PROMPT_FILE_PATH"VolumeFull.mp3");
// #endif
        volume = MAX_VOLUME_VALUE;
    }
    if (volume == player->volume) {
        return -EINVAL;
    }
    player->volume = volume;

#ifdef CONFIG_UI_ENABLE
    os_taskq_post(POST_TASK_NAME, 2, UI_MSG_SET_VOLUME, volume);
#endif

    log_d("set_dec_volume: %d\n", volume);

// TODO: play mix file
// #ifdef CONFIG_VOICE_PROMPT_FILE_PATH
//     if (volume == MAX_VOLUME_VALUE) {
//         app_music_play_mix_file(CONFIG_VOICE_PROMPT_FILE_PATH"VolumeFull.mp3");
//     } else {
//         app_music_play_mix_file(CONFIG_VOICE_PROMPT_FILE_PATH"Volume.mp3");
//     }
// #endif

    if (!player->dec_server) {
        return -EFAULT;
    }

    req.dec.cmd     = AUDIO_DEC_SET_VOLUME;
    req.dec.volume  = volume;
    server_request(player->dec_server, AUDIO_REQ_DEC, &req);

    return 0;
}

static int net_music_dec_file(music_player *player, void *_url)
{
    int err;
    struct net_download_parm parm;

    if (player->dec_ops && AUDIO_DEC_STOP != player->dec_ops->dec_status(player)) {
        // 停止解码
        player->dec_ops->dec_stop(player);
        // 执行回调
        if (player->dec_with_cb && player->dec_end_cb != NULL) {
            player->dec_end_cb(player->dec_end_cb_priv);
            player->dec_end_cb = NULL;
        }
    }

    puts("net_download_open\n");

    if (!_url) {
        return -1;
    }

    memset(&parm, 0, sizeof(struct net_download_parm));
    parm.url                = (const char *)_url;
    parm.prio               = 0;
    parm.cbuf_size          = 200 * 1024;
    parm.timeout_millsec    = 10000;
    parm.seek_high_range    = 0;
    parm.seek_low_range     = 0;

    parm.seek_threshold     = 1024 * 200;	//用户可适当调整
    /* parm.no_wait_close      = 1; */
    /* parm.start_play_threshold = 1024 * 4; */

    err = net_download_open(&player->net_file, &parm);
    if (err) {
        log_e("net_download_open: err = %d\n", err);
        return err;
    }
    player->url = (const char *)_url;

    /*
     * 异步等待网络下载ready，防止网络阻塞导致app卡住
     */
    player->wait_download_suspend = 0;
    player->wait_download = wait_completion(__net_download_ready,
                                            (int (*)(void *))__net_music_dec_file, (void *)player, player);

    player->dec_ops = &net_music_dec_ops;

    return 0;
}

static const struct music_dec_ops net_music_dec_ops = {
    .dec_file       = net_music_dec_file,
    // .dec_volume     = net_music_dec_volume,
    // .dec_progress   = net_music_dec_progress,
    .dec_stop       = net_music_dec_stop,
    .dec_status     = __get_dec_status,
};

/*======================================================================================================================
 * 
 *======================================================================================================================*/
int app_music_play_net_music(music_player *player, void *url)
{
    net_music_dec_file(player, url);
    player->dec_end_cb = NULL;
    player->dec_end_cb_priv = NULL;
    player->dec_with_cb = 0;
    return 0;
}
int app_music_play_net_music_with_callback(music_player *player, void *url, app_music_play_end_cb_t cb, void *priv)
{
    net_music_dec_file(player, url);
    player->dec_end_cb = cb;
    player->dec_end_cb_priv = priv;
    player->dec_with_cb = 1;
    return 0;
}
int app_music_play_lfs_music_with_callback(music_player *player, void *fname, app_music_play_end_cb_t cb, void *priv)
{
    lfs_music_dec_file(player, fname);
    player->dec_end_cb = cb;
    player->dec_end_cb_priv = priv;
    player->dec_with_cb = 1;
    return 0;
}
int app_music_play_local_music_with_callback(music_player *player, void *fname, app_music_play_end_cb_t cb, void *priv)
{
    local_music_dec_file(player, fname);
    player->dec_end_cb = cb;
    player->dec_end_cb_priv = priv;
    player->dec_with_cb = 1;
    return 0;
}

int app_music_play_stop(music_player *player)
{
    if (player->dec_ops && AUDIO_DEC_STOP != player->dec_ops->dec_status(player)) {
        // 停止解码
        int ret = player->dec_ops->dec_stop(player);
        // 执行回调
        if (player->dec_with_cb && player->dec_end_cb != NULL) {
            player->dec_end_cb(player->dec_end_cb_priv);
            player->dec_end_cb = NULL;
        }
        return ret;
    }
    return 0;
}

int app_music_play_get_status(music_player *player)
{
    if (player->dec_ops) {
        return player->dec_ops->dec_status(player);
    }
    return -1;
}

int set_dec_volume_step(music_player *player, int step)
{
    return __set_dec_volume(player, step);
}

int set_dec_volume(music_player *player, int volume)
{
    player->volume = 0;
    return __set_dec_volume(player, volume);
}


int music_player_init(music_player *player)
{
    memset(player, 0, sizeof(music_player));
    player->dec_ops = NULL;

#ifdef CONFIG_STORE_VOLUME
    if (syscfg_read(CFG_MUSIC_VOL, &player->volume, sizeof(player->volume)) < 0 ||
        player->volume < MIN_VOLUME_VALUE || player->volume > MAX_VOLUME_VALUE) {
        player->volume = INIT_VOLUME_VALUE;
    }
#else
    player->volume = INIT_VOLUME_VALUE;
#endif
    if (player->volume < 0 || player->volume > MAX_VOLUME_VALUE) {
        player->volume = INIT_VOLUME_VALUE;
    }

    player->dec_server = server_open("audio_server", "dec");
    if (!player->dec_server) {
        log_e("play_music open audio_server fail!\n");
        return -EPERM;
    }
    server_register_event_handler(player->dec_server, player, dec_server_event_handler);
    return 0;
}
