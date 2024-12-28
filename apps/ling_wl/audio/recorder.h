#ifndef __RECORDER_H__
#define __RECORDER_H__

#include "fs/fs.h"
#include "generic/circular_buf.h"
#include "server/server_core.h"

typedef void (*recorder_data_cb_t)(void* priv, void *data, u32 size);

typedef struct recorder_hdl {
    FILE *fp;
    struct server *enc_server;
    void *cache_buf;
    cbuffer_t save_cbuf;
    volatile u8 init_flag;
    volatile u8 running_flag;    // running:1, pause:0
    u8 gain;
    u8 channel;
    u8 direct;
    const char *sample_source;
    int sample_rate;
    recorder_data_cb_t data_cb;
    void *data_cb_priv;
} recorder_hdl;

typedef enum {
    RECERROR_Success = 0,
    RECERROR_Null_Pointer = -1,
    RECERROR_Malloc_Failed = -2,
    RECERROR_Api_Error = -3,
} RecorderErrorType;

int recorder_mode_init(recorder_hdl *recorder);
int recorder_set_data_cb(recorder_hdl *recorder, recorder_data_cb_t cb, void *priv);
int recorder_mode_exit(recorder_hdl *recorder);
int recorder_start(recorder_hdl *recorder);
int recorder_stop(recorder_hdl *recorder);
int recorder_get_play_pause_state(recorder_hdl *recorder);

#endif