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
#include <string.h>
#include <stdint.h>


#define CONFIG_RECORDER_MODE_ENABLE
#ifdef CONFIG_RECORDER_MODE_ENABLE

#define CONFIG_AUDIO_ENC_SAMPLE_SOURCE      "mic"                          //录音输入源选择
#define CONFIG_AUDIO_RECORDER_SAMPLERATE    16000                          //录音采样率
#define CONFIG_AUDIO_RECORDER_CHANNEL       1                              //录音通道数

#define CONFIG_AUDIO_ADC_CHANNEL_L          0        //左mic/aux通道
#define CONFIG_AUDIO_ADC_CHANNEL_R          2        //右mic/aux通道
#define CONFIG_AUDIO_ADC_GAIN               100      //mic/aux增益

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

    // 诊断日志：确认编码器是否真的在吐数据
    // 节流打印：前10次、每50次、以及异常(len==0)必打
    static u32 s_fwrite_cnt = 0;
    s_fwrite_cnt++;
    if (len == 0 || s_fwrite_cnt <= 10 || (s_fwrite_cnt % 50) == 0) {
        u8 *p = (u8 *)data;
        log_i("[REC][FW]cnt=%u len=%u cb=%p head=%02x %02x %02x %02x",
              s_fwrite_cnt, (u32)len, recorder ? recorder->data_cb : NULL,
              (p && len > 0) ? p[0] : 0,
              (p && len > 1) ? p[1] : 0,
              (p && len > 2) ? p[2] : 0,
              (p && len > 3) ? p[3] : 0);
    }

    if (recorder->data_cb != NULL) {
        recorder->data_cb(recorder->data_cb_priv, data, len);
    } else {
        // 如果没有注册回调，提示一次，避免“录音有输出但没人接”
        static u32 s_no_cb_cnt = 0;
        s_no_cb_cnt++;
        if (s_no_cb_cnt <= 5) {
            log_w("[REC][FW]data_cb is NULL, dropping len=%u", (u32)len);
        }
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
    recorder->running_flag = 0;

    if (recorder->enc_server) {
        // 先停止编码器，确保DNS编码器等完全停止
        req.enc.cmd = AUDIO_ENC_STOP;
        server_request(recorder->enc_server, AUDIO_REQ_ENC, &req);
        
        // 等待足够长的时间，确保DNS编码器任务完全退出
        // 增加等待时间，防止任务仍在运行时访问已释放的缓冲区
        msleep(200);
        
        // 然后关闭编码器
        req.enc.cmd = AUDIO_ENC_CLOSE;
        server_request(recorder->enc_server, AUDIO_REQ_ENC, &req);
        
        // 再等待一段时间，确保资源完全释放
        msleep(50);
    }

    // 只有在编码器完全停止后，才释放缓冲区
    // 先清空缓冲区，防止残留数据导致问题
    if (recorder->cache_buf) {
        cbuf_clear(&recorder->save_cbuf);
        free(recorder->cache_buf);
        recorder->cache_buf = NULL;
        // 清零缓冲区结构，防止悬空指针
        memset(&recorder->save_cbuf, 0, sizeof(cbuffer_t));
    }

    if (recorder->fp) {
        fclose(recorder->fp);
        recorder->fp = NULL;
    }

    return 0;
}

