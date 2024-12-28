#include "recorder.h"
#include "server/audio_server.h"
#include "server/server_core.h"
#include "system/app_core.h"
#include "generic/circular_buf.h"
#include "os/os_api.h"
#include "app_config.h"
#include "syscfg/syscfg_id.h"
#include "event/key_event.h"
#include "storage_device.h"
#include "fs/fs.h"
#include <time.h>


#define CONFIG_RECORDER_MODE_ENABLE
#ifdef CONFIG_RECORDER_MODE_ENABLE

#define CONFIG_AUDIO_ENC_SAMPLE_SOURCE      "mic"                          //录音输入源选择
#define CONFIG_AUDIO_DEC_PLAY_SOURCE        "dac"                          //播放输出源选择
#define CONFIG_AUDIO_RECORDER_SAMPLERATE    16000                          //录音采样率
#define CONFIG_AUDIO_RECORDER_CHANNEL       1                              //录音通道数

#define CONFIG_AUDIO_ADC_CHANNEL_L          1        //左mic/aux通道
#define CONFIG_AUDIO_ADC_CHANNEL_R          3        //右mic/aux通道
#define CONFIG_AUDIO_ADC_GAIN               100      //mic/aux增益

#define CONFIG_STORE_VOLUME
#define VOLUME_STEP         5
#define GAIN_STEP           5
#define MIN_VOLUME_VALUE	5
#define MAX_VOLUME_VALUE	100
#define INIT_VOLUME_VALUE   50

//AUDIO ADC支持的采样率
static const u16 sample_rate_table[] = {
    8000,
    11025,
    12000,
    16000,
    22050,
    24000,
    32000,
    44100,
    48000,
};

//编码器输出PCM数据
static int recorder_vfs_fwrite(void *file, void *data, u32 len)
{
    recorder_hdl *recorder = (recorder_hdl *)file;

    if (recorder->data_cb != NULL) {
        printf("vfs: %d", len);
        recorder->data_cb(recorder->data_cb_priv, data, len);
    }

    //此回调返回0录音就会自动停止
    return len;
}

static int recorder_vfs_fclose(void *file)
{
    return 0;
}

static int recorder_vfs_flen(void *file)
{
    return 0;
}

static const struct audio_vfs_ops recorder_vfs_ops = {
    .fwrite = recorder_vfs_fwrite,
    .fread  = NULL,
    .fclose = recorder_vfs_fclose,
    .flen   = recorder_vfs_flen,
};

static int recorder_close(recorder_hdl *recorder)
{
    union audio_req req = {0};

    if (!recorder->init_flag) {
        return 0;
    }

    log_d("----------recorder close----------\n");

    recorder->init_flag = 0;

    if (recorder->enc_server) {
        req.enc.cmd = AUDIO_ENC_CLOSE;
        server_request(recorder->enc_server, AUDIO_REQ_ENC, &req);
    }

    if (recorder->cache_buf) {
        free(recorder->cache_buf);
        recorder->cache_buf = NULL;
    }

    if (recorder->fp) {
        fclose(recorder->fp);
        recorder->fp = NULL;
    }

    return 0;
}

static void enc_server_event_handler(recorder_hdl *recorder, void *priv, int argc, int *argv)
{
    switch (argv[0]) {
    case AUDIO_SERVER_EVENT_ERR:
    case AUDIO_SERVER_EVENT_END:
        recorder_close(recorder);
        break;
    case AUDIO_SERVER_EVENT_SPEAK_START:
        log_i("speak start ! \n");
        break;
    case AUDIO_SERVER_EVENT_SPEAK_STOP:
        log_i("speak stop ! \n");
        break;
    default:
        break;
    }
}

