/**
 * 自定义WebSocket客户端实现
 * 基于RFC 6455标准，使用lwIP socket API
 * 用于替代SDK的WebSocket实现，解决DISCONN误判问题
 */

#include "wss_client_custom.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "lwip/inet.h"
#include "mbedtls/net.h"
#include "mbedtls/ssl.h"
#include "mbedtls/entropy.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/error.h"
#include "mbedtls/certs.h"
#include "mbedtls/mbedtls_config.h"
#include "web_socket/websocket_sha_1.h"
// 注意：不包含 websocket_base64.h，因为我们自己实现了简单的base64编码
// #include "web_socket/websocket_base64.h"
#include "log/log_mqtt.h"
#include "log.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <errno.h>

// 诊断：当传入 lwIP send()/mbedTLS BIO 的 buf 非 4 字节对齐时，打印关键信息用于定位“数据来源/是谁导致不对齐”。
// 注意：这里只做日志，不改变发送行为。
static void ws_hex16(const void *buf, size_t len, char out[3 * 16 + 1])
{
    static const char hex[] = "0123456789abcdef";
    const u8 *p = (const u8 *)buf;
    size_t n = (len > 16) ? 16 : len;
    size_t j = 0;
    for (size_t i = 0; i < n; i++) {
        out[j++] = hex[(p[i] >> 4) & 0xF];
        out[j++] = hex[p[i] & 0xF];
        out[j++] = (i == n - 1) ? '\0' : ' ';
    }
    if (n == 0) out[0] = '\0';
    out[3 * 16] = '\0';
}

static void ws_log_unaligned_send(const char *src, int fd, const void *buf, size_t len,
                                  const char *func, int line)
{
    static u32 log_cnt = 0;
    log_cnt++;
    // 防刷屏：前 8 次必打，之后每 1024 次打一次
    if (log_cnt > 8 && (log_cnt & 1023u) != 0u) {
        return;
    }
    char hex16[3 * 16 + 1];
    ws_hex16(buf, len, hex16);
    log_e("[ws_custom][unalign]src=%s fd=%d ptr=%p align=%u len=%u @%s:%d head16=%s",
          (src ? src : "?"),
          fd,
          buf,
          (unsigned)((uintptr_t)buf & 0x3u),
          (unsigned)len,
          (func ? func : "?"),
          line,
          hex16);
}

#define WS_LOG_UNALIGNED_SEND(_src, _fd, _buf, _len) \
    do { if (((uintptr_t)(_buf) & 0x3u) != 0u) ws_log_unaligned_send((_src), (_fd), (_buf), (_len), __func__, __LINE__); } while (0)

static const char *ws_mbedtls_errstr(int err, char *buf, size_t buflen)
{
    if (buf == NULL || buflen == 0) {
        return "";
    }
    buf[0] = '\0';
#if defined(MBEDTLS_ERROR_C)
    mbedtls_strerror(err, buf, buflen);
#else
    snprintf(buf, buflen, "mbedTLS error (enable MBEDTLS_ERROR_C for details)");
#endif
    return buf;
}

// 声明random32函数（在lwIP中定义）
extern unsigned int random32(int type);

#define WS_MAGIC_KEY "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
#define WS_RECV_BUF_SIZE (32 * 1024)
#define WS_SEND_BUF_SIZE (64 * 1024)
#define WS_RECV_TIMEOUT_MS 5000

// 简单的字节流 RingBuffer
typedef struct ws_ringbuffer {
    u8 *buffer;
    u32 capacity;
    u32 head; // 写索引
    u32 tail; // 读索引
    u32 count; // 数据量
} ws_ringbuffer_t;

static void ws_log_unaligned_rb_send(const char *src, int fd, const ws_ringbuffer_t *rb,
                                     const void *buf, size_t len,
                                     const char *func, int line)
{
    // 在 send_rb 的场景，额外打印 tail/head/count，直接定位“为何 peek 出来就不对齐”
    char hex16[3 * 16 + 1];
    ws_hex16(buf, len, hex16);
    log_e("[ws_custom][unalign_rb]src=%s fd=%d ptr=%p align=%u len=%u rb{cap=%u head=%u tail=%u cnt=%u} @%s:%d head16=%s",
          (src ? src : "?"),
          fd,
          buf,
          (unsigned)((uintptr_t)buf & 0x3u),
          (unsigned)len,
          rb ? (unsigned)rb->capacity : 0u,
          rb ? (unsigned)rb->head : 0u,
          rb ? (unsigned)rb->tail : 0u,
          rb ? (unsigned)rb->count : 0u,
          (func ? func : "?"),
          line,
          hex16);
}

#define WS_LOG_UNALIGNED_RB_SEND(_src, _fd, _rb, _buf, _len) // do { if (((uintptr_t)(_buf) & 0x3u) != 0u) ws_log_unaligned_rb_send((_src), (_fd), (_rb), (_buf), (_len), __func__, __LINE__); } while (0)

// 入队标注：在 rb_write 之前打印“来源/帧信息”，方便把 unalign_rb 和具体发送点关联起来
static void ws_frame_meta(const u8 *frame, u32 frame_len, u8 *opcode, u32 *payload_len)
{
    if (!frame || frame_len < 2) {
        if (opcode) *opcode = 0xFF;
        if (payload_len) *payload_len = 0;
        return;
    }
    if (opcode) *opcode = (u8)(frame[0] & 0x0F);

    u8 len7 = (u8)(frame[1] & 0x7F);
    u32 pl = 0;
    int pos = 2;
    if (len7 < 126) {
        pl = len7;
    } else if (len7 == 126) {
        if (frame_len >= 4) {
            pl = ((u32)frame[2] << 8) | frame[3];
        }
        pos = 4;
    } else { // 127
        if (frame_len >= 10) {
            pl = ((u32)frame[6] << 24) | ((u32)frame[7] << 16) | ((u32)frame[8] << 8) | (u32)frame[9];
        }
        pos = 10;
    }
    // 客户端构建帧强制 MASK=1，因此这里按“有 mask key”推进
    if ((frame[1] & 0x80) && frame_len >= (u32)(pos + 4)) {
        pos += 4;
    }
    if (payload_len) *payload_len = pl;
}

static void rb_init(ws_ringbuffer_t *rb, u8 *buf, u32 cap) {
    rb->buffer = buf;
    rb->capacity = cap;
    rb->head = 0;
    rb->tail = 0;
    rb->count = 0;
}

static u32 rb_space(ws_ringbuffer_t *rb) {
    return rb->capacity - rb->count;
}

static u32 rb_available(ws_ringbuffer_t *rb) {
    return rb->count;
}

// 写入数据
static u32 rb_write(ws_ringbuffer_t *rb, const u8 *data, u32 len) {
    if (len > rb_space(rb)) len = rb_space(rb); // 简单的截断，实际应用中可能需要更复杂的策略
    if (len == 0) return 0;
    
    u32 first_chunk = rb->capacity - rb->head;
    if (len <= first_chunk) {
        memcpy(rb->buffer + rb->head, data, len);
    } else {
        memcpy(rb->buffer + rb->head, data, first_chunk);
        memcpy(rb->buffer, data + first_chunk, len - first_chunk);
    }
    rb->head = (rb->head + len) % rb->capacity;
    rb->count += len;
    return len;
}

// 获取可读的连续内存块（Peek）
static u8* rb_peek(ws_ringbuffer_t *rb, u32 *len) {
    if (rb->count == 0) {
        *len = 0;
        return NULL;
    }
    
    u32 first_chunk = rb->capacity - rb->tail;
    if (rb->count <= first_chunk) {
        *len = rb->count;
    } else {
        *len = first_chunk;
    }
    return rb->buffer + rb->tail;
}

// 消耗数据（移动读指针）
static void rb_advance(ws_ringbuffer_t *rb, u32 len) {
    if (len > rb->count) len = rb->count;
    rb->tail = (rb->tail + len) % rb->capacity;
    rb->count -= len;
}

/**
 * WebSocket客户端内部结构
 */
struct ws_client_handle {
    int sock_fd;                    // TCP socket文件描述符
    mbedtls_ssl_context ssl;       // SSL上下文（WSS使用）
    mbedtls_ssl_config ssl_conf;
    mbedtls_net_context net_ctx;
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context ctr_drbg;
    bool use_ssl;                   // 是否使用SSL (WSS)
    
    ws_state_t state;              // 连接状态
    char host[256];                 // 服务器主机名
    int port;                       // 服务器端口
    char path[512];                 // WebSocket路径
    
    u8 *recv_buf;                  // 接收缓冲区
    u32 recv_buf_len;              // 接收缓冲区已用长度
    
    ws_ringbuffer_t send_rb;       // 发送 RingBuffer
    u8 *send_buf_mem;              // 发送缓冲区内存 (RingBuffer 底层存储)
    
    // 临时发送缓冲区 (用于 ws_client_send_binary 中构建帧，避免在锁内长时间占用 RingBuffer)
    // 或者直接构建进 RingBuffer? 直接构建比较复杂，还是先构建在 temp buf 然后写入 RB
    u8 *temp_send_buf;
    
    ws_recv_callback_t recv_cb;    // 接收回调
    ws_disconnect_callback_t disconnect_cb; // 断开回调
    
    int recv_thread_id;            // IO 线程ID (原接收线程)
    OS_MUTEX mutex;                // 互斥锁 (保护 send_rb 和状态)
    bool thread_running;          // 线程运行标志
};

/**
 * WebSocket帧头结构（RFC 6455）
 */
typedef struct {
    u8 fin_opcode;      // FIN(1bit) + RSV(3bit) + Opcode(4bit)
    u8 mask_len;        // MASK(1bit) + Payload len(7bit)
    u8 ext_len[8];      // 扩展长度字段（0-8字节）
    u8 masking_key[4];  // 掩码密钥（如果MASK=1）
} ws_frame_header_t;

/**
 * 解析WebSocket URL
 */
