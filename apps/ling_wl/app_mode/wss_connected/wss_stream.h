#ifndef __WSS_STREAM_H__
#define __WSS_STREAM_H__

#include "cpu.h"

void wss_stream_init();
void wss_stream_uninit();

void wss_stream_start();
void wss_stream_stop();

void wss_stream_reset();

bool wss_stream_on();

/**
 * @brief 从预缓冲队列中获取数据
 * @param buffer 目标缓冲区指针
 * @param max_len 最多读取的字节数
 * @return 实际读取的数据字节数
 */
uint32_t wss_stream_get_data(uint8_t *buffer, uint32_t max_len);

#endif