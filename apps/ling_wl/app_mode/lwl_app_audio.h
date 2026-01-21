#ifndef __LWL_APP_AUDIO_H__
#define __LWL_APP_AUDIO_H__

#include "cpu.h"
//播放音频大概需要等待100ms左右
#define play_system_music(x) lwl_app_audio_play(CONFIG_CP_VOICE_FILE_PATH x, NULL);


/**
 * @brief 初始化统一音频管理模块
 * @details
 *   初始化互斥锁、音频播放控制变量等资源。
 *   应在应用启动早期调用。
 */
void lwl_app_audio_init(void);

/**
 * @brief 反初始化统一音频管理模块
 * @details
 *   停止所有音频播放，释放互斥锁等资源。
 */
void lwl_app_audio_uninit(void);

/**
 * @brief 播放本地提示音文件
 * @details
 *   同步等待上一个播放完成，然后启动新播放。
 *   会先关闭任何正在进行的流式播放，再启动本地文件播放。
 *   播放完成时调用回调函数。
 * @param fname 本地音频文件名
 * @param priv  播放完成时的回调函数指针
 */
void lwl_app_audio_play(const char *fname, void *priv);

/**
 * @brief 检查本地文件播放是否正在进行
 * @return true 正在播放本地文件，false 未播放或已停止
 */
bool lwl_app_audio_playing(void);

/**
 * @brief 停止本地文件播放
 * @details
 *   立即停止播放器，清空待播放文件名。
 */
void lwl_app_audio_stop_playing(void);

/**
 * @brief 启动流式音频播放
 * @details
 *   先关闭本地文件播放，再启动流式音频播放线程。
 *   应确保stream已经初始化并正常运行。
 */
void lwl_app_audio_stream_start(void);

/**
 * @brief 停止流式音频播放
 * @details
 *   停止流式播放线程，停止播放器，清空stream缓冲。
 */
void lwl_app_audio_stream_stop(void);

/**
 * @brief 检查流式播放是否正在进行
 * @return true 正在进行流式播放，false 未播放或已停止
 */
bool lwl_app_audio_stream_playing(void);

/**
 * @brief 设置播放器音量
 * @param volume 音量值(1~100)
 */
void lwl_app_audio_set_volume(int volume);

/**
 * @brief 获取当前模式的播放状态
 * @details
 *   检查本地文件播放或流式播放是否有一个正在进行。
 * @return true 有音频正在播放，false 未播放
 */
bool lwl_app_audio_is_playing(void);

#endif