static int parse_ws_url(const char *url, char *host, int *port, char *path, bool *use_ssl)
{
    const char *p = url;
    
    // 检查协议
    if (strncmp(p, "ws://", 5) == 0) {
        *use_ssl = false;
        p += 5;
    } else if (strncmp(p, "wss://", 6) == 0) {
        *use_ssl = true;
        p += 6;
    } else {
        log_e("[ws_custom]Invalid URL scheme: %s", url);
        return -1;
    }
    
    // 解析主机名和端口
    const char *host_start = p;
    const char *port_start = strchr(p, ':');
    const char *path_start = strchr(p, '/');
    
    if (port_start && (!path_start || port_start < path_start)) {
        // 有端口号
        int host_len = port_start - host_start;
        if (host_len >= 256) return -1;
        memcpy(host, host_start, host_len);
        host[host_len] = '\0';
        
        *port = atoi(port_start + 1);
        if (*port <= 0) *port = (*use_ssl ? 443 : 80);
        
        if (path_start) {
            int path_len = strlen(path_start);
            if (path_len >= 512) return -1;
            strcpy(path, path_start);
        } else {
            strcpy(path, "/");
        }
    } else {
        // 没有端口号，使用默认端口
        if (path_start) {
            int host_len = path_start - host_start;
            if (host_len >= 256) return -1;
            memcpy(host, host_start, host_len);
            host[host_len] = '\0';
            strcpy(path, path_start);
        } else {
            int host_len = strlen(host_start);
            if (host_len >= 256) return -1;
            strcpy(host, host_start);
            strcpy(path, "/");
        }
        *port = (*use_ssl ? 443 : 80);
    }
    
    log_i("[ws_custom]Parsed URL: host=%s, port=%d, path=%s, ssl=%d", 
          host, *port, path, *use_ssl);
    return 0;
}

/**
 * 简单的Base64编码实现（用于WebSocket握手）
 */
static void simple_base64_encode(const u8 *src, int src_len, char *dst, int dst_size)
{
    static const char base64_table[] = 
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    
    int i, j = 0;
    u32 triple;
    
    for (i = 0; i < src_len - 2; i += 3) {
        triple = (src[i] << 16) | (src[i + 1] << 8) | src[i + 2];
        if (j + 4 < dst_size) {
            dst[j++] = base64_table[(triple >> 18) & 0x3F];
            dst[j++] = base64_table[(triple >> 12) & 0x3F];
            dst[j++] = base64_table[(triple >> 6) & 0x3F];
            dst[j++] = base64_table[triple & 0x3F];
        }
    }
    
    if (i < src_len) {
        triple = src[i] << 16;
        if (i + 1 < src_len) {
            triple |= src[i + 1] << 8;
        }
        
        if (j + 4 < dst_size) {
            dst[j++] = base64_table[(triple >> 18) & 0x3F];
            dst[j++] = base64_table[(triple >> 12) & 0x3F];
            if (i + 1 < src_len) {
                dst[j++] = base64_table[(triple >> 6) & 0x3F];
            } else {
                dst[j++] = '=';
            }
            dst[j++] = '=';
        }
    }
    
    if (j < dst_size) {
        dst[j] = '\0';
    } else if (dst_size > 0) {
        dst[dst_size - 1] = '\0';
    }
}

/**
 * 生成WebSocket握手请求
 */
static int generate_handshake_request(const char *host, int port, const char *path,
                                     char *key, char *request, int request_size)
{
    // 生成随机密钥
    u32 random_val = random32(0);
    u8 key_bytes[16];
    memcpy(key_bytes, &random_val, 4);
    random_val = random32(0);
    memcpy(key_bytes + 4, &random_val, 4);
    random_val = random32(0);
    memcpy(key_bytes + 8, &random_val, 4);
    random_val = random32(0);
    memcpy(key_bytes + 12, &random_val, 4);
    
    // 使用简单的base64编码
    simple_base64_encode(key_bytes, 16, key, 32);
    
    // 生成握手请求
    // 注意：
    // - 公网域名/WSS 常在反向代理/CDN 后面，Origin/Host 不规范可能导致对端主动断开（常见表现：客户端 TLS fatal alert、服务端 WS 1006 unexpected EOF）
    char host_line[300];
    if (port == 80 || port == 443) {
        snprintf(host_line, sizeof(host_line), "Host: %s\r\n", host);
    } else {
        snprintf(host_line, sizeof(host_line), "Host: %s:%d\r\n", host, port);
    }

    int len = snprintf(request, request_size,
        "GET %s HTTP/1.1\r\n"
        "%s"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: %s\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        // 不要使用 localhost；对于 wss 服务器，通常期望 https Origin（若服务端不校验 Origin，也不会受影响）
        "Origin: https://%s\r\n"
        "\r\n",
        path,
        host_line,
        key,
        host);
    
    if (len < 0 || len >= request_size) {
        log_e("[ws_custom]Handshake request too long");
        return -1;
    }
    
    return len;
}

/**
 * 验证WebSocket握手响应
 */
static int verify_handshake_response(const char *response, int response_len, const char *expected_key)
{
    // 添加调试日志：打印实际收到的响应（前200字节）
    int log_len = (response_len < 200) ? response_len : 200;
    log_i("[ws_custom]Received handshake response (%d bytes, first %d):", response_len, log_len);
    
    // 检查HTTP状态码（更灵活的方式：查找 " 101 " 或 " 101\r"）
    const char *status_code = strstr(response, " 101 ");
    if (status_code == NULL) {
        status_code = strstr(response, " 101\r");
    }
    if (status_code == NULL) {
        status_code = strstr(response, " 101\n");
    }
    
    if (status_code == NULL || status_code - response > 20) {
        // 打印实际的状态行（第一行）
        char status_line[128];
        int i = 0;
        while (i < response_len && i < sizeof(status_line) - 1 && response[i] != '\r' && response[i] != '\n') {
            status_line[i] = response[i];
            i++;
        }
        status_line[i] = '\0';
        log_e("[ws_custom]Invalid HTTP status in handshake. Expected status code 101, got: '%s'", status_line);
        return -1;
    }
    
    // 验证前面是HTTP版本
    if (status_code - response < 8) {
        char status_line[128];
        int i = 0;
        while (i < response_len && i < sizeof(status_line) - 1 && response[i] != '\r' && response[i] != '\n') {
            status_line[i] = response[i];
            i++;
        }
        status_line[i] = '\0';
        log_e("[ws_custom]Invalid HTTP version in status line: '%s'", status_line);
        return -1;
    }
    
    // 检查HTTP版本（允许前面有空白字符）
    const char *http_version = status_code - 8;
    if (strncmp(http_version, "HTTP/1.1", 8) != 0 &&
        strncmp(http_version, "HTTP/1.0", 8) != 0) {
        // 尝试向前查找HTTP版本
        int found = 0;
        for (int i = 0; i < 5 && http_version - i >= response; i++) {
            if (strncmp(http_version - i, "HTTP/1.1", 8) == 0 ||
                strncmp(http_version - i, "HTTP/1.0", 8) == 0) {
                found = 1;
                break;
            }
        }
        if (!found) {
            char status_line[128];
            int i = 0;
            while (i < response_len && i < sizeof(status_line) - 1 && response[i] != '\r' && response[i] != '\n') {
                status_line[i] = response[i];
                i++;
            }
            status_line[i] = '\0';
            log_e("[ws_custom]Invalid HTTP version in status line: '%s'", status_line);
            return -1;
        }
    }
    
    // 检查Upgrade头
    if (strstr(response, "Upgrade: websocket") == NULL &&
        strstr(response, "upgrade: websocket") == NULL) {
        log_e("[ws_custom]Missing Upgrade header");
        return -1;
    }
    
    // 检查Connection头
     if (strstr(response, "Connection: Upgrade") == NULL &&
        strstr(response, "connection: upgrade") == NULL &&
        strstr(response, "Connection: upgrade") == NULL) {
        log_e("[ws_custom]Missing Connection header. Headers received:\n%.*s", response_len, response); // 增加了响应内容的打印
        return -1;
    }
    
    // 计算期望的Accept值
    char accept_key[64];
    char combined[128];
    snprintf(combined, sizeof(combined), "%s%s", expected_key, WS_MAGIC_KEY);
    
    // 使用SDK的SHA1实现
    SHA1Context ctx;
    SHA1Reset(&ctx);
    SHA1Input(&ctx, combined, strlen(combined));
    SHA1Result(&ctx);
    
    // SHA1Result会将结果存储在ctx.Message_Digest中（5个32位整数=20字节）
    u8 sha1_hash[20];
    for (int i = 0; i < 5; i++) {
        sha1_hash[i * 4 + 0] = (ctx.Message_Digest[i] >> 24) & 0xFF;
        sha1_hash[i * 4 + 1] = (ctx.Message_Digest[i] >> 16) & 0xFF;
        sha1_hash[i * 4 + 2] = (ctx.Message_Digest[i] >> 8) & 0xFF;
        sha1_hash[i * 4 + 3] = ctx.Message_Digest[i] & 0xFF;
    }
    
    // 使用简单的base64编码
    simple_base64_encode(sha1_hash, 20, accept_key, sizeof(accept_key));
    
    // 检查Sec-WebSocket-Accept头
    char accept_header[128];
    snprintf(accept_header, sizeof(accept_header), "Sec-WebSocket-Accept: %s", accept_key);
    
    if (strstr(response, accept_header) == NULL) {
        // 尝试不区分大小写
        char *accept_pos = strstr(response, "Sec-WebSocket-Accept:");
        if (accept_pos == NULL) {
            accept_pos = strstr(response, "sec-websocket-accept:");
        }
        if (accept_pos) {
            accept_pos += strlen("Sec-WebSocket-Accept:");
            while (*accept_pos == ' ' || *accept_pos == '\t') accept_pos++;
            char *end = strchr(accept_pos, '\r');
            if (end) {
                int len = end - accept_pos;
                if (len < sizeof(accept_key)) {
                    char received_key[64];
                    memcpy(received_key, accept_pos, len);
                    received_key[len] = '\0';
                    if (strcmp(received_key, accept_key) != 0) {
                        log_e("[ws_custom]Invalid Sec-WebSocket-Accept: expected=%s, got=%s", 
                              accept_key, received_key);
                        return -1;
                    }
                }
            }
        } else {
            log_e("[ws_custom]Missing Sec-WebSocket-Accept header");
            return -1;
        }
    }
    
    return 0;
}

