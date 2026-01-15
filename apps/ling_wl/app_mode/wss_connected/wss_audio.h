#ifndef __WSS_AUDIO_H__
#define __WSS_AUDIO_H__

#include "cpu.h"

/**
 * @brief 初始化WSS音频模块
 * @details
 *   初始化互斥锁、stream缓冲、播放器等资源。
 */
void wss_audio_init();

/**
 * @brief 反初始化WSS音频模块
 * @details
 *   停止音频播放线程，释放所有资源。
 */
void wss_audio_uninit();

/**
 * @brief 开始蓝牙音频播放
 * @details
 *   启动音频播放线程，注册stream数据回调。
 *   线程会循环调用wss_stream_get_data()获取数据并写入播放器。
 */
void wss_ble_audio_start();

/**
 * @brief 停止蓝牙音频播放
 * @details
 *   停止播放线程，停止播放器，清空stream缓冲队列。
 */
void wss_ble_audio_stop();

/**
 * @brief 获取当前音频播放状态
 * @return true 正在播放，false 未播放或已停止
 */
bool is_audio_playing();

/**
 * @brief 设置播放器音量
 * @param volume 音量值(1~100)
 */

#endif