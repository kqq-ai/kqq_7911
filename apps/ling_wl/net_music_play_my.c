#include "net_music_play_my.h"

#include "app_config.h"
#include "asm/eq.h"
#include "circular_buf.h"
#include "errno.h"
#include "lfs.h"
#include "log.h"
#include "netdb.h"
#include "network_download/net_download.h"
#include "server/audio_server.h"
#include "server/server_core.h"
#include "syscfg_id.h"
#include "system/timer.h"
#include "wait.h"
#include <os/pthread.h>
#include <stdbool.h>
#include <string.h>

static const struct music_dec_ops local_music_dec_ops;
static const struct music_dec_ops net_music_dec_ops;
static const struct music_dec_ops lfs_music_dec_ops;
static const struct music_dec_ops cbuf_music_dec_ops;
static const struct music_dec_ops local_stream_dec_ops;
// 定义文件系统的全局句柄
extern lfs_t g_lfs;

/* Serialize audio decoder open/close to avoid concurrent allocations in
 * decoder/audio server */
static OS_MUTEX audio_play_mutex;

static volatile u8 g_t_key_first_pcm_pending;

// 独立的常驻提示音播放器实例
static music_player g_prompt_player;
static u8 g_prompt_queue_buffer[64 * 1024]
    __attribute__((aligned(4))); // 64KB 队列，足够容纳大多数提示音
static volatile int g_prompt_playing = 0;
static volatile int g_prompt_eof = 0;

#define PROMPT_QUEUE_SIZE (64 * 1024)

// Local prompt async stream context
typedef struct {
  RingQueue *queue;
  volatile int *done;
  volatile int *abort;
} local_stream_ctx_t;

typedef struct {
  music_player *player;
  char *path;
} local_stream_task_t;

static local_stream_ctx_t g_local_stream_ctx;
static volatile int g_local_stream_abort = 0;
static volatile int g_local_stream_done = 0;
static volatile int g_local_stream_thread_running = 0;
static pthread_t g_local_stream_thread;

#define LOCAL_STREAM_PREFILL_SIZE (0x3000)
#define LOCAL_STREAM_CHUNK_SIZE 1024

static void *local_stream_worker(void *arg);
static int local_stream_dec_start(music_player *player,
                                  local_stream_ctx_t *ctx);

/*======================================================================================================================
 * Persistent Prompt Decoder VFS Operations
 *======================================================================================================================*/
static int prompt_vfs_read(void *file, void *buf, u32 len) {
  RingQueue *queue = (RingQueue *)file;

  // 只有在关机时才返回0(EOF)，其他时候永远等待新数据
  if (g_prompt_eof) {
    log_i("[PROMPT_VFS] Shutdown EOF, returning 0\n");
    return 0;
  }

  int ret = ring_queue_try_read_timeout(queue, buf, len, 1000);
  // [PROOF] 关键取证：
  // 1. 如果此日志在第二个文件播放时不出现 ->
  // 证明解码器内部状态未重置（它认为流已结束）
  // 2. 如果出现但无声 -> 证明ID3头被错误当做音频数据解析导致Sync错误
  log_i("[PROMPT_VFS] read_req=%d, ret=%d", len, ret);

  if (ret == RINGQUEUE_Is_Empty) {
    return -1; // 无数据，等待重试（解码器不会停止）
  }
  return ret;
}

static int prompt_vfs_seek(void *file, u32 offset, int origin) {
  return -1; // 不支持 seek
}

static int prompt_vfs_get_file_len(void *file) {
  return -1; // 流式数据，长度未知
}

static const struct audio_vfs_ops prompt_audio_dec_vfs_ops = {
    .fread = prompt_vfs_read,
    .fseek = prompt_vfs_seek,
    .flen = prompt_vfs_get_file_len,
};

static void first_play_timer_cb(void *priv);

static void mark_first_play_start(music_player *player) {
  if (!player) {
    return;
  }
  player->first_play_pending = 1;
  player->first_play_start_ms = timer_get_ms();
  player->first_play_ms = 0;
  if (!player->first_play_timer) {
    player->first_play_timer = sys_timer_add(player, first_play_timer_cb, 10);
  }
}

static void clear_first_play_pending(music_player *player) {
  if (!player) {
    return;
  }
  player->first_play_pending = 0;
  if (player->first_play_timer) {
    sys_timer_del(player->first_play_timer);
    player->first_play_timer = 0;
  }
}

static void first_play_timer_cb(void *priv) {
  music_player *player = (music_player *)priv;
  union audio_req req = {0};
  u32 play_pos = 0;
  int err;

  if (!player || !player->first_play_pending || !player->dec_server) {
    clear_first_play_pending(player);
    return;
  }

  req.ioctl.cmd = AUDIO_IOCTRL_CMD_GET_PLAYPOS;
  req.ioctl.priv = &play_pos;
  err = server_request(player->dec_server, AUDIO_REQ_IOCTL, &req);
  if (err) {
    log_w("first_play_timer: get playpos err=%d\n", err);
    clear_first_play_pending(player);
    return;
  }

  if (play_pos > 0) {
    player->first_play_pending = 0;
    player->first_play_ms = timer_get_ms();
    log_i("🔥🔥🔥🔥🔥🔥🔥🔥first_play_time: start=%u ms, first=%u ms, delta=%u "
          "ms",
          player->first_play_start_ms, player->first_play_ms,
          player->first_play_ms - player->first_play_start_ms);
    if (player->first_play_timer) {
      sys_timer_del(player->first_play_timer);
      player->first_play_timer = 0;
    }
  }
}

static const char *audio_basename(const char *path) {
  const char *base = path;
  if (!path) {
    return "";
  }
  for (const char *p = path; *p; ++p) {
    if (*p == '/' || *p == '\\') {
      base = p + 1;
    }
  }
  return base;
}