/**
 * 读取HTTP响应（用于握手）
 */
static int read_http_response(int fd, char *buf, int buf_size, int timeout_ms)
{
    int total_read = 0;
    int header_end = -1;
    
    while (total_read < buf_size - 1) {
        fd_set read_fds;
        struct timeval tv;
        FD_ZERO(&read_fds);
        FD_SET(fd, &read_fds);
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
        
        int ret = select(fd + 1, &read_fds, NULL, NULL, &tv);
        if (ret <= 0) {
            log_e("[ws_custom]Select timeout or error");
            return -1;
        }
        
        int n = recv(fd, buf + total_read, buf_size - total_read - 1, 0);
        if (n <= 0) {
            log_e("[ws_custom]Recv error: %d", n);
            return -1;
        }
        
        total_read += n;
        buf[total_read] = '\0';
        
        // 检查是否收到完整的HTTP头（以\r\n\r\n结尾）
        if (header_end < 0) {
            char *end = strstr(buf, "\r\n\r\n");
            if (end) {
                header_end = end - buf + 4;
                // HTTP头已完整，但可能还有body，继续读取直到连接关闭或超时
                // 对于WebSocket握手，通常只有头，没有body
                break;
            }
        }
    }
    
    return total_read;
}

// 自定义 mbedTLS BIO 回调
static int my_net_send(void *ctx, const unsigned char *buf, size_t len) {
    int fd = *(int *)ctx;
    if (fd < 0) return MBEDTLS_ERR_NET_INVALID_CONTEXT;

    WS_LOG_UNALIGNED_SEND("mbedtls_bio_send", fd, buf, len);
    int ret = send(fd, buf, len, 0);
    if (ret < 0) {
        int errno_snapshot = errno;
        if (errno == EWOULDBLOCK || errno == EAGAIN) return MBEDTLS_ERR_SSL_WANT_WRITE;
        if (errno == EPIPE || errno == ECONNRESET) return MBEDTLS_ERR_NET_CONN_RESET;
        if (errno == EINTR) return MBEDTLS_ERR_SSL_WANT_WRITE;
        if (errno == 128) {  // pdFREERTOS_ERRNO_ENOTCONN - Socket is not connected
            log_e("[ws_custom]my_net_send: Socket is not connected (errno=128)");
            return MBEDTLS_ERR_NET_CONN_RESET;
        }
        // 走到这里就是最终会返回 NET_SEND_FAILED（-0x004E / -78）
        // 打印 errno + SO_ERROR，方便定位底层失败原因（断链/资源不足/非法指针等）
        int so_error = 0;
        socklen_t so_len = sizeof(so_error);
        int gs_ret = getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_error, &so_len);
        log_e("[ws_custom]my_net_send: NET_SEND_FAILED, fd=%d, errno=%d, getsockopt_ret=%d, so_error=%d, len=%u",
              fd, errno_snapshot, gs_ret, so_error, (unsigned)len);
        return MBEDTLS_ERR_NET_SEND_FAILED;
    }
    return ret;
}

static int my_net_recv(void *ctx, unsigned char *buf, size_t len) {
    int fd = *(int *)ctx;
    if (fd < 0) return MBEDTLS_ERR_NET_INVALID_CONTEXT;

    int ret = recv(fd, buf, len, 0);
    if (ret < 0) {
        if (errno == EWOULDBLOCK || errno == EAGAIN) return MBEDTLS_ERR_SSL_WANT_READ;
        if (errno == EPIPE || errno == ECONNRESET) return MBEDTLS_ERR_NET_CONN_RESET;
        if (errno == EINTR) return MBEDTLS_ERR_SSL_WANT_READ;
        if (errno == 128) {  // pdFREERTOS_ERRNO_ENOTCONN - Socket is not connected
            log_e("[ws_custom]my_net_recv: Socket is not connected (errno=128)");
            return MBEDTLS_ERR_NET_CONN_RESET;  // 直接返回连接重置，避免无效重试
        }
        
        // 只将致命错误返回 RECV_FAILED，其他错误返回 WANT_READ 让上层重试
        if (errno == EBADF || errno == EFAULT) {
            log_e("[ws_custom]my_net_recv failed, errno=%d", errno);
            return MBEDTLS_ERR_NET_RECV_FAILED;
        }
        
        return MBEDTLS_ERR_SSL_WANT_READ;
    }
    return ret;
}

/**
 * 建立原始TCP连接
 */
