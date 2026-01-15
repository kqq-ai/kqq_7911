#include "wss_stream.h"
#include "wss_app_util.h"
#include "os/os_api.h"

static OS_MUTEX g_stream_mutex;

#define PREBUFFER_SIZE        (500 * 1024) /* 500KB */

static uint8_t g_prebuffer[PREBUFFER_SIZE];
static uint32_t g_prebuffer_write_pos = 0; /* 循环队列写指针 */
static uint32_t g_prebuffer_read_pos = 0;  /* 循环队列读指针 */
static bool g_stream_end = false; // 音频流结束标志

/**
 * @brief 计算循环队列中可读取的数据长度
 * @return 可读数据的字节数
 */
static uint32_t _get_readable_size(void)
{
    if (g_prebuffer_write_pos >= g_prebuffer_read_pos) {
        return g_prebuffer_write_pos - g_prebuffer_read_pos;
    } else {
        return PREBUFFER_SIZE - g_prebuffer_read_pos + g_prebuffer_write_pos;
    }
}

/**
 * @brief 计算循环队列中可写入的空间大小
 * @return 可写空间的字节数
 */
static uint32_t _get_writable_size(void)
{
    if (g_prebuffer_write_pos >= g_prebuffer_read_pos) {
        return PREBUFFER_SIZE - (g_prebuffer_write_pos - g_prebuffer_read_pos) - 1;
    } else {
        return g_prebuffer_read_pos - g_prebuffer_write_pos - 1;
    }
}

/**
 * @brief 音频数据流写入（不包含播放启动逻辑）
 * @details
 *   接收音频数据并写入预缓冲循环队列。
 *   无内部计时逻辑，仅负责数据缓冲/转发。
 * @param data 音频数据指针
 * @param len  音频数据长度
 * @return 0 成功，-1 失败
 */
static int wss_stream_stream_write(uint8_t *data, uint32_t len)
{
    log_i("[STREAM]wss_stream_stream_write len=%u", len);
    if (!data || len == 0) {
        return 0;
    }

    os_mutex_pend(&g_stream_mutex, 0);

    uint32_t writable = _get_writable_size();
    
    if (writable >= len) {
        /* 有足够空间，写入数据 */
        uint32_t space_to_end = PREBUFFER_SIZE - g_prebuffer_write_pos;
        
        if (space_to_end >= len) {
            /* 可以一次性写入 */
            memcpy(g_prebuffer + g_prebuffer_write_pos, data, len);
            g_prebuffer_write_pos = (g_prebuffer_write_pos + len) % PREBUFFER_SIZE;
        } else {
            /* 需要分两部分写入 */
            memcpy(g_prebuffer + g_prebuffer_write_pos, data, space_to_end);
            memcpy(g_prebuffer, data + space_to_end, len - space_to_end);
            g_prebuffer_write_pos = len - space_to_end;
        }
        
        log_d("[STREAM]prebuffered %u bytes, readable=%u", len, _get_readable_size());
        os_mutex_post(&g_stream_mutex);
        return 0;
    } else {
        /* 预缓冲已满，丢弃数据 */
        log_w("[STREAM]prebuffer full, can only write %u bytes, dropping %u bytes", writable, len);
        os_mutex_post(&g_stream_mutex);
        return -1;
    }
}

/**
 * @brief 从预缓冲队列中获取数据
 * @param buffer 目标缓冲区指针
 * @param max_len 最多读取的字节数
 * @return 实际读取的数据字节数
 */
uint32_t wss_stream_get_data(uint8_t *buffer, uint32_t max_len)
{
    if (!buffer || max_len == 0) {
        return 0;
    }

    os_mutex_pend(&g_stream_mutex, 0);

    uint32_t readable = _get_readable_size();
    uint32_t to_read = (readable < max_len) ? readable : max_len;
    
    if (to_read > 0) {
        uint32_t space_to_end = PREBUFFER_SIZE - g_prebuffer_read_pos;
        
        if (space_to_end >= to_read) {
            /* 可以一次性读取 */
            memcpy(buffer, g_prebuffer + g_prebuffer_read_pos, to_read);
            g_prebuffer_read_pos = (g_prebuffer_read_pos + to_read) % PREBUFFER_SIZE;
        } else {
            /* 需要分两部分读取 */
            memcpy(buffer, g_prebuffer + g_prebuffer_read_pos, space_to_end);
            memcpy(buffer + space_to_end, g_prebuffer, to_read - space_to_end);
            g_prebuffer_read_pos = to_read - space_to_end;
        }
        
        log_d("[STREAM]retrieved %u bytes", to_read);
    }
    
    os_mutex_post(&g_stream_mutex);
    
    return to_read;
}

void wss_stream_start() {
    os_mutex_pend(&g_stream_mutex, 0);
    g_stream_end = false;
    g_prebuffer_write_pos = 0;
    g_prebuffer_read_pos = 0;
    odmp_register_audio_data_cbk(wss_stream_stream_write); 
    os_mutex_post(&g_stream_mutex);
}

void wss_stream_stop() {
    os_mutex_pend(&g_stream_mutex, 0);
    odmp_register_audio_data_cbk(NULL); 
    log_i("[STREAM]g_prebuffer_write_pos=%u marking stream end", g_prebuffer_write_pos);
    g_stream_end = true;
    os_mutex_post(&g_stream_mutex);
}

void wss_stream_reset() {
    log_i("[STREAM]wss_stream_reset");
    wss_stream_stop();
    os_mutex_pend(&g_stream_mutex, 0);
    g_stream_end = true;
    g_prebuffer_write_pos = 0;
    g_prebuffer_read_pos = 0;
    os_mutex_post(&g_stream_mutex);
}

bool wss_stream_on() {
    return g_stream_end == false || _get_readable_size() > 0;
}

void wss_stream_init()
{
    log_i("[STREAM]Init WSS stream\n");
    if (!os_mutex_valid(&g_stream_mutex)) {
        os_mutex_create(&g_stream_mutex);
    }
    g_stream_end = true;
    g_prebuffer_write_pos = 0;
    g_prebuffer_read_pos = 0;
    odmp_register_audio_data_cbk(NULL); 
}

void wss_stream_uninit()
{
    log_i("[STREAM]Uninit WSS stream\n");
    os_mutex_pend(&g_stream_mutex, 0);

    os_mutex_post(&g_stream_mutex);
}