// 同步将文件内容读入队列
static int load_file_to_queue(const char *path, RingQueue *queue) {
  FILE *file = fopen(path, "r");
  if (!file) {
    log_e("load_file_to_queue open failed: %s", path);
    return -1;
  }

  u8 buf[1024];
  int read_len;
  int total_len = 0;

  while (1) {
    read_len = fread(buf, 1, sizeof(buf), file);
    if (read_len <= 0) {
      break;
    }
    int written = 0;
    int retry = 0;
    while (written < read_len) {
      int ret = ring_queue_try_write(queue, (const char *)buf + written,
                                     read_len - written);
      if (ret > 0) {
        written += ret;
        total_len += ret;
        retry = 0;
      } else if (ret == RINGQUEUE_Is_Full) {
        // 队列满极其罕见（因为我们通常会清空），但为了健壮性稍微等待
        if (++retry > 5)
          break;
        msleep(1);
      } else {
        break;
      }
    }
  }

  fclose(file);
  log_i("[PROMPT] loaded %d bytes from %s", total_len, path);
  return total_len;
}

static void led_ui_post_msg(const char *msg, ...) {
  // nothing to do
}

static int app_music_shutdown(int priv) {
  // 关机前停止常开的提示音解码器
  if (g_prompt_player.dec_server) {
    log_i("[PROMPT]Shutting down always-on decoder");
    g_prompt_eof = 1; // 触发VFS返回EOF
    msleep(50);       // 等待解码器读取到EOF

    union audio_req req = {0};
    req.dec.cmd = AUDIO_DEC_STOP;
    server_request(g_prompt_player.dec_server, AUDIO_REQ_DEC, &req);
    server_close(g_prompt_player.dec_server);
    g_prompt_player.dec_server = NULL;
  }
  sys_power_poweroff();
  return 0;
}

static int __get_dec_status(music_player *player) {
  union audio_req req = {0};

  if (!player->dec_server) {
    return -EFAULT;
  }

  req.dec.cmd = AUDIO_DEC_GET_STATUS;
  server_request(player->dec_server, AUDIO_REQ_DEC, &req);

  return req.dec.status;
}

static void dec_server_event_handler(void *priv, int argc, int *argv) {
  music_player *player = (music_player *)priv;

  os_mutex_pend(&audio_play_mutex, 0);
  switch (argv[0]) {
  case AUDIO_SERVER_EVENT_ERR:
    log_i("[AUDIO_DECODER]AUDIO_SERVER_EVENT_ERR\n");
  case AUDIO_SERVER_EVENT_END:
    log_i("app_server, music: AUDIO_SERVER_EVENT_END\n");
    clear_first_play_pending(player);

    // 检查是否是提示音播放完成
    if ((void *)argv[1] == (void *)&g_prompt_player.queue) {
      log_i("[PROMPT]Prompt playback ended\n");

      // 停止解码器
      union audio_req r = {0};
      r.dec.cmd = AUDIO_DEC_STOP;
      if (g_prompt_player.dec_server) {
        server_request(g_prompt_player.dec_server, AUDIO_REQ_DEC, &r);
      }

      // 清空队列和状态
      ring_queue_clear(&g_prompt_player.queue);
      g_prompt_playing = 0;
      g_prompt_eof = 0;

      // 执行回调
      if (g_prompt_player.dec_with_cb && g_prompt_player.dec_end_cb != NULL) {
        g_prompt_player.dec_end_cb(g_prompt_player.dec_end_cb_priv);
        g_prompt_player.dec_end_cb = NULL;
        g_prompt_player.dec_with_cb = 0;
      }

      os_mutex_post(&audio_play_mutex);
      return;
    }

    if (player->cmp_file != (void *)argv[1]) {
      log_e("not the same file, may be closed! %p %p\n", player->cmp_file,
            (void *)argv[1]);
    }
    app_music_play_stop(player);
    /* player->play_time = 0; */
    break;
  case AUDIO_SERVER_EVENT_CURR_TIME:
    log_d("play_time: %d\n", argv[1]);
    player->play_time = argv[1];
    if (player->first_play_pending && argv[1] > 0) {
      player->first_play_pending = 0;
      player->first_play_ms = timer_get_ms();
      log_i("first_play_time: start=%u ms, first=%u ms, delta=%u ms",
            player->first_play_start_ms, player->first_play_ms,
            player->first_play_ms - player->first_play_start_ms);
      if (player->first_play_timer) {
        sys_timer_del(player->first_play_timer);
        player->first_play_timer = 0;
      }
    }

    sys_power_auto_shutdown_clear();
    /* player->dec_ops->dec_progress(argv[1]); */
    break;
  }
  os_mutex_post(&audio_play_mutex);
}

/*======================================================================================================================
 * lfs music decoder
 *======================================================================================================================*/
int my_lfs_download_read(void *file, void *buf, u32 len) {
  lfs_file_t *f = (lfs_file_t *)file;
  int ret = lfs_file_read(&g_lfs, f, buf, len);
  log_d("[DEC] lfs file read: len = %d, ret = %d\n", len, ret);
  /* 打印 file变量 地址*/
  return ret;
}
int my_lfs_download_seek(void *file, u32 offset, int origin) {
  lfs_file_t *f = (lfs_file_t *)file;
  int ret = lfs_file_seek(&g_lfs, f, offset, origin);
  /* 打印 file变量 地址*/
  log_d("[DEC] lfs file seek: offset = %d, orig = %d, ret = %d\n", offset,
        origin, ret);
  return ret;
}
int my_lfs_download_get_file_len(void *file) {
  lfs_file_t *f = (lfs_file_t *)file;
  int ret = lfs_file_size(&g_lfs, f);
  /* 打印 file变量 地址*/
  log_d("[DEC] lfs file size: %d\n", ret);
  return ret;
}

static const struct audio_vfs_ops lfs_audio_dec_vfs_ops = {
    .fread = my_lfs_download_read,        // 使用文件系统的读取函数
    .fseek = my_lfs_download_seek,        // 使用文件系统的定位函数
    .flen = my_lfs_download_get_file_len, // 使用文件系统的获取文件长度函数
};