static int raw_tcp_connect(const char *host, int port) {
    struct sockaddr_in server_addr;
    struct hostent *server;
    int sock_fd = -1;
    
    // 解析主机名
    server = gethostbyname(host);
    if (server == NULL) {
        log_e("[ws_custom]Failed to resolve hostname: %s", host);
        return -1;
    }
    
    // 创建socket
    sock_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (sock_fd < 0) {
        log_e("[ws_custom]Failed to create socket");
        return -1;
    }
    
    // 设置socket选项
    int opt = 1;
    setsockopt(sock_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    
    // 设置接收/发送超时 (握手阶段使用)
    struct timeval tv;
    tv.tv_sec = WS_RECV_TIMEOUT_MS / 1000;
    tv.tv_usec = (WS_RECV_TIMEOUT_MS % 1000) * 1000;
    setsockopt(sock_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock_fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    
    // 连接服务器
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    memcpy(&server_addr.sin_addr.s_addr, server->h_addr, server->h_length);
    server_addr.sin_port = htons(port);
    
    if (connect(sock_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        log_e("[ws_custom]Failed to connect to %s:%d", host, port);
        close(sock_fd);
        return -1;
    }
    
    return sock_fd;
}

/**
 * 执行WebSocket握手（普通TCP）
 */
static int do_tcp_handshake(ws_client_t client, const char *host, int port, const char *path)
{
    // 连接TCP
    client->sock_fd = raw_tcp_connect(host, port);
    if (client->sock_fd < 0) return -1;
    
    log_i("[ws_custom]TCP connected to %s:%d", host, port);

    // 使用 heap 上的 client->temp_send_buf 和 client->recv_buf 替代栈上的大数组，防止 stack overflow
    char *handshake_req = (char *)client->temp_send_buf;
    char *response = (char *)client->recv_buf;
    int handshake_req_size = 1024; // temp_send_buf is 16KB
    int response_size = 2048;      // recv_buf is 32KB
    
    // 生成握手请求
    char ws_key[32];
    int req_len = generate_handshake_request(host, port, path, ws_key, handshake_req, handshake_req_size);
    if (req_len < 0) {
        close(client->sock_fd);
        client->sock_fd = -1;
        return -1;
    }
    
    // 发送握手请求
    WS_LOG_UNALIGNED_SEND("tcp_handshake_req", client->sock_fd, handshake_req, (size_t)req_len);
    int sent = send(client->sock_fd, handshake_req, req_len, 0);
    if (sent != req_len) {
        log_e("[ws_custom]Failed to send handshake request: sent=%d, expected=%d", sent, req_len);
        close(client->sock_fd);
        client->sock_fd = -1;
        return -1;
    }
    
    // 读取握手响应
    int resp_len = read_http_response(client->sock_fd, response, response_size, WS_RECV_TIMEOUT_MS);
    if (resp_len < 0) {
        log_e("[ws_custom]Failed to read handshake response");
        close(client->sock_fd);
        client->sock_fd = -1;
        return -1;
    }
    
    // 添加调试日志
    log_i("[ws_custom]Handshake response received, length=%d", resp_len);
    
    // 验证握手响应
    if (verify_handshake_response(response, resp_len, ws_key) < 0) {
        log_e("[ws_custom]Handshake verification failed");
        close(client->sock_fd);
        client->sock_fd = -1;
        return -1;
    }
    
    // 握手成功后，设置Socket为非阻塞模式
    int flags = fcntl(client->sock_fd, F_GETFL, 0);
    fcntl(client->sock_fd, F_SETFL, flags | O_NONBLOCK);
    
    log_i("🤝 [ws_custom]WebSocket handshake successful (Non-blocking set)");
    return 0;
}

/**
 * 执行WebSocket握手（SSL/TLS）
 */
static int do_ssl_handshake(ws_client_t client, const char *host, int port, const char *path)
{
    int ret;
    enum { WS_SSL_HS_IO_TMP_SIZE = 512 };
    // 4 字节对齐的握手 IO 临时缓冲（避免 response+resp_len / req+sent 非对齐导致 misalign）
    u32 hs_io_tmp_word[(WS_SSL_HS_IO_TMP_SIZE + 3) / 4];
    unsigned char *hs_io_tmp = (unsigned char *)hs_io_tmp_word;
    
    // 初始化mbedTLS
    mbedtls_ssl_init(&client->ssl);
    mbedtls_ssl_config_init(&client->ssl_conf);
    mbedtls_net_init(&client->net_ctx);
    mbedtls_entropy_init(&client->entropy);
    mbedtls_ctr_drbg_init(&client->ctr_drbg);
    
    // 初始化随机数生成器
    const char *pers = "websocket_client";
    ret = mbedtls_ctr_drbg_seed(&client->ctr_drbg, mbedtls_entropy_func, &client->entropy,
                                (const unsigned char *)pers, strlen(pers));
    if (ret != 0) {
        char ebuf[128];
        log_e("[ws_custom]mbedtls_ctr_drbg_seed failed: -0x%04x (%s)", -ret,
              ws_mbedtls_errstr(ret, ebuf, sizeof(ebuf)));
        goto cleanup;
    }
    
    // 配置SSL
    ret = mbedtls_ssl_config_defaults(&client->ssl_conf,
                                      MBEDTLS_SSL_IS_CLIENT,
                                      MBEDTLS_SSL_TRANSPORT_STREAM,
                                      MBEDTLS_SSL_PRESET_DEFAULT);
    if (ret != 0) {
        char ebuf[128];
        log_e("[ws_custom]mbedtls_ssl_config_defaults failed: -0x%04x (%s)", -ret,
              ws_mbedtls_errstr(ret, ebuf, sizeof(ebuf)));
        goto cleanup;
    }
    
    mbedtls_ssl_conf_authmode(&client->ssl_conf, MBEDTLS_SSL_VERIFY_NONE);
    mbedtls_ssl_conf_rng(&client->ssl_conf, mbedtls_ctr_drbg_random, &client->ctr_drbg);
    
    // 设置CA证书（如果需要验证）
    // mbedtls_ssl_conf_ca_chain(&client->ssl_conf, &cacert, NULL);
    
    // 设置主机名
    ret = mbedtls_ssl_set_hostname(&client->ssl, host);
    if (ret != 0) {
        char ebuf[128];
        log_e("[ws_custom]mbedtls_ssl_set_hostname failed: -0x%04x (%s)", -ret,
              ws_mbedtls_errstr(ret, ebuf, sizeof(ebuf)));
        goto cleanup;
    }
    
    // 初始化SSL上下文
    ret = mbedtls_ssl_setup(&client->ssl, &client->ssl_conf);
    if (ret != 0) {
        char ebuf[128];
        log_e("[ws_custom]mbedtls_ssl_setup failed: -0x%04x (%s)", -ret,
              ws_mbedtls_errstr(ret, ebuf, sizeof(ebuf)));
        goto cleanup;
    }
    
    // 连接TCP (使用 raw_tcp_connect)
    client->sock_fd = raw_tcp_connect(host, port);
    if (client->sock_fd < 0) {
        ret = -1;
        goto cleanup;
    }
    
    log_i("[ws_custom]TCP connected to %s:%d (SSL)", host, port);
    
    // 设置bio回调 (使用 client->sock_fd 而不是 net_ctx)
    mbedtls_ssl_set_bio(&client->ssl, &client->sock_fd, my_net_send, my_net_recv, NULL);
    
    // 执行SSL握手
    while ((ret = mbedtls_ssl_handshake(&client->ssl)) != 0) {
        if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
            char ebuf[128];
            log_e("[ws_custom]mbedtls_ssl_handshake failed: -0x%04x (%s)", -ret,
                  ws_mbedtls_errstr(ret, ebuf, sizeof(ebuf)));
            goto cleanup;
        }
    }
    
    log_i("[ws_custom]SSL handshake successful");
    
    // 生成WebSocket握手请求
    char ws_key[32];
    char handshake_req[1024];
    int req_len = generate_handshake_request(host, port, path, ws_key, handshake_req, sizeof(handshake_req));
    if (req_len < 0) {
        ret = -1;
        goto cleanup;
    }
    
    // 通过SSL发送握手请求
    int sent = 0;
    while (sent < req_len) {
        unsigned char *p = (unsigned char *)handshake_req + sent;
        int wlen = req_len - sent;
        if (((uintptr_t)p & 0x3u) != 0u) {
            if (wlen > (int)WS_SSL_HS_IO_TMP_SIZE) wlen = (int)WS_SSL_HS_IO_TMP_SIZE;
            memcpy(hs_io_tmp, p, wlen);
            p = hs_io_tmp;
        }
        ret = mbedtls_ssl_write(&client->ssl, p, wlen);
        if (ret <= 0) {
            if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
                char ebuf[128];
                log_e("[ws_custom]mbedtls_ssl_write failed: -0x%04x (%s)", -ret,
                      ws_mbedtls_errstr(ret, ebuf, sizeof(ebuf)));
                goto cleanup;
            }
            continue;
        }
        sent += ret;
    }
    
    // 读取握手响应
    char response[2048];
    int resp_len = 0;
    while (resp_len < (int)sizeof(response) - 1) {
        int rcap = (int)sizeof(response) - resp_len - 1;
        int rlen = rcap;
        if (rlen > (int)WS_SSL_HS_IO_TMP_SIZE) rlen = (int)WS_SSL_HS_IO_TMP_SIZE;
        ret = mbedtls_ssl_read(&client->ssl, hs_io_tmp, rlen);
        if (ret <= 0) {
            if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE) {
                continue;
            }
            {
                char ebuf[128];
                log_e("[ws_custom]mbedtls_ssl_read failed: -0x%04x (%s)", -ret,
                      ws_mbedtls_errstr(ret, ebuf, sizeof(ebuf)));
            }
            goto cleanup;
        }
        if (ret > rcap) ret = rcap; // 防御：不应发生
        memcpy((unsigned char *)response + resp_len, hs_io_tmp, ret);
        resp_len += ret;
        response[resp_len] = '\0';
        
        // 检查是否收到完整的HTTP头
        if (strstr(response, "\r\n\r\n") != NULL) {
            break;
        }
    }
    
    // 验证握手响应
    if (verify_handshake_response(response, resp_len, ws_key) < 0) {
        log_e("[ws_custom]Handshake verification failed");
        ret = -1;
        goto cleanup;
    }
    
    // 握手成功后，设置Socket为非阻塞模式
    int flags = fcntl(client->sock_fd, F_GETFL, 0);
    fcntl(client->sock_fd, F_SETFL, flags | O_NONBLOCK);
    
    log_i("[ws_custom]WebSocket handshake successful (SSL, Non-blocking set)");
    return 0;
    
cleanup:
    if (client->sock_fd >= 0) {
        close(client->sock_fd);
        client->sock_fd = -1;
    }
    // mbedtls cleanup
    mbedtls_ssl_free(&client->ssl);
    mbedtls_ssl_config_free(&client->ssl_conf);
    mbedtls_ctr_drbg_free(&client->ctr_drbg);
    mbedtls_entropy_free(&client->entropy);
    // net_ctx 不再使用
    return -1;
}

/**
 * 验证WebSocket帧头（检查RSV位、MASK位和opcode）
 * @param buf 帧缓冲区
 * @param frame_len 帧长度
 * @return 0表示有效，-1表示无效
 */
static int validate_ws_frame_header(const u8 *buf, int frame_len)
{
    if (frame_len < 2) {
        log_e("[ws_custom]ERROR: Frame too short: %d bytes", frame_len);
        return -1;
    }
    
    u8 first_byte = buf[0];
    u8 second_byte = buf[1];
    u8 opcode = first_byte & 0x0F;
    bool fin = (first_byte & 0x80) != 0;
    bool rsv1 = (first_byte & 0x40) != 0;
    bool rsv2 = (first_byte & 0x20) != 0;
    bool rsv3 = (first_byte & 0x10) != 0;
    bool mask = (second_byte & 0x80) != 0;
    
    // 检查RSV位（必须为0）
    if (rsv1 || rsv2 || rsv3) {
        log_e("[ws_custom]ERROR: RSV bits set! first_byte=0x%02x, RSV1=%d, RSV2=%d, RSV3=%d, opcode=0x%02x",
              first_byte, rsv1, rsv2, rsv3, opcode);
        // 打印帧头的前16字节用于调试
        log_e("[ws_custom]Frame header (first 16 bytes):");
        int print_len = (frame_len < 16) ? frame_len : 16;
        for (int i = 0; i < print_len; i++) {
            log_e("  [%d] = 0x%02x", i, buf[i]);
        }
        return -1;
    }
    
    // 检查MASK位（客户端必须设置）
    if (!mask) {
        log_e("[ws_custom]ERROR: MASK bit not set! second_byte=0x%02x, opcode=0x%02x", 
              second_byte, opcode);
        return -1;
    }
    
    // 检查opcode的有效性
    if (opcode > 0xA || (opcode >= 0x3 && opcode <= 0x7)) {
        log_e("[ws_custom]ERROR: Invalid opcode! opcode=0x%02x, first_byte=0x%02x", 
              opcode, first_byte);
        return -1;
    }
    
    log_d("[ws_custom]Frame validated: opcode=0x%02x, FIN=%d, RSV=0, MASK=1, frame_len=%d",
          opcode, fin, frame_len);
    
    return 0;
}

/**
 * 构建WebSocket帧（RFC 6455）
 */
