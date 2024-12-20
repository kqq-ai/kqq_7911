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

#define CONFIG_PLAY_PROMPT_DISABLE_KEY	1 //播放提示音时屏蔽按键
#define CONFIG_STORE_VOLUME
#define VOLUME_STEP 5
#define MIN_VOLUME_VALUE	5
#define MAX_VOLUME_VALUE	100
#define INIT_VOLUME_VALUE   60

#define DEC_BUF_LEN      12 * 1024

static const struct music_dec_ops local_music_dec_ops;
static const struct music_dec_ops net_music_dec_ops;

static void led_ui_post_msg(const char *msg, ...) {
    // nothing to do
}

static int app_music_shutdown(int priv) {
    sys_power_poweroff();
    return 0;
}

static void do_dec_end_handler(music_player *player, void *file)
{
    if (file == player->dec_end_file) {
        player->dec_end_file = NULL;
        if (player->dec_end_handler) {
            if (player->dec_end_args[1] == -1) {
                ((int (*)(music_player *, int))player->dec_end_handler)(player, player->dec_end_args[0]);
            } else {
                ((int (*)(music_player *, int, int))player->dec_end_handler)(player, player->dec_end_args[0],
                        player->dec_end_args[1]);
            }
        }
    }
}

static void set_dec_end_handler(music_player *player, void *file, void *handler, int arg0, int arg1)
{
    player->dec_end_file = file;
    player->dec_end_args[0] = arg0;
    player->dec_end_args[1] = arg1;
    player->dec_end_handler = handler;
}

/*======================================================================================================================
 * local music decoder
 *======================================================================================================================*/


/*
 * 选择文件并播放, mode为选择方式，如FSEL_NEXT_FILE
 */