//将MIC的数字信号采集后推到用户注册的回调
static int recorder_enc_init(recorder_hdl *recorder, int sample_rate, u8 channel)
{
    int err;
    union audio_req req = {0};

    log_d("----------recorder_enc_init----------\n");

    if (channel > 2) {
        channel = 2;
    }
    recorder->cache_buf = malloc(sample_rate * channel); //上层缓冲buf缓冲0.5秒的数据，缓冲太大听感上会有延迟
    if (recorder->cache_buf == NULL) {
        return -1;
    }
    cbuf_init(&recorder->save_cbuf, recorder->cache_buf, sample_rate * channel);

    recorder->init_flag = 1;
    recorder->running_flag = 1;

    /****************打开编码器*******************/
    memset(&req, 0, sizeof(union audio_req));

    //BIT(x)用来区分上层需要获取哪个通道的数据
    if (channel == 2) {
        req.enc.channel_bit_map = BIT(CONFIG_AUDIO_ADC_CHANNEL_L) | BIT(CONFIG_AUDIO_ADC_CHANNEL_R);
    } else {
        req.enc.channel_bit_map = BIT(CONFIG_AUDIO_ADC_CHANNEL_L);
    }
    req.enc.frame_size = 1280; //sample_rate / 100 * 4 * channel;	//收集够多少字节PCM数据就回调一次fwrite
    req.enc.output_buf_len = req.enc.frame_size * 3; //底层缓冲buf至少设成3倍frame_size
    req.enc.cmd = AUDIO_ENC_OPEN;
    req.enc.channel = channel;
    req.enc.volume = recorder->gain;
    req.enc.sample_rate = sample_rate;
    req.enc.format = "pcm";
    req.enc.sample_source = recorder->sample_source;
    req.enc.vfs_ops = &recorder_vfs_ops;
    req.enc.file = (FILE *)recorder;
    if (channel == 1 && !strcmp(recorder->sample_source, "mic") && (sample_rate == 8000 || sample_rate == 16000)) {
        req.enc.use_vad = 0; //打开VAD断句功能
        req.enc.dns_enable = 0; //打开降噪功能
        req.enc.vad_auto_refresh = 0; //VAD自动刷新
    }

    err = server_request(recorder->enc_server, AUDIO_REQ_ENC, &req);
    if (err) {
        goto __err;
    }

    return 0;

__err:
    if (recorder->cache_buf) {
        free(recorder->cache_buf);
        recorder->cache_buf = NULL;
    }

    recorder->init_flag = 0;

    return -1;
}

static void recorder_play_pause(recorder_hdl *recorder)
{
    union audio_req req = {0};

    req.enc.cmd = AUDIO_ENC_PP;
    int ret = server_request(recorder->enc_server, AUDIO_REQ_ENC, &req);
    if (ret == 0) {
        printf("play_pause success.\n");
        recorder->running_flag = !recorder->running_flag;
    } else {
        printf("play_pause failure.\n");
    }

    if (recorder->cache_buf) {
        cbuf_clear(&recorder->save_cbuf);
    }
}

//调整ADC的模拟增益
static int recorder_enc_gain_change(recorder_hdl *recorder, int step)
{
    union audio_req req = {0};

    int gain = recorder->gain + step;
    if (gain < 0) {
        gain = 0;
    } else if (gain > 100) {
        gain = 100;
    }
    if (gain == recorder->gain) {
        return -1;
    }
    recorder->gain = gain;

    if (!recorder->enc_server) {
        return -1;
    }

    log_d("set_enc_gain: %d\n", gain);

    req.enc.cmd     = AUDIO_ENC_SET_VOLUME;
    req.enc.volume  = gain;
    return server_request(recorder->enc_server, AUDIO_REQ_ENC, &req);
}

int recorder_mode_init(recorder_hdl *recorder)
{
    log_i("recorder_play_main\n");
    if (recorder == NULL) {
        return RECERROR_Null_Pointer;
    }

    memset(recorder, 0, sizeof(struct recorder_hdl));

    recorder->sample_source = "mic";

    recorder->channel = CONFIG_AUDIO_RECORDER_CHANNEL;
    recorder->gain = CONFIG_AUDIO_ADC_GAIN;
    recorder->sample_rate = CONFIG_AUDIO_RECORDER_SAMPLERATE;

    recorder->enc_server = server_open("audio_server", "enc");
    server_register_event_handler_to_task(recorder->enc_server, NULL, enc_server_event_handler, "app_core");

    return RECERROR_Success;
}

int recorder_set_data_cb(recorder_hdl *recorder, recorder_data_cb_t cb, void *priv)
{
    if (recorder == NULL) {
        return RECERROR_Null_Pointer;
    }
    recorder->data_cb = cb;
    recorder->data_cb_priv = priv;

    return RECERROR_Success;
}

int recorder_mode_exit(recorder_hdl *recorder)
{
    if (recorder == NULL) {
        return RECERROR_Null_Pointer;
    }
    recorder_close(recorder);
    server_close(recorder->enc_server);
    recorder->enc_server = NULL;

    return RECERROR_Success;
}

int recorder_start(recorder_hdl *recorder)
{
    if (recorder == NULL) {
        return RECERROR_Null_Pointer;
    }
    if (recorder_get_play_pause_state(recorder) == 0) {
        if (recorder->init_flag == 0) {
            recorder_enc_init(recorder, recorder->sample_rate, recorder->channel);
        } else {
            recorder_play_pause(recorder);
        }
    }
    
    return RECERROR_Success;
}

int recorder_stop(recorder_hdl *recorder)
{
    if (recorder == NULL) {
        return RECERROR_Null_Pointer;
    }
    if (recorder_get_play_pause_state(recorder) == 1) {
        recorder_play_pause(recorder);
    }
    
    return RECERROR_Success;
}

int recorder_get_play_pause_state(recorder_hdl *recorder)
{
    if (recorder == NULL) {
        return RECERROR_Null_Pointer;
    }
    return recorder->running_flag;
}
#endif