static int build_ws_frame(u8 *buf, int buf_size, const u8 *payload, u32 payload_len,
                         ws_frame_type_t frame_type, bool mask)
{
    if (buf_size < 14) return -1; // 最小帧头大小
    
    // 验证frame_type的有效性（RFC 6455允许的opcode值）
    u8 opcode = (u8)(frame_type & 0x0F);
    // 有效的opcode: 0x0(continuation), 0x1(text), 0x2(binary), 0x8(close), 0x9(ping), 0xA(pong)
    // 0x3-0x7和0xB-0xF是保留值，不允许使用
    if (opcode > 0xA || (opcode >= 0x3 && opcode <= 0x7)) {
        log_e("[ws_custom]ERROR: Invalid opcode 0x%02x (frame_type=0x%02x)", opcode, (u8)frame_type);
        return -1; // 拒绝无效的opcode
    }
    
    // 初始化缓冲区，避免内存污染
    memset(buf, 0, buf_size < 14 ? buf_size : 14);
    
    int pos = 0;
    
    // FIN + Opcode（显式清除RSV位）
    // 第一个字节格式：FIN(bit7) RSV1(bit6) RSV2(bit5) RSV3(bit4) Opcode(bit0-3)
    // RSV位必须为0，所以只设置FIN位(0x80)和Opcode(低4位)
    u8 first_byte = 0x80 | opcode;  // FIN=1, RSV=0, Opcode=frame_type的低4位
    
    // 严格检查RSV位是否被错误设置
    if ((first_byte & 0x70) != 0) {
        log_e("[ws_custom]ERROR: RSV bits are set! first_byte=0x%02x, frame_type=0x%02x, opcode=0x%02x", 
              first_byte, (u8)frame_type, opcode);
        // 强制清除RSV位
        first_byte = 0x80 | opcode;
        log_e("[ws_custom]Fixed: first_byte=0x%02x", first_byte);
    }
    
    buf[pos++] = first_byte;
    
    // MASK + Payload length（客户端必须mask，强制设置MASK=1）
    u32 len_field;
    u8 second_byte;
    if (payload_len < 126) {
        len_field = payload_len;
        second_byte = 0x80 | len_field;  // 强制设置MASK=1（客户端必须mask）
        buf[pos++] = second_byte;
    } else if (payload_len < 65536) {
        len_field = 126;
        second_byte = 0x80 | len_field;  // 强制设置MASK=1
        buf[pos++] = second_byte;
        buf[pos++] = (payload_len >> 8) & 0xFF;
        buf[pos++] = payload_len & 0xFF;
    } else {
        len_field = 127;
        second_byte = 0x80 | len_field;  // 强制设置MASK=1
        buf[pos++] = second_byte;
        buf[pos++] = 0; // 64位长度，高32位为0
        buf[pos++] = 0;
        buf[pos++] = 0;
        buf[pos++] = 0;
        buf[pos++] = (payload_len >> 24) & 0xFF;
        buf[pos++] = (payload_len >> 16) & 0xFF;
        buf[pos++] = (payload_len >> 8) & 0xFF;
        buf[pos++] = payload_len & 0xFF;
    }
    
    // 验证MASK位是否正确设置（客户端必须mask）
    if ((second_byte & 0x80) == 0) {
        log_e("[ws_custom]ERROR: MASK bit not set! second_byte=0x%02x, mask=%d", second_byte, mask);
        return -1;
    }
    
    // Masking key（客户端必须mask）
    if (mask) {
        // 关键：确保 payload 写入不会溢出 buf
        // pos 此时是“帧头(含扩展长度)已写入后的偏移”，接下来还会写入 4 字节 masking key 和 payload。
        // 由于下面会先写入 masking key（pos 会自增 4），我们在写 payload 前再次检查一次即可。
        u32 mask_key = random32(0);
        // 保存masking key的位置
        u8 *mask_bytes = buf + pos;
        // 使用明确的字节序，避免memcpy的字节序问题
        buf[pos++] = (mask_key >> 0) & 0xFF;
        buf[pos++] = (mask_key >> 8) & 0xFF;
        buf[pos++] = (mask_key >> 16) & 0xFF;
        buf[pos++] = (mask_key >> 24) & 0xFF;
        // 注意：pos已经通过上面的4个pos++增加了4，不需要再加4

        // 再次检查：pos 已包含 masking key；保证 pos + payload_len <= buf_size
        if ((u32)pos + payload_len > (u32)buf_size) {
            log_e("[ws_custom]ERROR: build_ws_frame overflow prevented. buf_size=%d header+mask=%d payload_len=%u opcode=0x%02x",
                  buf_size, pos, payload_len, opcode);
            return -1;
        }
        
        // 应用掩码
        if (payload_len > 0 && payload != NULL) {
            for (u32 i = 0; i < payload_len; i++) {
                buf[pos + i] = payload[i] ^ mask_bytes[i % 4];
            }
        }
    } else {
        // 客户端不应该发送未mask的帧，这是一个错误
        log_e("[ws_custom]ERROR: Client must mask frames! mask=%d", mask);
        return -1;
    }
    
    // 发送前最终验证：检查帧头的RSV位和MASK位
    int frame_total_len = pos + payload_len;
    if (validate_ws_frame_header(buf, frame_total_len) < 0) {
        log_e("[ws_custom]ERROR: Frame validation failed in build_ws_frame");
        return -1;
    }
    
    return frame_total_len;
}

/**
 * 解析WebSocket帧（RFC 6455）
 */
static int parse_ws_frame(const u8 *data, int data_len, u8 **payload, u32 *payload_len,
                         ws_frame_type_t *frame_type, bool *is_fin)
{
    // 判断数据长度是否足够解析
    if (data_len < 2) return 0;
    
    // 解析第一个字节
    *is_fin = (data[0] & 0x80) != 0;
    u8 opcode = data[0] & 0x0F;
    *frame_type = (ws_frame_type_t)opcode;
    
    // 解析第二个字节
    bool masked = (data[1] & 0x80) != 0;
    u8 len_field = data[1] & 0x7F;
    
    int header_len = 2;
    u64 payload_length = 0;
    
    if (len_field < 126) {
        payload_length = len_field;
    } else if (len_field == 126) {
        if (data_len < 4) return 0;
        payload_length = ((u64)data[2] << 8) | data[3];
        header_len = 4;
    } else if (len_field == 127) {
        if (data_len < 10) return 0;
        payload_length = ((u64)data[2] << 56) | ((u64)data[3] << 48) |
                        ((u64)data[4] << 40) | ((u64)data[5] << 32) |
                        ((u64)data[6] << 24) | ((u64)data[7] << 16) |
                        ((u64)data[8] << 8) | data[9];
        header_len = 10;
    }
    
    // 检查掩码
    u8 masking_key[4] = {0};
    if (masked) {
        if (data_len < header_len + 4) return 0;
        memcpy(masking_key, data + header_len, 4);
        header_len += 4;
    }
    
    // 检查是否有足够的数据
    if (data_len < header_len + payload_length) {
        return 0; // 数据不完整，需要更多数据
    }
    
    // 提取payload
    *payload = (u8 *)(data + header_len);
    *payload_len = payload_length;
    
    // 如果被掩码，需要解掩码
    if (masked && payload_length > 0) {
        for (u32 i = 0; i < payload_length; i++) {
            (*payload)[i] ^= masking_key[i % 4];
        }
    }
    
    return header_len + payload_length; // 返回总帧长度
}

/**
 * WebSocket接收线程
 */