int local_music_dec_file(music_player *player, void *file, int breakpoint, void *handler, int arg)
{
    int err;
    union audio_req req = {0};

    log_i("app_music_dec_local_file\n");

    player->dec_ops->dec_stop(player, 0);

    if (!file) {
        return -1;
    }

    set_dec_end_handler(player, file, handler, arg, -1);

    if (breakpoint) {
        req.dec.bp = &player->lbp.local_bp;
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
    if (!__this->play_voice_prompt) {
        req.dec.eq_attr = &eq_attr;
        req.dec.eq_hdl = __this->eq_hdl;
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

    if (breakpoint) {
        player->lbp.local_bp.len = 0;
    }

    if (player->seek_step && !player->play_voice_prompt) {
        player->dec_ops->dec_seek(player, player->seek_step);
        player->seek_step = 0;
    }

    req.dec.cmd = AUDIO_DEC_START;

    err = server_request(player->dec_server, AUDIO_REQ_DEC, &req);
    if (err) {
        log_e("audio_dec_start: err = %d\n", err);
        fclose((FILE *)file);
        return err;
    }
    player->file = (FILE *)file;
    player->cmp_file = (void *)file;

    if (!player->play_voice_prompt) {
        player->dec_ops = &local_music_dec_ops;
    }

    led_ui_post_msg("dec_start");
    puts("play_music_file: suss\n");

    return 0;
}

static int local_music_dec_stop(music_player *player, int save_breakpoint)
{
    int err = 0;
    union audio_req req;

    int resume_play = 0;

    if (!player->file) {
        return 0;
    }

    log_i("local_music_dec_stop\n");

    if (save_breakpoint) {  
        err = __get_dec_breakpoint(player, &player->lbp.local_bp, resume_play);
        if (!err) {
            if (player->local_bp_file && player->local_bp_file != player->file) {
                fclose(player->local_bp_file);
            }
            player->local_bp_file = player->file;
        }
    }

    if (player->dec_server) {
        req.dec.cmd = AUDIO_DEC_STOP;
        server_request(player->dec_server, AUDIO_REQ_DEC, &req);
    }

    int argv[2];
    argv[0] = AUDIO_SERVER_EVENT_END;
    argv[1] = (int)player->file;
    server_event_handler_del(player->dec_server, 2, argv);

    if (!save_breakpoint || err) {
        fclose(player->file);
        if (player->local_bp_file == player->file) {
            player->local_bp_file = NULL;
        }
    }
    player->file = NULL;

    return 0;
}

static int local_music_dec_seek(music_player *player, int seek_step)
{
    int err;
    union audio_req r = {0};

    if (0 == seek_step) {
        return 0;
    }

    if (player->total_time != 0 && player->total_time != -1) {
        if (player->play_time + seek_step <= 0 || player->play_time + seek_step >= player->total_time) {
            log_e("local music seek out of range\n");
            return -1;
        }
    }

    if (seek_step > 0) {
        r.dec.cmd = AUDIO_DEC_FF;
        r.dec.ff_fr_step = seek_step;
    } else {
        r.dec.cmd = AUDIO_DEC_FR;
        r.dec.ff_fr_step = -seek_step;
    }

    log_i("local music seek step : %d\n", seek_step);

    if (!player->dec_server) {
        return -EFAULT;
    }

    err = server_request(player->dec_server, AUDIO_REQ_DEC, &r);
    if (!err && r.dec.status == AUDIO_DEC_START) {
        led_ui_post_msg("dec_start");
    }

    return 0;
}


static const struct music_dec_ops local_music_dec_ops = {
    // .switch_dir     = local_music_dec_switch_dir,
    // .switch_file    = local_music_dec_switch_file,
    .dec_file       = local_music_dec_file,
    // .dec_breakpoint = local_music_dec_breakpoint,
    // .dec_play_pause = local_music_dec_play_pause,
    // .dec_volume     = local_music_dec_volume,
    // .dec_progress   = local_musci_dec_progress,
    .dec_stop       = local_music_dec_stop,
    .dec_seek       = local_music_dec_seek,
    // .dec_status     = __get_dec_status,
};

/*======================================================================================================================
 * net music decoder
 *======================================================================================================================*/

static const struct audio_vfs_ops net_audio_dec_vfs_ops = {
    .fread = net_download_read,
    .fseek = net_download_seek,
    .flen  = net_download_get_file_len,
};

static int net_music_dec_end(music_player *player, int save_bp)
{
    log_i("net_music_dec_end\n");

    net_music_dec_stop(player, save_bp);

    return 0;
}

static int __get_dec_breakpoint(music_player *player, struct audio_dec_breakpoint *bp, int resume_play)
{
    int err;
    union audio_req r = {0};

    bp->len = 0;
    r.dec.bp = bp;
    r.dec.cmd = AUDIO_DEC_GET_BREAKPOINT;

    if (bp->data) {
        free(bp->data);
        bp->data = NULL;
    }

    if (!player->dec_server) {
        return -EFAULT;
    }

    err = server_request(player->dec_server, AUDIO_REQ_DEC, &r);
    if (err) {
        return err;
    }

    if (resume_play && r.dec.dec_type && !strcmp(r.dec.dec_type, "m4a") && bp->len > 512) {
        bp->len -= 512;
    }

    if (!resume_play && r.dec.status == AUDIO_DEC_STOP) {
        bp->len = 0;
        free(bp->data);
        bp->data = NULL;
        return -EFAULT;
    }
    /* put_buf(bp->data, bp->len); */

    return 0;
}

static int net_music_dec_stop(music_player *player, int save_breakpoint)
{
    int err;
    union audio_req r = {0};

    log_i("net_music_dec_stop: %d\n", save_breakpoint);

    if (!player->net_file) {
        return 0;
    }

    net_download_buf_inactive(player->net_file);

    if (player->wait_download) {
        /*
         * 歌曲还未播放，删除wait
         */
        /* sys_timer_del(__this->wait_download); */
        wait_completion_del(player->wait_download);
        player->wait_download = 0;
        player->wait_download_suspend = 1;
    } else {
        if (save_breakpoint) {
            int url_len = strlen(player->url) + 1;
            if (url_len > player->net_bp.url_len) {
                if (player->net_bp.url) {
                    free(player->net_bp.url);
                }
                player->net_bp.url = malloc(url_len);
                if (!player->net_bp.url) {
                    player->net_bp.url_len = 0;
                } else {
                    player->net_bp.url_len = url_len;
                }
            }
            if (player->net_bp.url) {
                err = __get_dec_breakpoint(player, &player->net_bp.dec_bp, 0);
                if (!err) {
                    strcpy(player->net_bp.url, player->url);
                    player->net_bp.fptr = player->net_bp.dec_bp.fptr;
                    if (net_download_check_if_is_m3u8_url(player->net_file)) {
                        player->net_bp.fptr = 0;
                        free(player->net_bp.dec_bp.data);
                        player->net_bp.dec_bp.data = NULL;
                    }
                    log_d("save_breakpoint: fptr=%x\n", player->net_bp.fptr);
                } else if (1 == err) {		//解决暂停时的断点问题
                }
            }
        }

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
    player->seek_step = 0;

    return 0;
}

static int __net_download_ready(void *priv)
{
    music_player *player = (music_player *)priv;
    log_i("wait: %p\n", player);
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
    int breakpoint = player->breakpoint;

    player->wait_download = 0;
    player->wait_download_suspend = 0;

    if (player->download_ready < 0) {
        /* 网络下载失败 */
        goto __err;
    }
    if (player->play_voice_prompt) {
        set_dec_end_handler(player, player->file, __net_music_dec_file, breakpoint, -1);
        return 0;
    }

    req.dec.dec_type = net_download_get_media_type(player->net_file);
    if (req.dec.dec_type == NULL) {
        goto __err;
    }
    printf("urc_file_type: %s\n", req.dec.dec_type);

    if (breakpoint == 1) {
        // player->ai_name = player->net_bp.ai_name;
        req.dec.bp = &player->net_bp.dec_bp;
    }

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
    req.dec.eq_hdl = __this->eq_hdl;
#endif
#endif

    err = server_request(player->dec_server, AUDIO_REQ_DEC, &req);
    if (err) {
        goto __err;
    }

    player->cmp_file = player->net_file;
    player->play_time = req.dec.play_time;
    player->total_time = req.dec.total_time;

    if (breakpoint == 1) {
        player->net_bp.dec_bp.len = 0;
    }

    net_download_set_read_timeout(player->net_file, 0);

    if (player->seek_step) {
        player->dec_ops->dec_seek(player, player->seek_step);
        player->seek_step = 0;
        net_download_set_read_timeout(player->net_file, 0);
    }

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

    net_download_close(player->net_file);
    player->net_file = NULL;

    return -EFAULT;
}

static int net_music_dec_file(music_player *player, void *_url, int breakpoint, void *handler, int arg)
{
    int err;
    struct net_download_parm parm;

    if (player->dec_ops) {
        player->dec_ops->dec_stop(player, 0);
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
    parm.seek_low_range     = breakpoint ? player->net_bp.fptr : 0;
    
    parm.seek_threshold     = 1024 * 200;	//用户可适当调整
    /* parm.no_wait_close      = 1; */
    /* parm.start_play_threshold = 1024 * 4; */

    err = net_download_open(&player->net_file, &parm);
    if (err) {
        log_e("net_download_open: err = %d\n", err);
        return err;
    }
    player->url = (const char *)_url;

    set_dec_end_handler(player, player->net_file, handler, arg, -1);

    /*
     * 异步等待网络下载ready，防止网络阻塞导致app卡住
     */
    player->wait_download_suspend = 0;
    player->wait_download = wait_completion(__net_download_ready,
                                            (int (*)(void *))__net_music_dec_file, (void *)player, player);

    player->dec_ops = &net_music_dec_ops;

    return 0;
}

static int net_music_dec_seek(music_player *player, int seek_step)
{
    int err;
    union audio_req r = {0};

    if (0 == seek_step) {
        return 0;
    }

    if (player->total_time != 0 && player->total_time != -1) {
        if (player->play_time + seek_step <= 0 || player->play_time + seek_step >= player->total_time) {
            log_e("local music seek out of range\n");
            return -1;
        }
    }

    if (seek_step > 0) {
        r.dec.cmd = AUDIO_DEC_FF;
        r.dec.ff_fr_step = seek_step;
    } else {
        r.dec.cmd = AUDIO_DEC_FR;
        r.dec.ff_fr_step = -seek_step;
    }

    log_i("net music seek step : %d\n", seek_step);

    if (!player->dec_server) {
        return -EFAULT;
    }

    err = server_request(player->dec_server, AUDIO_REQ_DEC, &r);
    if (!err && r.dec.status == AUDIO_DEC_START) {
        led_ui_post_msg("dec_start");
    }

    return 0;
}

static const struct music_dec_ops net_music_dec_ops = {
    .dec_file       = net_music_dec_file,
    // .dec_breakpoint = net_music_dec_breakpoint,
    // .dec_play_pause = net_music_dec_play_pause,
    // .dec_volume     = net_music_dec_volume,
    // .dec_progress   = net_music_dec_progress,
    .dec_stop       = net_music_dec_stop,
    .dec_seek       = net_music_dec_seek,
    // .dec_status     = __get_dec_status,
};

static int __play_net_music(music_player *player, const char *url)
{
    return net_music_dec_file(player, (void *)url, 0, net_music_dec_end, 0);
}

int app_music_play_net_music(music_player *player, void *url)
{
    if (player->play_voice_prompt) {
        if (player->dec_end_handler != (void *)app_music_shutdown) {
            set_dec_end_handler(player, player->dec_end_file, __play_net_music, (int)url, -1);
        }
        return 0;
    }
    if (player->net_bp.url && player->net_bp.dec_bp.len > 0 && !strcmp((const char *)url, player->net_bp.url)) {
        return net_music_dec_file(player, url, 1, net_music_dec_end, 0);
    }

    return net_music_dec_file(player, url, 0, net_music_dec_end, 0);
}


/*
 * ****************************提示音*************************************
 */
static void dec_server_event_handler(void *priv, int argc, int *argv)
{
    music_player *player = (music_player *)priv;

    switch (argv[0]) {
    case AUDIO_SERVER_EVENT_ERR:
        log_i("app_music: AUDIO_SERVER_EVENT_ERR\n");
    case AUDIO_SERVER_EVENT_END:
        log_i("app_music: AUDIO_SERVER_EVENT_END\n");
        if (player->cmp_file != (void *)argv[1]) {
            printf("not the same file, may be closed!\n");
            break;
        }
        /* os_time_dly(3); */
        union audio_req r = {0};
        r.dec.cmd = AUDIO_DEC_PAUSE;
        if (player->dec_server) {
            server_request(player->dec_server, AUDIO_REQ_DEC, &r);
        }
        led_ui_post_msg("dec_end");
        if (!player->key_disable) {
            key_event_enable();
        }
        if (player->play_voice_prompt) {
            local_music_dec_ops.dec_stop(player, 0);
            player->play_voice_prompt = 0;
        }
        do_dec_end_handler(player, (void *)argv[1]);
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

int music_player_init(music_player *player)
{
    memset(player, 0, sizeof(music_player));
    player->net_bp.url = malloc(512);
    player->net_bp.url_len = 512;
    player->dec_ops = &net_music_dec_ops;

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