static void enc_server_event_handler(void *priv, int argc, int *argv)
{
    recorder_hdl *recorder = (recorder_hdl *)priv;
    switch (argv[0]) {
    case AUDIO_SERVER_EVENT_ERR:
    case AUDIO_SERVER_EVENT_END:
        recorder_close(recorder);
        break;
    case AUDIO_SERVER_EVENT_SPEAK_START:
        // log_i("[Recorder VAD]Speak start !\n");
        if (recorder && recorder->vad_cb) {
            recorder->vad_cb(recorder->data_cb_priv, 1);
        }
        break;
    case AUDIO_SERVER_EVENT_SPEAK_STOP:
        // log_i("[Recorder VAD]Speak stop !\n");
        if (recorder && recorder->vad_cb) {
            recorder->vad_cb(recorder->data_cb_priv, 0);
        }
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
    log_i("[REC][INIT]enter recorder=%p init=%u running=%u sr=%d ch=%u src=%s",
          recorder,
          recorder ? recorder->init_flag : 0,
          recorder ? recorder->running_flag : 0,
          sample_rate, channel,
          (recorder && recorder->sample_source) ? recorder->sample_source : "(null)");

    if (channel > 2) {
        channel = 2;
    }
    // 修复：添加整数溢出检查和合理的上限检查
    // 确保缓冲区大小对齐到4字节，避免内存对齐问题
    size_t cache_size = (size_t)sample_rate * (size_t)channel;
    // 检查整数溢出：如果sample_rate或channel很大，乘积可能溢出
    if (cache_size < (size_t)sample_rate || cache_size < (size_t)channel) {
        log_e("[recorder]Integer overflow in cache size calculation: sample_rate=%d, channel=%d", 
              sample_rate, channel);
        return -1;
    }
    // 设置合理的上限（例如10MB），防止分配过大内存
    const size_t MAX_CACHE_SIZE = 10 * 1024 * 1024;  // 10MB
    if (cache_size > MAX_CACHE_SIZE) {
        log_e("[recorder]Cache size too large: %zu > %zu, sample_rate=%d, channel=%d", 
              cache_size, MAX_CACHE_SIZE, sample_rate, channel);
        return -1;
    }
    u32 buf_size = (u32)cache_size;
    buf_size = (buf_size + 3) & ~3;  // 对齐到4字节边界
    
    recorder->cache_buf = malloc(buf_size);
    if (recorder->cache_buf == NULL) {
        log_e("[recorder]Failed to allocate cache buffer: size=%u", buf_size);
        return -1;
    }
    
    // 确保缓冲区地址对齐（虽然malloc通常已经对齐，但为了安全起见）
    uintptr_t addr = (uintptr_t)recorder->cache_buf;
    if (addr % 4 != 0) {
        log_w("recorder buffer address not aligned: 0x%x", addr);
        // 如果未对齐，调整大小以补偿
        buf_size = (buf_size + 3) & ~3;
    }
    
    cbuf_init(&recorder->save_cbuf, recorder->cache_buf, buf_size);

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
    req.enc.frame_size = 144; //sample_rate / 100 * 4 * channel;	//收集够多少字节PCM数据就回调一次fwrite
    req.enc.output_buf_len = req.enc.frame_size * 3; //底层缓冲buf至少设成3倍frame_size
    req.enc.cmd = AUDIO_ENC_OPEN;
    req.enc.channel = channel;
    req.enc.volume = recorder->gain;
    req.enc.sample_rate = sample_rate;
    req.enc.format = "mp3";
    req.enc.sample_source = recorder->sample_source;
    req.enc.vfs_ops = &recorder_vfs_ops;
    req.enc.file = (FILE *)recorder;
    if (channel == 1 && !strcmp(recorder->sample_source, "mic") && (sample_rate == 8000 || sample_rate == 16000)) {
        /*
         * Push-to-talk product requirement:
         * - Do NOT use device-side VAD (it causes frequent [VAD] start/stop logs and sentence splitting).
         * - Recording/upload is controlled by key press/release in upper layer.
         */
        req.enc.use_vad = 0;
        req.enc.vad_auto_refresh = 0;
        req.enc.dns_enable = 1; // keep noise suppression
    }
    if (req.enc.dns_enable == 1) //降噪的配置
    {
        // 配置DNS参数
        struct aec_s_attr parm = {0};
        parm.DNS_gain_floor = 0.1;      //单麦 建议0.1
        parm.DNS_over_drive = 2;        //建议 1-3.0  不能太大，太大会吃音(失真)
        req.enc.aec_attr = &parm;
    }

    log_i("[REC][INIT]open enc: cmd=%u fmt=%s sr=%u ch=%u frame=%u outbuf=%u use_vad=%u dns=%u",
          (u32)req.enc.cmd,
          req.enc.format ? req.enc.format : "(null)",
          (u32)req.enc.sample_rate,
          (u32)req.enc.channel,
          (u32)req.enc.frame_size,
          (u32)req.enc.output_buf_len,
          (u32)req.enc.use_vad,
          (u32)req.enc.dns_enable);

    err = server_request(recorder->enc_server, AUDIO_REQ_ENC, &req);
    if (err) {
        log_e("[REC][INIT]AUDIO_ENC_OPEN failed err=%d", err);
        goto __err;
    }

    log_i("[REC][INIT]AUDIO_ENC_OPEN ok");
    return 0;

__err:
    if (recorder->cache_buf) {
        free(recorder->cache_buf);
        recorder->cache_buf = NULL;
    }

    recorder->init_flag = 0;
    recorder->running_flag = 0;

    return -1;
}

/*
 * NOTE:
 * AUDIO_ENC_PP is a toggle command. To avoid accidentally "resuming" when we only
 * want to stop/pause, we wrap it as a "set running state" helper: only toggle
 * when current state differs from desired state.
 */
static void recorder_set_running_state(recorder_hdl *recorder, u8 running)
{
    union audio_req req = {0};

    if (!recorder || !recorder->enc_server) {
        return;
    }
    if (!!running == !!recorder->running_flag) {
        return;
    }

    log_i("[REC][PP]toggle running %u -> %u (init=%u)", (u32)recorder->running_flag, (u32)!!running, (u32)recorder->init_flag);
    /*
     * IMPORTANT:
     * - recorder_stop() uses AUDIO_ENC_STOP (hard stop).
     * - Many audio_server implementations do NOT allow resuming a STOPped encoder with AUDIO_ENC_PP.
     *   That matches your log: ret=-22 (EINVAL).
     * So when we want to "start" from stopped state, use AUDIO_ENC_START.
     */
    if (running) {
        req.enc.cmd = AUDIO_ENC_START;
        int ret = server_request(recorder->enc_server, AUDIO_REQ_ENC, &req);
        if (ret == 0) {
            recorder->running_flag = 1;
            if (recorder->cache_buf) {
                cbuf_clear(&recorder->save_cbuf);
            }
            log_i("[REC][START_CMD]ok running=1");
            return;
        }
        log_e("❌❌❌ [REC][START_CMD]fail ret=%d, fallback to PP", ret);
        // fallback: try PP for implementations where STOP==PAUSE
        req.enc.cmd = AUDIO_ENC_PP;
        ret = server_request(recorder->enc_server, AUDIO_REQ_ENC, &req);
        if (ret == 0) {
            recorder->running_flag = 1;
            if (recorder->cache_buf) {
                cbuf_clear(&recorder->save_cbuf);
            }
            log_i("[REC][PP]ok running=1");
        } else {
            log_e("[REC][PP]fail ret=%d", ret);
        }
    } else {
        // We don't use "pause" in current flow; stop is handled by recorder_stop().
        req.enc.cmd = AUDIO_ENC_PP;
        int ret = server_request(recorder->enc_server, AUDIO_REQ_ENC, &req);
        if (ret == 0) {
            recorder->running_flag = 0;
            if (recorder->cache_buf) {
                cbuf_clear(&recorder->save_cbuf);
            }
            log_i("[REC][PP]ok running=0");
        } else {
            log_e("[REC][PP]fail ret=%d", ret);
        }
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
    server_register_event_handler_to_task(recorder->enc_server, recorder, enc_server_event_handler, "app_core");

    return RECERROR_Success;
}

int recorder_set_data_cb(recorder_hdl *recorder, recorder_data_cb_t cb, void *priv)
{
    if (recorder == NULL) {
        return RECERROR_Null_Pointer;
    }
    log_i("[REC][CB]set data_cb=%p priv=%p (init=%u running=%u)",
          cb, priv, (u32)recorder->init_flag, (u32)recorder->running_flag);
    recorder->data_cb = cb;
    recorder->data_cb_priv = priv;

    return RECERROR_Success;
}

int recorder_set_vad_cbk(recorder_hdl *recorder, recorder_vad_cb_t cb, void *priv)
{
    if (recorder == NULL) {
        return RECERROR_Null_Pointer;
    }
    recorder->vad_cb = cb;
    recorder->vad_cb_priv = priv;

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
    log_i("[REC][START]enter init=%u running=%u", (u32)recorder->init_flag, (u32)recorder->running_flag);
    if (recorder_get_play_pause_state(recorder) == 0) {
        if (recorder->init_flag == 0) {
            int ret = recorder_enc_init(recorder, recorder->sample_rate, recorder->channel);
            log_i("[REC][START]enc_init ret=%d init=%u running=%u", ret, (u32)recorder->init_flag, (u32)recorder->running_flag);
        } else {
            recorder_set_running_state(recorder, 1);
        }
    }
    
    return RECERROR_Success;
}

int recorder_stop(recorder_hdl *recorder)
{
    if (recorder == NULL) {
        return RECERROR_Null_Pointer;
    }
    log_i("[REC][STOP]enter init=%u running=%u", (u32)recorder->init_flag, (u32)recorder->running_flag);

    /*
     * IMPORTANT:
     * 当前项目里 stop 之后经常出现 AUDIO_ENC_START / AUDIO_ENC_PP 返回 -22(EINVAL)，
     * 说明 audio_server 把 AUDIO_ENC_STOP 当作“硬停止/释放”，后续不能用 START/PP 恢复。
     * 因此这里直接走 recorder_close()（STOP + CLOSE + 释放缓冲），让下次 start 重新 OPEN。
     */
    recorder_close(recorder);
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