static void ws_recv_thread(void *param)
{
    ws_client_t client = (ws_client_t)param;
    u8 *frame_buf = malloc(WS_RECV_BUF_SIZE);

    // 临时缓冲区用于构建自动响应 (PING/CLOSE)
    u8 *resp_buf = malloc(1024);

    // SSL/TLS 场景下的对齐 bounce buffer：
    // WL82(AC791N) 上部分库/优化路径可能做 32-bit 访问，若传入/写入指针非 4 字节对齐会触发 misalign_err。
    // RingBuffer 的 tail 会导致 peek 指针不对齐；recv_buf_len 也会导致 read 目标地址不对齐。
    // 因此统一用对齐临时缓冲区做 ssl_read/ssl_write，再 memcpy 到业务缓冲区。
    enum { WS_SSL_IO_TMP_SIZE = 2048 };
    u8 *ssl_io_tmp_mem = malloc(WS_SSL_IO_TMP_SIZE + 8);
    u8 *ssl_io_tmp = NULL;

    // 退出原因（传给上层，便于判断是服务端 CLOSE / EOF / 错误）
    char exit_reason[96] = "io_thread_exit";

    if (!frame_buf || !resp_buf || !ssl_io_tmp_mem) {
        log_e("[ws_custom]Failed to allocate buffers");
        client->thread_running = false;
        strncpy(exit_reason, "alloc_failed", sizeof(exit_reason) - 1);
        exit_reason[sizeof(exit_reason) - 1] = '\0';
        if (frame_buf) free(frame_buf);
        if (resp_buf) free(resp_buf);
        if (ssl_io_tmp_mem) free(ssl_io_tmp_mem);
        return;
    }
    ssl_io_tmp = (u8 *)(((uintptr_t)ssl_io_tmp_mem + 3u) & ~(uintptr_t)3u);

    int ssl_read_error_count = 0;
    const int MAX_SSL_READ_RETRIES = 3;
    const int SSL_READ_RETRY_DELAY = 100; // 100ms

    // P2: TLS耦合观测（避免刷屏：只在“卡住”时打印一次，读写恢复后复位）
    bool logged_read_want_write = false;
    bool logged_write_want_read = false;

    // P3: SSL场景下强制监听写事件（避免TLS read->WANT_WRITE 卡住）
    bool logged_ssl_watch_write = false;
    bool ssl_need_write = false; // ssl_read 返回 WANT_WRITE 时置位：下一轮在“可写事件”上重试 ssl_read

    log_i("[ws_custom]IO thread started (Non-blocking)");

    int send_fatal_err_count = 0;

    while (client->thread_running && (client->state == WS_STATE_CONNECTED || client->state == WS_STATE_CLOSING)) {
        fd_set read_fds, write_fds;
        FD_ZERO(&read_fds);
        FD_ZERO(&write_fds);
        
        int fd = client->sock_fd; // 统一使用 sock_fd
        
        if (fd < 0) break;
        
        FD_SET(fd, &read_fds);
        
        // 检查是否有数据需要发送
        os_mutex_pend(&client->mutex, 0);
        bool has_data_to_send = (rb_available(&client->send_rb) > 0);
        os_mutex_post(&client->mutex);

        // SSL/TLS 读写耦合：只有在“确实需要写(ssl_read WANT_WRITE)”或“应用层有待发数据”时才监听 write_fds。
        // 否则 TCP socket 通常一直处于“可写”状态，会导致 select 立即返回 -> IO 线程空转抢占 CPU，最终触发 WDT。
        if (has_data_to_send || (client->use_ssl && ssl_need_write)) {
            FD_SET(fd, &write_fds);
            if (client->use_ssl && ssl_need_write && !logged_ssl_watch_write) {
                log_mqtt("【debug】[ws_custom]select watch_write enabled (ssl_need_write), fd=%d", fd);
                logged_ssl_watch_write = true;
            }
        }
        
        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 50 * 1000; // 50ms 轮询
        
        int ret = select(fd + 1, &read_fds, &write_fds, NULL, &tv);
        
        if (ret < 0) {
            log_e("[ws_custom]Select error: %d", errno);
            snprintf(exit_reason, sizeof(exit_reason), "select_error errno=%d", errno);
            break;
        }
        
        // 处理写事件
        if (ret > 0 && FD_ISSET(fd, &write_fds)) {
            os_mutex_pend(&client->mutex, 0);
            u32 len = 0;
            u8 *data = rb_peek(&client->send_rb, &len);
            
            if (data && len > 0) {
                int sent = 0;
                if (client->use_ssl) {
                    // SSL 写：避免把非 4 字节对齐的 RingBuffer 指针直接传入 mbedTLS
                    const u8 *write_ptr = data;
                    u32 write_len = len;
                    WS_LOG_UNALIGNED_RB_SEND("ws_io_ssl_write", client->sock_fd, &client->send_rb, write_ptr, write_len);
                    if (((uintptr_t)write_ptr & 0x3u) != 0u) {
                        write_len = (write_len > (u32)WS_SSL_IO_TMP_SIZE) ? (u32)WS_SSL_IO_TMP_SIZE : write_len;
                        memcpy(ssl_io_tmp, write_ptr, write_len);
                        write_ptr = ssl_io_tmp;
                    }
                    sent = mbedtls_ssl_write(&client->ssl, write_ptr, write_len);
                } else {
                    WS_LOG_UNALIGNED_RB_SEND("ws_io_tcp_send", client->sock_fd, &client->send_rb, data, len);
                    sent = send(client->sock_fd, data, len, 0);
                }
                
                if (sent > 0) {
                    // log_d("[ws_custom]Sent %d bytes", sent);
                    rb_advance(&client->send_rb, sent);
                    // 有实际写进展，解除“写需要读”的卡住标记
                    logged_write_want_read = false;
                    send_fatal_err_count = 0;
                    ssl_need_write = false; // 有写进展，TLS 层通常已满足
                } else {
                    // 处理非阻塞错误
                    bool would_block = false;
                    if (client->use_ssl) {
                        if (sent == MBEDTLS_ERR_SSL_WANT_WRITE || sent == MBEDTLS_ERR_SSL_WANT_READ) {
                            would_block = true;
                        }
                        if (sent == MBEDTLS_ERR_SSL_WANT_WRITE) {
                            ssl_need_write = true;
                        }

                        // P2: 观测TLS耦合：ssl_write 可能需要先读（WANT_READ）才能继续写
                        if (sent == MBEDTLS_ERR_SSL_WANT_READ && !logged_write_want_read) {
                            log_mqtt("【debug】[ws_custom]ssl_write WANT_READ (tls_coupling), state=%d, fd=%d, rb_avail=%d, rb_space=%d",
                                     (int)client->state, client->sock_fd,
                                     (int)rb_available(&client->send_rb), (int)rb_space(&client->send_rb));
                            logged_write_want_read = true;
                        }
                    } else {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) {
                            would_block = true;
                        }
                    }
                    
                    if (!would_block) {
                        log_e("[ws_custom]Send error: %d", sent);
                        // -0x004E: NET_SEND_FAILED，经常意味着底层 socket 已异常（ENOTCONN/断链等），继续死循环会导致系统状态进一步恶化
                        send_fatal_err_count++;
                        if (client->use_ssl &&
                            (sent == MBEDTLS_ERR_NET_SEND_FAILED || sent == MBEDTLS_ERR_NET_CONN_RESET)) {
                            snprintf(exit_reason, sizeof(exit_reason), "ssl_send_fatal=%d", sent);
                            client->thread_running = false;
                        } else if (!client->use_ssl) {
                            snprintf(exit_reason, sizeof(exit_reason), "tcp_send_error=%d", errno);
                            client->thread_running = false;
                        } else if (send_fatal_err_count >= 3) {
                            snprintf(exit_reason, sizeof(exit_reason), "ssl_send_error=%d", sent);
                            client->thread_running = false;
                        }
                    }
                }
            }
            os_mutex_post(&client->mutex);
            if (!client->thread_running) {
                break;
            }
        }
        
        // 处理读事件
        if (ret == 0) continue; // 超时，继续下一轮循环
        
        // SSL 模式下：当 ssl_read 返回 WANT_WRITE 时，需要在“可写事件”上重试 ssl_read，否则会卡死。
        bool should_read =
            (ret > 0 && FD_ISSET(fd, &read_fds)) ||
            (client->use_ssl && ssl_need_write && ret > 0 && FD_ISSET(fd, &write_fds));

        if (should_read) {
            // 读取数据
            int n = 0;
            if (client->use_ssl) {
                // SSL 读：避免直接写入到可能非对齐的 recv_buf+recv_buf_len
                int want = (int)(WS_RECV_BUF_SIZE - client->recv_buf_len);
                if (want > WS_SSL_IO_TMP_SIZE) want = WS_SSL_IO_TMP_SIZE;
                if (want <= 0) {
                    // 接收缓冲区已满，先交给上层解析/腾挪（本轮不再读）
                    n = 0;
                } else {
                    n = mbedtls_ssl_read(&client->ssl, ssl_io_tmp, want);
                }
                if (n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE) {
                    // P2: 观测TLS耦合：ssl_read 可能需要先写（WANT_WRITE）才能继续读
                    if (n == MBEDTLS_ERR_SSL_WANT_WRITE && !logged_read_want_write) {
                        // watch_write: 当前是否监听写事件（当前实现：has_data_to_send 或 ssl_need_write 才监听）
                        bool watch_write = has_data_to_send || ssl_need_write;
                        log_mqtt("【debug】[ws_custom]ssl_read WANT_WRITE (tls_coupling), state=%d, fd=%d, watch_write=%d, rb_avail=%d, rb_space=%d",
                                 (int)client->state, client->sock_fd, (int)watch_write,
                                 (int)rb_available(&client->send_rb), (int)rb_space(&client->send_rb));
                        logged_read_want_write = true;
                    }
                    ssl_need_write = (n == MBEDTLS_ERR_SSL_WANT_WRITE);
                    n = 0; // 忽略
                } else if (n == 0) {
                    // mbedTLS: 0 表示底层连接 EOF（对端可能直接断开，未必发送 close_notify）
                    // 为了定位根因，尽量收集当下 socket 错误信息（仅调试日志）
                    int so_error = 0;
                    socklen_t so_len = sizeof(so_error);
                    int fd_snapshot = client->sock_fd;
                    int errno_snapshot = errno;
                    int gs_ret = -1;
                    if (fd_snapshot >= 0) {
                        gs_ret = getsockopt(fd_snapshot, SOL_SOCKET, SO_ERROR, &so_error, &so_len);
                    }
                    log_mqtt("【debug】[ws_custom]ssl_eof: ssl_read==0, state=%d, fd=%d, errno=%d, getsockopt_ret=%d, so_error=%d",
                             (int)client->state, fd_snapshot, errno_snapshot, gs_ret, so_error);
                    log_i("[ws_custom]SSL connection closed by peer (EOF)");
                    strncpy(exit_reason, "ssl_eof", sizeof(exit_reason) - 1);
                    exit_reason[sizeof(exit_reason) - 1] = '\0';
                    break;
                } else if (n <= 0) {
                    // 处理可重试的网络层错误：-0x004c (RECV_FAILED) 和 -0x0050 (CONN_RESET)
                    // 这些错误可能是临时性的网络抖动，不应该立即断开连接
                    if (n == MBEDTLS_ERR_NET_RECV_FAILED || n == MBEDTLS_ERR_NET_CONN_RESET) {
                        ssl_read_error_count++;
                        log_w("[ws_custom]SSL read error -0x%04x (temporary), retry %d/%d",
                              -n, ssl_read_error_count, MAX_SSL_READ_RETRIES);

                        if (ssl_read_error_count < MAX_SSL_READ_RETRIES) {
                            msleep(SSL_READ_RETRY_DELAY);
                            continue; // 重试
                        } else {
                            log_e("[ws_custom]SSL read error -0x%04x (permanent), maximum retries reached", -n);
                            snprintf(exit_reason, sizeof(exit_reason), "ssl_read_error=-0x%04x (retries_exhausted)", -n);
                            break;
                        }
                    } else if (n != MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY && n != 0) {
                        char ebuf[128];
                        log_e("[ws_custom]SSL read error: -0x%04x (%s)", -n,
                              ws_mbedtls_errstr(n, ebuf, sizeof(ebuf)));
                        snprintf(exit_reason, sizeof(exit_reason), "ssl_read_error=-0x%04x", -n);
                        break;
                    }
                }
            } else {
                if (ret > 0 && FD_ISSET(fd, &read_fds)) {
                    n = recv(client->sock_fd, client->recv_buf + client->recv_buf_len,
                            WS_RECV_BUF_SIZE - client->recv_buf_len, 0);
                    if (n < 0) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) n = 0;
                        else {
                            log_e("[ws_custom]TCP recv error: %d", errno);
                            snprintf(exit_reason, sizeof(exit_reason), "tcp_recv_error errno=%d", errno);
                            break;
                        }
                    } else if (n == 0) {
                        log_i("[ws_custom]Connection closed by peer");
                        strncpy(exit_reason, "tcp_eof", sizeof(exit_reason) - 1);
                        exit_reason[sizeof(exit_reason) - 1] = '\0';
                        break;
                    }
                } else if (n == 0) {
                    log_i("[ws_custom]Connection closed by peer");
                    break;
                }
            }
            
            if (n > 0) {
                if (client->use_ssl) {
                    // n 字节从对齐临时缓冲复制到业务接收缓冲
                    if ((u32)n > (WS_RECV_BUF_SIZE - client->recv_buf_len)) {
                        // 理论上不应发生（want 已限制），防御一下
                        n = (int)(WS_RECV_BUF_SIZE - client->recv_buf_len);
                    }
                    memcpy(client->recv_buf + client->recv_buf_len, ssl_io_tmp, n);
                }
                client->recv_buf_len += n;
                // 有实际读进展，解除“读需要写”的卡住标记
                logged_read_want_write = false;
                ssl_need_write = false;
                
                // 解析WebSocket帧
                while (client->recv_buf_len > 0) {
                    u8 *payload = NULL;
                    u32 payload_len = 0;
                    ws_frame_type_t frame_type;
                    bool is_fin;
                    
                    int frame_len = parse_ws_frame(client->recv_buf, client->recv_buf_len,
                                                  &payload, &payload_len, &frame_type, &is_fin);
                    
                    if (frame_len == 0) break; // 数据不够
                    
                    if (frame_len < 0) {
                         log_e("[ws_custom]Invalid WebSocket frame");
                         client->thread_running = false;
                         break;
                    }
                    
                    // 处理帧
                    switch (frame_type) {
                        case WS_FRAME_TEXT:
                        case WS_FRAME_BINARY:
                            if (client->recv_cb && payload_len > 0) {
                                client->recv_cb(payload, payload_len, (frame_type == WS_FRAME_TEXT) ? 1 : 0);
                            }
                            break;
                            
                        case WS_FRAME_CLOSE:
                            log_i("[ws_custom]Received CLOSE frame");
                            // 尝试解析 close code
                            if (payload_len >= 2) {
                                u16 code = ((u16)payload[0] << 8) | payload[1];
                                snprintf(exit_reason, sizeof(exit_reason), "ws_close code=%u", code);
                            } else {
                                strncpy(exit_reason, "ws_close", sizeof(exit_reason) - 1);
                                exit_reason[sizeof(exit_reason) - 1] = '\0';
                            }
                            // 写入 CLOSE 响应到 RingBuffer
                            os_mutex_pend(&client->mutex, 0);
                            int close_len = build_ws_frame(resp_buf, 1024, NULL, 0, WS_FRAME_CLOSE, true);
                            if (close_len > 0 && rb_space(&client->send_rb) >= close_len) {
                                rb_write(&client->send_rb, resp_buf, close_len);
                            }
                            os_mutex_post(&client->mutex);
                            client->thread_running = false;
                            // 直接退出线程，避免继续处理后续数据
                            goto thread_exit;
                            
                        case WS_FRAME_PING:
                            log_i("[ws_custom]Received PING frame");
                            // 写入 PONG 响应到 RingBuffer
                            os_mutex_pend(&client->mutex, 0);
                            int pong_len = build_ws_frame(resp_buf, 1024, payload, payload_len, WS_FRAME_PONG, true);
                            if (pong_len > 0 && rb_space(&client->send_rb) >= pong_len) {
                                rb_write(&client->send_rb, resp_buf, pong_len);
                            } else {
                                log_e("[ws_custom]Failed to buffer PONG response");
                            }
                            os_mutex_post(&client->mutex);
                            break;
                            
                        case WS_FRAME_PONG:
                            log_i("[ws_custom]Received PONG frame");
                            break;
                            
                        default:
                            log_i("[ws_custom]Received unknown frame type: 0x%02x", frame_type);
                            break;
                    }
                    
                    // 移除处理过的帧
                     memmove(client->recv_buf, client->recv_buf + frame_len,
                           client->recv_buf_len - frame_len);
                    client->recv_buf_len -= frame_len;
                }
            }
        }

        // 防止 ssl_need_write 模式下 write-ready 立即返回导致的空转
        if (client->use_ssl && ssl_need_write && !has_data_to_send &&
            ret > 0 && FD_ISSET(fd, &write_fds) && !FD_ISSET(fd, &read_fds)) {
            msleep(5);
        }
    }
    
    log_i("[ws_custom]IO thread exited");