static int lfs_music_dec_file(music_player *player, const char *file_path) {
  int err;
  union audio_req req = {0};

  log_i("[AUDIO_DECODER]app_lfs_music_dec_lfs_file\n");

  os_mutex_pend(&audio_play_mutex, 0);
  app_music_play_stop(player);

  lfs_file_t *lfs_file = (lfs_file_t *)malloc(sizeof(lfs_file_t));
  // 打开文件系统中的MP3文件
  err = lfs_file_open(&g_lfs, lfs_file, file_path, LFS_O_RDONLY);
  if (err) {
    log_e("Failed to open file: %s, error: %d\n", file_path, err);
    os_mutex_post(&audio_play_mutex);
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
  if (!player->play_voice_prompt) {
    req.dec.eq_attr = &eq_attr;
    req.dec.eq_hdl = player->eq_hdl;
  }
#endif
#endif

  err = server_request(player->dec_server, AUDIO_REQ_DEC, &req);
  if (err) {
    log_e("Failed to start playback: %d\n", err);
    lfs_file_close(&g_lfs, lfs_file);
    os_mutex_post(&audio_play_mutex);
    return err;
  }

  player->play_time = req.dec.play_time;
  player->total_time = req.dec.total_time;

  req.dec.cmd = AUDIO_DEC_START;

  err = server_request(player->dec_server, AUDIO_REQ_DEC, &req);
  if (err) {
    log_e("Failed to start playback: %d\n", err);
    lfs_file_close(&g_lfs, lfs_file);
    os_mutex_post(&audio_play_mutex);
    return err;
  }
  player->lfs_file = lfs_file;
  player->cmp_file = lfs_file;
  mark_first_play_start(player);

  // if (!player->play_voice_prompt)
  // {
  //     player->dec_ops = &lfs_music_dec_ops;
  // }
  player->dec_ops = &lfs_music_dec_ops;

  led_ui_post_msg("dec_start");
  log_i("[AUDIO_DECODER]play_music_file: suss\n");
  os_mutex_post(&audio_play_mutex);
  return 0;
}

static int lfs_music_dec_stop(music_player *player) {
  int err = 0;
  union audio_req req;

  int resume_play = 0;

  if (!player->lfs_file) {
    return 0;
  }

  log_i("[AUDIO_DECODER]lfs_music_dec_stop\n");

  if (player->dec_server) {
    req.dec.cmd = AUDIO_DEC_STOP;
    server_request(player->dec_server, AUDIO_REQ_DEC, &req);
  }

  int argv[2];
  argv[0] = AUDIO_SERVER_EVENT_END;
  argv[1] = (int)player->lfs_file;
  server_event_handler_del(player->dec_server, 2, argv);

  /* 关闭文件 */
  lfs_file_close(&g_lfs, player->lfs_file);

  player->lfs_file = NULL;

  return 0;
}

static const struct music_dec_ops lfs_music_dec_ops = {
    .dec_file = lfs_music_dec_file,
    // .dec_volume                 = lfs_music_dec_volume,
    // .dec_progress         = lfs_musci_dec_progress,
    .dec_stop = lfs_music_dec_stop,
    .dec_status = __get_dec_status,
};

/*======================================================================================================================
 * stream music decoder
 *======================================================================================================================*/
int my_stream_download_read(void *file, void *buf, u32 len) {
  int ret = ring_queue_try_read_timeout(file, buf, len, 1000);

  log_d("[DEC] cbuf file read: len = %d, ret = %d\n", len, ret);

  if (RINGQUEUE_Is_Empty == ret) {
    return -1;
  }
  return ret;
}
int my_stream_download_seek(void *file, u32 offset, int origin) { return -1; }
int my_stream_download_get_file_len(void *file) { return -1; }

static const struct audio_vfs_ops cbuf_audio_dec_vfs_ops = {
    .fread = my_stream_download_read,
    .fseek = my_stream_download_seek,
    .flen = my_stream_download_get_file_len,
};

static int my_local_stream_read(void *file, void *buf, u32 len) {
  local_stream_ctx_t *ctx = (local_stream_ctx_t *)file;
  int ret = ring_queue_try_read_timeout(ctx->queue, buf, len, 1000);
  if (ret == RINGQUEUE_Is_Empty) {
    if (*ctx->done || *ctx->abort) {
      return 0; // EOF
    }
    return -1;
  }
  return ret;
}

static int my_local_stream_seek(void *file, u32 offset, int origin) {
  return -1;
}

static int my_local_stream_get_file_len(void *file) { return -1; }

static const struct audio_vfs_ops local_stream_audio_dec_vfs_ops = {
    .fread = my_local_stream_read,
    .fseek = my_local_stream_seek,
    .flen = my_local_stream_get_file_len,
};

static int __stream_download_ready(void *priv) {
  music_player *player = (music_player *)priv;
  RingQueue *queue = &player->queue;
  if (0x3000 < ring_queue_size(queue)) {
    return 1;
  }
  return 0;
}

static int __stream_music_dec_file(music_player *player) {
  int err;
  union audio_req req = {0};

  os_mutex_pend(&audio_play_mutex, 0);
  app_music_play_stop(player);

  RingQueue *queue = &player->queue;

  req.dec.dec_type = "mp3";
  req.dec.cmd = AUDIO_DEC_OPEN;
  req.dec.volume = player->volume;
  req.dec.output_buf = NULL;
  req.dec.output_buf_len = DEC_BUF_LEN;
  req.dec.file = (void *)queue; // 传递文件句柄
  req.dec.channel = 0;
  req.dec.sample_rate = 0;
  req.dec.priority = 1;
  req.dec.vfs_ops = &cbuf_audio_dec_vfs_ops; // 使用文件系统的vfs_ops
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
  if (!player->play_voice_prompt) {
    req.dec.eq_attr = &eq_attr;
    req.dec.eq_hdl = player->eq_hdl;
  }
#endif
#endif

  err = server_request(player->dec_server, AUDIO_REQ_DEC, &req);
  if (err) {
    log_e("Failed to start playback: %d\n", err);
    os_mutex_post(&audio_play_mutex);
    return err;
  }

  player->play_time = req.dec.play_time;
  player->total_time = req.dec.total_time;

  req.dec.cmd = AUDIO_DEC_START;

  err = server_request(player->dec_server, AUDIO_REQ_DEC, &req);
  if (err) {
    log_e("Failed to start playback: %d\n", err);
    os_mutex_post(&audio_play_mutex);
    return err;
  }
  player->cmp_file = queue;
  os_mutex_post(&audio_play_mutex);
  mark_first_play_start(player);
  return 0;
}

static int stream_music_dec_file(music_player *player) {
  os_mutex_pend(&audio_play_mutex, 0);
  app_music_play_stop(player);
  log_i("[AUDIO_DECODER]app_stream_music_dec_file\n");

  /*
   * 异步等待网络下载ready，防止网络阻塞导致app卡住
   */
  player->wait_download_suspend = 0;
  /*
   * NOTE:
   * wait_completion()/server 事件回调依赖“任务消息队列(taskq)”来投递执行。
   * 这里的 stream 播放经常由 pthread 线程(例如 threadpool)触发，pthread 没有
   * taskq， 会导致日志: [WAIT_COMPLETION]wait pthread ... taskq not exist
   *
   * 因此固定投递到 app_core（具备 taskq 且在主循环中接收队列消息）。
   */
  player->wait_download = wait_completion_add_to_task(
      "app_core", (int (*)(void *))__stream_download_ready,
      (int (*)(void *))__stream_music_dec_file, (void *)player, player);

  // if (!player->play_voice_prompt)
  // {
  //                 player->dec_ops = &lfs_music_dec_ops;
  // }
  player->dec_ops = &cbuf_music_dec_ops;

  led_ui_post_msg("dec_start");
  log_i("[AUDIO_DECODER]play_music_file: suss\n");
  os_mutex_post(&audio_play_mutex);
  return 0;
}

static int local_stream_dec_start(music_player *player,
                                  local_stream_ctx_t *ctx) {
  int err;
  union audio_req req = {0};

  os_mutex_pend(&audio_play_mutex, 0);
  app_music_play_stop(player);

  req.dec.dec_type = "mp3";
  req.dec.cmd = AUDIO_DEC_OPEN;
  req.dec.volume = player->volume;
  req.dec.output_buf = NULL;
  req.dec.output_buf_len = DEC_BUF_LEN;
  req.dec.file = (void *)ctx;
  req.dec.channel = 0;
  req.dec.sample_rate = 0;
  req.dec.priority = 1;
  req.dec.vfs_ops = &local_stream_audio_dec_vfs_ops;
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
  if (!player->play_voice_prompt) {
    req.dec.eq_attr = &eq_attr;
    req.dec.eq_hdl = player->eq_hdl;
  }
#endif
#endif

  err = server_request(player->dec_server, AUDIO_REQ_DEC, &req);
  if (err) {
    log_e("local_stream_dec open failed: %d\n", err);
    os_mutex_post(&audio_play_mutex);
    return err;
  }

  player->play_time = req.dec.play_time;
  player->total_time = req.dec.total_time;

  req.dec.cmd = AUDIO_DEC_START;
  err = server_request(player->dec_server, AUDIO_REQ_DEC, &req);
  if (err) {
    log_e("local_stream_dec start failed: %d\n", err);
    os_mutex_post(&audio_play_mutex);
    return err;
  }

  player->cmp_file = (void *)ctx;
  player->dec_ops = &local_stream_dec_ops;
  os_mutex_post(&audio_play_mutex);
  return 0;
}

static int stream_music_dec_stop(music_player *player) {
  int err = 0;
  union audio_req req;

  int resume_play = 0;

  log_i("[AUDIO_DECODER]cbuf_music_dec_stop\n");

  if (player->dec_server) {
    req.dec.cmd = AUDIO_DEC_STOP;
    server_request(player->dec_server, AUDIO_REQ_DEC, &req);
  }

  int argv[2];
  argv[0] = AUDIO_SERVER_EVENT_END;
  argv[1] = (int)&player->queue;
  server_event_handler_del(player->dec_server, 2, argv);

  /* 关闭文件 */
  log_i("[AUDIO_DECODER]cbuf_music_dec_stop: suss\n");

  ring_queue_clear(&player->queue);

  return 0;
}

static void *local_stream_worker(void *arg) {
  local_stream_task_t *task = (local_stream_task_t *)arg;
  music_player *player = task->player;
  const char *path = task->path;
  int read_len;
  int prefill = 0;
  u8 buf[LOCAL_STREAM_CHUNK_SIZE];

  FILE *file = fopen(path, "r");
  if (!file) {
    log_e("[LOCAL_STREAM]open failed: %s\n", path);
    goto out;
  }

  g_local_stream_ctx.queue = &player->queue;
  g_local_stream_ctx.done = &g_local_stream_done;
  g_local_stream_ctx.abort = &g_local_stream_abort;
  g_local_stream_done = 0;

  while (!g_local_stream_abort && prefill < LOCAL_STREAM_PREFILL_SIZE) {
    read_len = fread(buf, 1, sizeof(buf), file);
    if (read_len <= 0) {
      break;
    }
    int written = 0;
    while (written < read_len && !g_local_stream_abort) {
      int ret = ring_queue_try_write(
          &player->queue, (const char *)buf + written, read_len - written);
      if (ret == RINGQUEUE_Is_Full) {
        msleep(2);
        continue;
      }
      if (ret < 0) {
        msleep(2);
        continue;
      }
      written += ret;
      prefill += ret;
    }
  }

  if (!g_local_stream_abort) {
    if (local_stream_dec_start(player, &g_local_stream_ctx) != 0) {
      goto out_close;
    }
  }

  while (!g_local_stream_abort) {
    read_len = fread(buf, 1, sizeof(buf), file);
    if (read_len <= 0) {
      break;
    }
    int written = 0;
    while (written < read_len && !g_local_stream_abort) {
      int ret = ring_queue_try_write(
          &player->queue, (const char *)buf + written, read_len - written);
      if (ret == RINGQUEUE_Is_Full) {
        msleep(2);
        continue;
      }
      if (ret < 0) {
        msleep(2);
        continue;
      }
      written += ret;
    }
  }

out_close:
  if (file) {
    fclose(file);
  }
out:
  g_local_stream_done = 1;
  g_local_stream_thread_running = 0;
  if (task) {
    free(task->path);
    free(task);
  }
  return NULL;
}

static int local_stream_dec_stop(music_player *player) {
  union audio_req req;

  log_i("[AUDIO_DECODER]local_stream_dec_stop\n");
  g_local_stream_abort = 1;
  g_local_stream_done = 1;

  if (player->dec_server) {
    req.dec.cmd = AUDIO_DEC_STOP;
    server_request(player->dec_server, AUDIO_REQ_DEC, &req);
  }

  int argv[2];
  argv[0] = AUDIO_SERVER_EVENT_END;
  argv[1] = (int)&g_local_stream_ctx;
  server_event_handler_del(player->dec_server, 2, argv);

  ring_queue_clear(&player->queue);
  return 0;
}

static const struct music_dec_ops cbuf_music_dec_ops = {
    .dec_file = stream_music_dec_file,
    // .dec_volume                 = cbuf_music_dec_volume,
    // .dec_progress         = cbuf_musci_dec_progress,
    .dec_stop = stream_music_dec_stop,
    .dec_status = __get_dec_status,
};

static const struct music_dec_ops local_stream_dec_ops = {
    .dec_file = NULL,
    .dec_stop = local_stream_dec_stop,
    .dec_status = __get_dec_status,
};

/*======================================================================================================================
 * local music decoder
 *======================================================================================================================*/
static int local_music_dec_file(music_player *player, void *fname) {
  int err;
  union audio_req req = {0};
  const char *base = audio_basename((const char *)fname);
  bool is_t_key = (strcmp(base, "t_key.mp3") == 0);
  u32 t0 = timer_get_ms();

  /* serialize decoder open/close to avoid concurrent allocations in audio
   * server */
  os_mutex_pend(&audio_play_mutex, 0);
  log_i("[TIMING]local_dec mutex_wait: %u ms (file=%s)", timer_get_ms() - t0,
        base);
  app_music_play_stop(player);
  log_i("[TIMING]local_dec after stop: %u ms (file=%s)", timer_get_ms() - t0,
        base);

  FILE *file = fopen(fname, "r");
  if (!file) {
    log_w("[TIMING]local_dec fopen failed: %u ms (file=%s)",
          timer_get_ms() - t0, base);
    os_mutex_post(&audio_play_mutex);
    return -1;
  }
  log_i("[TIMING]local_dec fopen ok: %u ms (file=%s)", timer_get_ms() - t0,
        base);
  log_i("[AUDIO_DECODER]app_music_dec_local_file %s\n", (char *)fname);

  req.dec.cmd = AUDIO_DEC_OPEN;
  req.dec.volume = player->volume;
  req.dec.output_buf = NULL;
  req.dec.output_buf_len = DEC_BUF_LEN;
  req.dec.file = (FILE *)file;
  req.dec.channel = 0;
  req.dec.sample_rate = 0;
  req.dec.priority = 1;
  req.dec.vfs_ops = NULL;
  req.dec.sample_source = "dac";
  g_t_key_first_pcm_pending = is_t_key ? 1 : 0;

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

  if (!player || !player->dec_server) {
    log_e("audio_dec_open: invalid player or dec_server\n");
    fclose((FILE *)file);
    os_mutex_post(&audio_play_mutex);
    return -EFAULT;
  }

  err = server_request(player->dec_server, AUDIO_REQ_DEC, &req);
  if (err) {
    log_w("[TIMING]local_dec open err=%d: %u ms (file=%s)", err,
          timer_get_ms() - t0, base);
    log_e("audio_dec_open: err = %d\n", err);
    fclose((FILE *)file);
    os_mutex_post(&audio_play_mutex);
    return err;
  }
  log_i("[TIMING]local_dec open ok: %u ms (file=%s)", timer_get_ms() - t0,
        base);

  player->play_time = req.dec.play_time;
  player->total_time = req.dec.total_time;

  req.dec.cmd = AUDIO_DEC_START;

  err = server_request(player->dec_server, AUDIO_REQ_DEC, &req);
  if (err) {
    log_w("[TIMING]local_dec start err=%d: %u ms (file=%s)", err,
          timer_get_ms() - t0, base);
    log_e("audio_dec_start: err = %d\n", err);
    fclose((FILE *)file);
    os_mutex_post(&audio_play_mutex);
    return err;
  }
  log_i("[TIMING]local_dec start ok: %u ms (file=%s)", timer_get_ms() - t0,
        base);
  player->file = (FILE *)file;
  player->cmp_file = (void *)file;
  mark_first_play_start(player);

  // if (!player->play_voice_prompt) {
  //                 player->dec_ops = &local_music_dec_ops;
  // }
  player->dec_ops = &local_music_dec_ops;

  led_ui_post_msg("dec_start");
  log_i("[AUDIO_DECODER]play_music_file: suss\n");

  /* release mutex after decoder successfully started */
  os_mutex_post(&audio_play_mutex);
  return 0;
}

static int local_music_dec_stop(music_player *player) {
  int err = 0;
  union audio_req req;

  int resume_play = 0;

  if (!player->file) {
    return 0;
  }

  log_i("[AUDIO_DECODER]local_music_dec_stop\n");

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
    .dec_file = local_music_dec_file,
    // .dec_volume                 = local_music_dec_volume,
    // .dec_progress         = local_musci_dec_progress,
    .dec_stop = local_music_dec_stop,
    .dec_status = __get_dec_status,
};

/*======================================================================================================================
 * net music decoder
 *======================================================================================================================*/
int my_net_download_read(void *priv, void *buf, u32 len) {
  int ret = net_download_read(priv, buf, len);
  log_d("[DEC] net file read: len = %d, ret = %d\n", len, ret);
  return ret;
}
int my_net_download_seek(void *priv, u32 offset, int orig) {
  int ret = net_download_seek(priv, offset, orig);
  log_d("[DEC] net file seek: offset = %d, orig = %d, ret = %d\n", offset, orig,
        ret);

  int tmp_size = net_download_get_tmp_data_size(priv);
  log_d("[DEC] net file tmp data size: %d\n", tmp_size);
  return ret;
}
int my_net_download_get_file_len(void *priv) {
  int ret = net_download_get_file_len(priv);
  log_d("[DEC] net file size: %d\n", ret);
  return ret;
}

static const struct audio_vfs_ops net_audio_dec_vfs_ops = {
    .fread = my_net_download_read,
    .fseek = my_net_download_seek,
    .flen = my_net_download_get_file_len,
};

static int net_music_dec_stop(music_player *player) {
  int err;
  union audio_req r = {0};

  log_i("[AUDIO_DECODER]net_music_dec_stop\n");

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

static int __net_download_ready(void *priv) {
  music_player *player = (music_player *)priv;
  player->download_ready = net_download_check_ready(player->net_file);
  if (player->download_ready) {
    return 1;
  }
  return 0;
}

static int __net_music_dec_file(void *priv) {
  int err;
  union audio_req req = {0};

  music_player *player = (music_player *)priv;

  os_mutex_pend(&audio_play_mutex, 0);

  player->wait_download = 0;
  player->wait_download_suspend = 0;

  if (player->download_ready < 0) {
    /* 网络下载失败 */
    goto __err;
  }
  // if (player->play_voice_prompt) {
  //                 set_dec_end_handler(player, player->file,
  //                 __net_music_dec_file, 0, -1); return 0;
  // }

  req.dec.dec_type = net_download_get_media_type(player->net_file);
  if (req.dec.dec_type == NULL) {
    goto __err;
  }

  net_download_set_read_timeout(player->net_file, 1000);

  req.dec.cmd = AUDIO_DEC_OPEN;
  req.dec.volume = player->volume;
  req.dec.output_buf = NULL;
  req.dec.output_buf_len = DEC_BUF_LEN;
  req.dec.file = (FILE *)player->net_file;
  req.dec.channel = 0;
  req.dec.sample_rate = 0;
  req.dec.priority = 1;
  req.dec.vfs_ops = &net_audio_dec_vfs_ops;
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
  mark_first_play_start(player);

  net_download_set_tmp_data_threshold(player->net_file, 80 * 1024);

  os_mutex_post(&audio_play_mutex);
  return 0;

__err:
  log_i("[AUDIO_DECODER]play_net_music_faild\n");

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

  os_mutex_post(&audio_play_mutex);
  return -EFAULT;
}

static int __set_dec_volume(music_player *player, int step) {
  union audio_req req = {0};

  int volume = player->volume + step;
  if (volume < MIN_VOLUME_VALUE) {
    volume = MIN_VOLUME_VALUE;
  } else if (volume > MAX_VOLUME_VALUE) {
    // TODO: play mix file
    // #ifdef CONFIG_VOICE_PROMPT_FILE_PATH
    //                                 app_music_play_mix_file(CONFIG_VOICE_PROMPT_FILE_PATH"VolumeFull.mp3");
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
  //                 if (volume == MAX_VOLUME_VALUE) {
  //                                 app_music_play_mix_file(CONFIG_VOICE_PROMPT_FILE_PATH"VolumeFull.mp3");
  //                 } else {
  //                                 app_music_play_mix_file(CONFIG_VOICE_PROMPT_FILE_PATH"Volume.mp3");
  //                 }
  // #endif

  if (!player->dec_server) {
    return -EFAULT;
  }

  req.dec.cmd = AUDIO_DEC_SET_VOLUME;
  req.dec.volume = volume;
  server_request(player->dec_server, AUDIO_REQ_DEC, &req);

  return 0;
}

static int net_music_dec_file(music_player *player, void *_url) {
  int err;
  struct net_download_parm parm;

  os_mutex_pend(&audio_play_mutex, 0);
  app_music_play_stop(player);

  log_i("[AUDIO_DECODER]net_download_open\n");

  if (!_url) {
    os_mutex_post(&audio_play_mutex);
    return -1;
  }

  memset(&parm, 0, sizeof(struct net_download_parm));
  parm.url = (const char *)_url;
  parm.prio = 0;
  parm.cbuf_size = 200 * 1024;
  parm.timeout_millsec = 10000;
  parm.seek_high_range = 0;
  parm.seek_low_range = 0;

  parm.seek_threshold = 1024 * 200; // 用户可适当调整
  /* parm.no_wait_close                        = 1; */
  /* parm.start_play_threshold = 1024 * 4; */

  err = net_download_open(&player->net_file, &parm);
  if (err) {
    log_e("net_download_open: err = %d\n", err);
    os_mutex_post(&audio_play_mutex);
    return err;
  }
  player->url = (const char *)_url;

  /*
   * 异步等待网络下载ready，防止网络阻塞导致app卡住
   */
  player->wait_download_suspend = 0;
  player->wait_download = wait_completion(__net_download_ready,
                                          (int (*)(void *))__net_music_dec_file,
                                          (void *)player, player);

  player->dec_ops = &net_music_dec_ops;

  os_mutex_post(&audio_play_mutex);
  return 0;
}

static const struct music_dec_ops net_music_dec_ops = {
    .dec_file = net_music_dec_file,
    // .dec_volume                 = net_music_dec_volume,
    // .dec_progress         = net_music_dec_progress,
    .dec_stop = net_music_dec_stop,
    .dec_status = __get_dec_status,
};

/*======================================================================================================================
 *
 *======================================================================================================================*/
int app_music_play_net_music(music_player *player, void *url) {
  net_music_dec_file(player, url);
  player->dec_end_cb = NULL;
  player->dec_end_cb_priv = NULL;
  player->dec_with_cb = 0;
  return 0;
}
int app_music_play_net_music_with_callback(music_player *player, void *url,
                                           app_music_play_end_cb_t cb,
                                           void *priv) {
  net_music_dec_file(player, url);
  player->dec_end_cb = cb;
  player->dec_end_cb_priv = priv;
  player->dec_with_cb = 1;
  return 0;
}
int app_music_play_lfs_music_with_callback(music_player *player, void *fname,
                                           app_music_play_end_cb_t cb,
                                           void *priv) {
  lfs_music_dec_file(player, fname);
  player->dec_end_cb = cb;
  player->dec_end_cb_priv = priv;
  player->dec_with_cb = 1;
  return 0;
}
int app_music_play_stream_music_with_callback(music_player *player,
                                              app_music_play_end_cb_t cb,
                                              void *priv) {
  stream_music_dec_file(player);
  player->dec_end_cb = cb;
  player->dec_end_cb_priv = priv;
  player->dec_with_cb = 1;
  return 0;
}
int app_music_play_local_music_with_callback(music_player *player, void *fname,
                                             app_music_play_end_cb_t cb,
                                             void *priv) {
  local_music_dec_file(player, fname);
  player->dec_end_cb = cb;
  player->dec_end_cb_priv = priv;
  player->dec_with_cb = 1;
  return 0;
}
int app_music_play_stream_write(music_player *player, void *buf, uint32_t len) {
  if (player->dec_ops == &cbuf_music_dec_ops) {
    assert(player->queue.capacity > len);
    return ring_queue_write(&player->queue, buf, len);
  }
  return 0;
}

int app_music_play_local_music_async_with_callback(music_player *player,
                                                   void *fname,
                                                   app_music_play_end_cb_t cb,
                                                   void *priv) {
  if (!player || !fname) {
    return -EINVAL;
  }

  g_local_stream_abort = 1;
  // 这里不应该等待太久不超过60ms
  if (g_local_stream_thread_running) {
    for (int i = 0; i < 30 && g_local_stream_thread_running; i++) {
      msleep(2);
    }
  }
  g_local_stream_abort = 0;
  g_local_stream_done = 0;
  ring_queue_clear(&player->queue);

  player->dec_end_cb = cb;
  player->dec_end_cb_priv = priv;
  player->dec_with_cb = 1;

  local_stream_task_t *task =
      (local_stream_task_t *)malloc(sizeof(local_stream_task_t));
  if (!task) {
    return -ENOMEM;
  }
  task->player = player;
  size_t path_len = strlen((const char *)fname) + 1;
  task->path = (char *)malloc(path_len);
  if (!task->path) {
    free(task);
    return -ENOMEM;
  }
  memcpy(task->path, fname, path_len);

  g_local_stream_thread_running = 1;
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  // Increase stack size to avoid pthread stack overflow on small defaults.
  pthread_attr_setstacksize(&attr, 8 * 1024);
  int th_err =
      pthread_create(&g_local_stream_thread, &attr, local_stream_worker, task);
  pthread_attr_destroy(&attr);
  if (th_err != 0) {
    g_local_stream_thread_running = 0;
    free(task->path);
    free(task);
    return -EFAULT;
  }
  return 0;
}

/*======================================================================================================================
 * Persistent Prompt Decoder - Fire-and-Forget Playback
 *======================================================================================================================*/
/**
 * @brief 播放提示音（使用常开解码器）
 * @param fname 提示音文件路径
 * @param cb 播放完成回调（当前不支持）
 * @param priv 回调私有数据
 * @return 0 成功，负值失败
 */
int app_music_play_prompt_sync(const char *fname, app_music_play_end_cb_t cb,
                               void *priv) {
  FILE *file = NULL;
  u8 buf[1024];
  int read_len;
  u32 total_written = 0;
  u32 t0 = timer_get_ms();
  const char *base = audio_basename(fname);

  if (!fname) {
    log_e("[PROMPT_SYNC]Invalid filename\n");
    return -EINVAL;
  }

  log_i("[PROMPT_SYNC]🎵 Playing: %s", base);

  os_mutex_pend(&audio_play_mutex, 0);

  // [DEBUG] 打印当前状态
  log_i("[PROMPT_SYNC] Entry: dec=%p, playing=%d", g_prompt_player.dec_server,
        g_prompt_playing);

  // 1. 清空队列（丢弃旧数据）
  ring_queue_clear(&g_prompt_player.queue);
  g_prompt_playing = 1;
  clear_first_play_pending(&g_prompt_player);

  // 2. 打开文件
  file = fopen(fname, "r");
  if (!file) {
    log_e("[PROMPT_SYNC]Failed to open file: %s", fname);
    g_prompt_playing = 0;
    os_mutex_post(&audio_play_mutex);
    return -ENOENT;
  }

  // 3. 推送数据到队列
  while ((read_len = fread(buf, 1, sizeof(buf), file)) > 0) {
    int written = 0;
    while (written < read_len) {
      int ret =
          ring_queue_try_write(&g_prompt_player.queue,
                               (const char *)buf + written, read_len - written);
      if (ret == RINGQUEUE_Is_Full) {
        // 队列满，短暂等待解码器消费
        os_mutex_post(&audio_play_mutex);
        msleep(2);
        os_mutex_pend(&audio_play_mutex, 0);
        continue;
      }
      if (ret < 0) {
        log_e("[PROMPT_SYNC]Queue write error: %d", ret);
        break;
      }
      written += ret;
      total_written += ret;
    }
  }

  fclose(file);

  // [DEBUG] 写入完成
  log_i("[PROMPT_SYNC] Loaded: size=%u", total_written);

  // 4. 如果是首次播放，需要OPEN+START解码器
  if (!g_prompt_player.dec_server) {
    log_i("[PROMPT_SYNC] First play (dec=NULL), opening decoder...");

    // 打开解码服务
    g_prompt_player.dec_server = server_open("audio_server", "dec");
    if (!g_prompt_player.dec_server) {
      log_e("[PROMPT_SYNC]Failed to open audio_server\n");
      g_prompt_playing = 0;
      os_mutex_post(&audio_play_mutex);
      return -EPERM;
    }
    server_register_event_handler(g_prompt_player.dec_server, &g_prompt_player,
                                  dec_server_event_handler);

    // OPEN解码器
    union audio_req req = {0};
    req.dec.cmd = AUDIO_DEC_OPEN;
    req.dec.dec_type = "mp3";
    req.dec.volume = g_prompt_player.volume;
    req.dec.output_buf = NULL;
    req.dec.output_buf_len = DEC_BUF_LEN;
    req.dec.file = (void *)&g_prompt_player.queue;
    req.dec.channel = 0;
    req.dec.sample_rate = 0;
    req.dec.priority = 1;
    req.dec.vfs_ops = &prompt_audio_dec_vfs_ops;
    req.dec.sample_source = "dac";

    int err = server_request(g_prompt_player.dec_server, AUDIO_REQ_DEC, &req);
    if (err) {
      log_e("[PROMPT_SYNC]Failed to OPEN decoder: %d", err);
      server_close(g_prompt_player.dec_server);
      g_prompt_player.dec_server = NULL;
      g_prompt_playing = 0;
      os_mutex_post(&audio_play_mutex);
      return err;
    }

    // START解码器
    req.dec.cmd = AUDIO_DEC_START;
    err = server_request(g_prompt_player.dec_server, AUDIO_REQ_DEC, &req);
    if (err) {
      log_e("[PROMPT_SYNC]Failed to START decoder: %d", err);
      req.dec.cmd = AUDIO_DEC_STOP;
      server_request(g_prompt_player.dec_server, AUDIO_REQ_DEC, &req);
      server_close(g_prompt_player.dec_server);
      g_prompt_player.dec_server = NULL;
      g_prompt_playing = 0;
      os_mutex_post(&audio_play_mutex);
      return err;
    }

    g_prompt_player.cmp_file = (void *)&g_prompt_player.queue;
    log_i("[PROMPT_SYNC]Decoder opened and started");
  } else {
    log_i("[PROMPT_SYNC] Reusing existing decoder, sending START to wake it "
          "up...");
    // 尝试重新 START，不需要重新 OPEN
    union audio_req req = {0};
    req.dec.cmd = AUDIO_DEC_START;
    int err = server_request(g_prompt_player.dec_server, AUDIO_REQ_DEC, &req);
    if (err) {
      log_w("[PROMPT_SYNC] Re-START failed: %d (maybe it never stopped?)", err);
    }
  }

  mark_first_play_start(&g_prompt_player);

  log_i("[TIMING]🚀 prompt_sync: %u ms, %u bytes (file=%s)",
        timer_get_ms() - t0, total_written, base);

  os_mutex_post(&audio_play_mutex);
  return 0;
}

static int prompt_music_dec_stop(music_player *player) {
  log_i("[PROMPT_DEC]prompt_music_dec_stop (clear queue only)");
  // 只清空队列，解码器保持运行状态
  ring_queue_clear(&g_prompt_player.queue);
  g_prompt_playing = 0;
  // 不调用 AUDIO_DEC_STOP，保持解码器常开
  return 0;
}

static const struct music_dec_ops prompt_music_dec_ops = {
    .dec_file = NULL,
    .dec_stop = prompt_music_dec_stop,
    .dec_status = __get_dec_status,
};

int app_music_play_stop(music_player *player) {
  log_i("[AUDIO_DECODER]app_music_play_stop");
  int ret = 0;
  os_mutex_pend(&audio_play_mutex, 0);
  clear_first_play_pending(player);
  if (player->dec_ops &&
      AUDIO_DEC_STOP != player->dec_ops->dec_status(player)) {
    // 停止解码
    ret = player->dec_ops->dec_stop(player);
    // 执行回调
    if (player->dec_with_cb && player->dec_end_cb != NULL) {
      player->dec_end_cb(player->dec_end_cb_priv);
      player->dec_end_cb = NULL;
    }
  }
  os_mutex_post(&audio_play_mutex);
  return 0;
}

int app_music_play_get_status(music_player *player) {
  if (player->dec_ops) {
    return player->dec_ops->dec_status(player);
  }
  return -1;
}

int set_dec_volume_step(music_player *player, int step) {
  return __set_dec_volume(player, step);
}

int set_dec_volume(music_player *player, int volume) {
  player->volume = 0;
  return __set_dec_volume(player, volume);
}

int music_player_init(music_player *player) {
  memset(player, 0, sizeof(music_player));
  player->dec_ops = NULL;
  ring_queue_init_static(&player->queue, player->queue_buffer,
                         sizeof(player->queue_buffer));

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
  server_register_event_handler(player->dec_server, player,
                                dec_server_event_handler);
  /* initialize audio play mutex on first music_player_init call */
  if (!os_mutex_valid(&audio_play_mutex)) {
    os_mutex_create(&audio_play_mutex);
  }

  /*======================================================================================================================
   * Initialize Persistent Prompt Decoder (Lazy-Open Strategy)
   *======================================================================================================================*/
  log_i("[PROMPT_INIT]🚀 Initializing prompt player (lazy-open mode)...");

  // 只初始化数据结构，不打开解码器（延迟到首次播放）
  memset(&g_prompt_player, 0, sizeof(g_prompt_player));
  ring_queue_init_static(&g_prompt_player.queue, g_prompt_queue_buffer,
                         PROMPT_QUEUE_SIZE);
  g_prompt_player.volume = player->volume;
  g_prompt_player.dec_ops = &prompt_music_dec_ops;
  g_prompt_player.dec_server = NULL; // 首次播放时打开
  g_prompt_playing = 0;
  g_prompt_eof = 0;

  log_i("[PROMPT_INIT]✅ Prompt player ready (will open on first play)");

  return 0;
}
