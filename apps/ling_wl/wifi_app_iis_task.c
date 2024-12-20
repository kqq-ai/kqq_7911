#include "system/includes.h"
#include "app_config.h"
#include "asm/iis.h"
#include "hello_record_pcm.inl"

//#define USE_IIS_TEST_DEMO

#ifdef USE_IIS_TEST_DEMO
// 全局缓冲区模拟音频数据
//u8 audio_data[1024] = { /* 填充一些音频数据 */ };

// 音频输出回调函数
void audio_output_callback(void *priv, u8 *data, int len, u8 channel) {
    // 将音频数据复制到输出缓冲区
    memcpy(data, _ac_hello_pcm_data, len);
    printf("-------------------------Audio Output: Channel=%d, Length=%d------------------\r\n", channel, len);
}

void iis_test_task(void) {
    printf("---------------------------IIS Test Started.-------------------------------\r\n");
    struct iis_platform_data iis_cfg = {
        .channel_in = 0,               // 不启用输入通道
        .channel_out = BIT(0),         // 启用 CH0（单声道输出）
        .port_sel = IIS_PORTA,         // 使用 PORTA IO 组
        .data_width = 16,              // 数据位宽 16 位
        .mclk_output = 1,              // 输出 MCLK
        .slave_mode = 0,               // 主机模式
        .update_edge = 1,              // SCLK 上升沿更新数据
        .f32e = 1,                     // 每帧 64 个 SCLK
        .keep_alive = 1,               // 保持硬件模块打开
        .sel = 0,                      // 使用左通道数据
        .width_16_to_24 = 0,           // 不扩展 16bit 数据为 24bit
        .dump_points_num = 0,          // 不丢弃数据
        .sr_points = 256,              // 默认采样点数
    };

    // 打开 IIS0 模块
    if (iis_open(&iis_cfg, 0) != 0) {
        printf("Failed to open IIS0!\n");
        return;
    }

    // 设置解码器数据输出回调
    iis_set_dec_data_handler(NULL, audio_output_callback, 0);

    // 设置采样率为 44.1kHz
    if (iis_set_sample_rate(16000, 0) != 0) {
        printf("Failed to set sample rate!\n");
        return;
    }

    // 打开输出通道 CH0 和 CH1
    iis_channel_on(BIT(0), 0);

    // 模拟任务运行
    printf("IIS Test Running...\n");
    for (int i = 0; i < 10; i++) {
        // 模拟音频数据处理
        printf("Playing audio frame %d...\n", i);
    }

    while(1);

    // 关闭通道和模块
    iis_channel_off(BIT(0), 0);
    iis_close(0);
    printf("---------------------------IIS Test Finished.------------------------------------\r\n");
}

static int iis_test_c_main1(void)
{
    os_task_create(iis_test_task, NULL, 10, 1000, 0, "iis_test_task");
    return 0;
}

late_initcall(iis_test_c_main1);


#endif