thread_exit:
    log_mqtt("【debug】[ws_custom]ws_recv_thread: thread_exit begin, state=%d, fd=%d, use_ssl=%d, exit_reason=%s",
          client ? (int)client->state : -1,
          client ? client->sock_fd : -1,
          client ? (int)client->use_ssl : 0,
          exit_reason);
    free(frame_buf);
    free(resp_buf);
    free(ssl_io_tmp_mem);

    client->thread_running = false;

    // 统一在 IO 线程退出时做清理，避免外部 kill 导致 socket 直接被硬断（服务端常见 1006 unexpected EOF）
    os_mutex_pend(&client->mutex, 0);
    log_mqtt("【debug】[ws_custom]ws_recv_thread: cleanup begin (locked), state=%d, fd=%d",
          (int)client->state, client->sock_fd);
    if (client->sock_fd >= 0) {
        if (client->use_ssl) {
            log_mqtt("【debug】[ws_custom]ws_recv_thread: ssl_close_notify/free begin");
            (void)mbedtls_ssl_close_notify(&client->ssl);
            mbedtls_ssl_free(&client->ssl);
            mbedtls_ssl_config_free(&client->ssl_conf);
            mbedtls_ctr_drbg_free(&client->ctr_drbg);
            mbedtls_entropy_free(&client->entropy);
            log_mqtt("【debug】[ws_custom]ws_recv_thread: ssl resources freed");
        }
        log_mqtt("【debug】[ws_custom]ws_recv_thread: closing fd=%d", client->sock_fd);
        close(client->sock_fd);
        client->sock_fd = -1;
    }
    client->state = WS_STATE_DISCONNECTED;
    client->recv_thread_id = 0;
    log_mqtt("【debug】[ws_custom]ws_recv_thread: cleanup done, state=%d, fd=%d", (int)client->state, client->sock_fd);
    os_mutex_post(&client->mutex);

    if (client->disconnect_cb) {
        log_mqtt("【debug】[ws_custom]ws_recv_thread: calling disconnect_cb, exit_reason=%s", exit_reason);
        client->disconnect_cb(exit_reason);
    }
    log_mqtt("【debug】[ws_custom]ws_recv_thread: thread_exit end");
}

// API实现
ws_client_t ws_client_create(void)
{
    ws_client_t client = malloc(sizeof(struct ws_client_handle));
    if (!client) {
        return NULL;
    }
    
    memset(client, 0, sizeof(struct ws_client_handle));
    client->sock_fd = -1;
    client->state = WS_STATE_DISCONNECTED;
    
    client->recv_buf = malloc(WS_RECV_BUF_SIZE);
    client->send_buf_mem = malloc(WS_SEND_BUF_SIZE);
    client->temp_send_buf = malloc(16 * 1024); // 临时构建缓冲区
    
    if (!client->recv_buf || !client->send_buf_mem || !client->temp_send_buf) {
        ws_client_destroy(client);
        return NULL;
    }
    
    rb_init(&client->send_rb, client->send_buf_mem, WS_SEND_BUF_SIZE);
    
    os_mutex_create(&client->mutex);
    
    return client;
}

int ws_client_connect(ws_client_t client, const char *url,
                     ws_recv_callback_t recv_cb,
                     ws_disconnect_callback_t disconnect_cb)
{
    if (!client || !url) {
        return -1;
    }
    
    os_mutex_pend(&client->mutex, 0);
    
    if (client->state != WS_STATE_DISCONNECTED) {
        os_mutex_post(&client->mutex);
        return -1;
    }
    
    client->state = WS_STATE_CONNECTING;
    client->recv_cb = recv_cb;
    client->disconnect_cb = disconnect_cb;
    client->recv_buf_len = 0;
    
    // 解析URL
    if (parse_ws_url(url, client->host, &client->port, client->path, &client->use_ssl) < 0) {
        client->state = WS_STATE_DISCONNECTED;
        os_mutex_post(&client->mutex);
        return -1;
    }
    
    // 执行握手
    int ret;
    if (client->use_ssl) {
        ret = do_ssl_handshake(client, client->host, client->port, client->path);
    } else {
        ret = do_tcp_handshake(client, client->host, client->port, client->path);
    }
    
    if (ret < 0) {
        client->state = WS_STATE_DISCONNECTED;
        os_mutex_post(&client->mutex);
        return -1;
    }
    
    client->state = WS_STATE_CONNECTED;
    client->thread_running = true;
    
    // 启动接收线程
    // mbedTLS + select + 日志在高压场景下 2KB 栈很容易溢出，溢出后常表现为 illeg_err / pc_limit_err（PC 跳到堆/栈区）
    thread_fork("ws_custom_io", 10, 4096, 10, &client->recv_thread_id,
                ws_recv_thread, client);
    
    os_mutex_post(&client->mutex);
    log_i("[ws_custom] WebSocket connected successfully✅");
    return 0;
}

int ws_client_send_text(ws_client_t client, const u8 *data, u32 len)
{
    // 实验A：文本帧与二进制帧必须严格区分，不能根据 payload 内容猜测帧类型。
    // 之前用 data[0] < 128 判断会把大量二进制误发成 TEXT，服务端/网关可能直接 CLOSE。
    if (!client || !data || len == 0) {
        return -1;
    }

    os_mutex_pend(&client->mutex, 0);

    if (client->state != WS_STATE_CONNECTED) {
        log_w("[ws_custom]WARNING: WebSocket not connected, state=%d", client->state);
        os_mutex_post(&client->mutex);
        return -1;
    }

    // 构建 WebSocket TEXT 帧（客户端必须 mask）
    int frame_len = build_ws_frame(client->temp_send_buf, 16 * 1024,
                                   data, len, WS_FRAME_TEXT, true);
    if (frame_len < 0) {
        log_e("[ws_custom]ERROR: build_ws_frame(TEXT) failed, frame_len=%d", frame_len);
        os_mutex_post(&client->mutex);
        return -1;
    }
    if (validate_ws_frame_header(client->temp_send_buf, frame_len) < 0) {
        log_e("[ws_custom]ERROR: Frame validation failed before send in ws_client_send_text");
        os_mutex_post(&client->mutex);
        return -1;
    }
    if (rb_available(&client->send_rb) > (client->send_rb.capacity / 2)) {
        log_w("[ws_custom]WARNING: Send buffer > 50%% full (%d/%d). Dropping frame len=%d to prevent bloat.", 
              rb_available(&client->send_rb), client->send_rb.capacity, frame_len);
        os_mutex_post(&client->mutex);
        return -1;
    }
    if (rb_space(&client->send_rb) < frame_len) {
        log_e("[ws_custom]ERROR: Send buffer full! Dropping frame. space=%d, need=%d",
              rb_space(&client->send_rb), frame_len);
        os_mutex_post(&client->mutex);
        return -1;
    }
    rb_write(&client->send_rb, client->temp_send_buf, frame_len);
    os_mutex_post(&client->mutex);
    return 0;
}

int ws_client_send_binary(ws_client_t client, const u8 *data, u32 len)
{
    if (!client || !data || len == 0) {
        return -1;
    }
    
    os_mutex_pend(&client->mutex, 0);
    
    if (client->state != WS_STATE_CONNECTED) {
        os_mutex_post(&client->mutex);
        return -1;
    }
    
    // 实验A：二进制发送必须固定使用 BINARY 帧，不能根据 payload 内容猜测帧类型。
    ws_frame_type_t frame_type = WS_FRAME_BINARY;
    
    // 使用临时缓冲区构建帧，避免长时间占用锁
    // 注意：WS_SEND_BUF_SIZE 现在是 RingBuffer 的总大小，不能用来做单帧构建
    // 使用 temp_send_buf (16KB)
    int frame_len = build_ws_frame(client->temp_send_buf, 16 * 1024,
                                   data, len, frame_type, true);  // 强制mask=true
    
    if (frame_len < 0) {
        log_e("[ws_custom]ERROR: build_ws_frame failed, frame_len=%d", frame_len);
        os_mutex_post(&client->mutex);
        return -1;
    }
    
    // 发送前最终验证帧头
    if (validate_ws_frame_header(client->temp_send_buf, frame_len) < 0) {
        log_e("[ws_custom]ERROR: Frame validation failed before send in ws_client_send_binary");
        os_mutex_post(&client->mutex);
        return -1;
    }
    
    // 发送帧 -> 写入 RingBuffer
    // log_i("[ws_custom]Buffering frame: type=0x%02x, len=%d", frame_type, len);
    // 注意：这里绝不能用 sprintf 循环拼 hex（会反复写 '\0'，极易越界导致栈被踩，最终触发 PC 跑飞/axi_rd_inv）
    if (len > 0 && len < 128) { // 只打印短数据的内容
        static const char hex[] = "0123456789abcdef";
        char dump_buf[256];
        int dump_len = (len > 127) ? 127 : (int)len; // 确保 2*dump_len + 1 <= 255
        // 修复：添加边界检查，防止数组越界
        if (dump_len * 2 + 1 > sizeof(dump_buf)) {
            dump_len = (sizeof(dump_buf) - 1) / 2;
        }
        for (int i = 0; i < dump_len; i++) {
            // 确保索引在有效范围内
            if (i * 2 + 1 < sizeof(dump_buf) && (data[i] >> 4) < 16 && (data[i] & 0xF) < 16) {
                dump_buf[i * 2]     = hex[(data[i] >> 4) & 0xF];
                dump_buf[i * 2 + 1] = hex[data[i] & 0xF];
            }
        }
        dump_buf[dump_len * 2] = '\0';
        log_i("[ws_custom]Data: %s", dump_buf);
        if (frame_type == WS_FRAME_TEXT) {
            log_i("[ws_custom]Text: %.*s", dump_len, data);
        }
    }

    // 尝试写入 RingBuffer
    // 检查缓冲区占用率，如果超过50%，则丢弃非关键帧（这里假设所有通过此接口发送的都是可以丢弃的）
    // WS_SEND_BUF_SIZE = 64KB, 50% = 32KB
    if (rb_available(&client->send_rb) > (client->send_rb.capacity / 2)) {
        // log_w("[ws_custom]Warning: Send buffer > 50%% full (%d/%d). Dropping frame len=%d to prevent bloat.", 
            //   rb_available(&client->send_rb), client->send_rb.capacity, frame_len);
        os_mutex_post(&client->mutex);
        return -1; // 模拟发送失败，上层应用应该忽略此错误
    }

    if (rb_space(&client->send_rb) < frame_len) {
        log_e("[ws_custom]ERROR: Send buffer full! Dropping frame. space=%d, need=%d", 
              rb_space(&client->send_rb), frame_len);
        os_mutex_post(&client->mutex);
        return -1; // 发送缓冲区满，丢弃
    }
    
    rb_write(&client->send_rb, client->temp_send_buf, frame_len);
    
    os_mutex_post(&client->mutex);
    return len; // 返回原始数据长度
}

int ws_client_send_ping(ws_client_t client)
{
    if (!client) {
        return -1;
    }
    
    os_mutex_pend(&client->mutex, 0);
    
    if (client->state != WS_STATE_CONNECTED) {
        os_mutex_post(&client->mutex);
        return -1;
    }
    
    u8 ping_payload[] = "ping";
    
    log_i("[ws_custom]Buffering PING frame");

    int frame_len = build_ws_frame(client->temp_send_buf, 16 * 1024,
                                   ping_payload, sizeof(ping_payload) - 1,
                                   WS_FRAME_PING, true);
    
    if (frame_len < 0) {
        log_e("[ws_custom]ERROR: build_ws_frame failed for PING, frame_len=%d", frame_len);
        os_mutex_post(&client->mutex);
        return -1;
    }
    
    // 发送前验证帧头
    if (validate_ws_frame_header(client->temp_send_buf, frame_len) < 0) {
        log_e("[ws_custom]ERROR: Frame validation failed before send in ws_client_send_ping");
        os_mutex_post(&client->mutex);
        return -1;
    }
    
    if (rb_space(&client->send_rb) < frame_len) {
         log_e("[ws_custom]ERROR: Send buffer full for PING!");
         os_mutex_post(&client->mutex);
         return -1;
    }
    
    rb_write(&client->send_rb, client->temp_send_buf, frame_len);
    
    os_mutex_post(&client->mutex);
    return 0;
}

void ws_client_disconnect(ws_client_t client)
{
    if (!client) {
        return;
    }

    // 幂等短路：避免上层多处重复调用导致调试日志混乱
    // 满足以下条件基本代表已经彻底断开，不需要重复执行 disconnect 流程
    if (client->state == WS_STATE_DISCONNECTED && client->sock_fd < 0 && client->recv_thread_id == 0) {
        log_mqtt("【debug】[ws_custom]ws_client_disconnect: already disconnected (skip), state=%d, fd=%d, tid=%d",
                 (int)client->state, client->sock_fd, client->recv_thread_id);
        return;
    }

    log_mqtt("【debug】[ws_custom]ws_client_disconnect: enter, state=%d, running=%d, fd=%d, tid=%d, use_ssl=%d",
          (int)client->state, (int)client->thread_running, client->sock_fd, client->recv_thread_id, (int)client->use_ssl);
    
    os_mutex_pend(&client->mutex, 0);
    
    if (client->state == WS_STATE_CONNECTED) {
        client->state = WS_STATE_CLOSING;
        // client->thread_running = false; // 先不置 false，让线程有机会发送 CLOSE 帧
        
        // 发送CLOSE帧（客户端必须mask）
        // 使用临时缓冲区
        int close_len = build_ws_frame(client->temp_send_buf, 16 * 1024,
                                      NULL, 0, WS_FRAME_CLOSE, true);
        if (close_len > 0) {
            // 发送前验证帧头
            if (validate_ws_frame_header(client->temp_send_buf, close_len) < 0) {
                log_e("[ws_custom]ERROR: Frame validation failed for CLOSE frame in ws_client_disconnect");
            } else {
                if (rb_space(&client->send_rb) >= close_len) {
                    rb_write(&client->send_rb, client->temp_send_buf, close_len);
                    log_mqtt("【debug】[ws_custom]ws_client_disconnect: buffered CLOSE frame, len=%d", close_len);
                } else {
                    log_mqtt("【debug】[ws_custom]ws_client_disconnect: not enough rb_space for CLOSE, space=%d, need=%d",
                          rb_space(&client->send_rb), close_len);
                }
            }
        } else {
            log_e("[ws_custom]ERROR: build_ws_frame failed for CLOSE frame, close_len=%d", close_len);
        }
    }
    
    os_mutex_post(&client->mutex);
    
    // 给一点时间让IO线程发送数据
    msleep(100);

    log_mqtt("【debug】[ws_custom]ws_client_disconnect: request io thread exit (thread_running=false)");
    client->thread_running = false;
    
    // 等待接收线程退出 (此时不能持有锁)
    if (client->recv_thread_id > 0) {
        log_mqtt("【debug】[ws_custom]ws_client_disconnect: waiting io thread tid=%d", client->recv_thread_id);
        thread_kill(&client->recv_thread_id, KILL_WAIT);
        log_mqtt("【debug】[ws_custom]ws_client_disconnect: io thread wait done");
    }
    
    // 重新获取锁进行清理
    os_mutex_pend(&client->mutex, 0);
    
    // 兜底清理：如果 IO 线程因为被强杀等原因没走到 thread_exit，这里保证资源最终被回收。
    if (client->sock_fd >= 0) {
        log_mqtt("【debug】[ws_custom]ws_client_disconnect: fallback cleanup begin, fd=%d, use_ssl=%d",
              client->sock_fd, (int)client->use_ssl);
        if (client->use_ssl) {
            (void)mbedtls_ssl_close_notify(&client->ssl);
            mbedtls_ssl_free(&client->ssl);
            mbedtls_ssl_config_free(&client->ssl_conf);
            mbedtls_ctr_drbg_free(&client->ctr_drbg);
            mbedtls_entropy_free(&client->entropy);
        }
        close(client->sock_fd);
        client->sock_fd = -1;
    }
    client->state = WS_STATE_DISCONNECTED;
    client->recv_thread_id = 0;
    log_mqtt("【debug】[ws_custom]ws_client_disconnect: exit, state=%d, fd=%d, tid=%d",
          (int)client->state, client->sock_fd, client->recv_thread_id);
    
    os_mutex_post(&client->mutex);
}

void ws_client_destroy(ws_client_t client)
{
    if (!client) {
        return;
    }
    
    ws_client_disconnect(client);
    
    if (client->recv_buf) {
        free(client->recv_buf);
    }
    if (client->send_buf_mem) {
        free(client->send_buf_mem);
    }
    if (client->temp_send_buf) {
        free(client->temp_send_buf);
    }
    
    os_mutex_del(&client->mutex, 0);
    free(client);
}

ws_state_t ws_client_get_state(ws_client_t client)
{
    if (!client) {
        return WS_STATE_DISCONNECTED;
    }
    return client->state;
}

