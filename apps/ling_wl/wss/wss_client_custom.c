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
#include "log.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

// 声明random32函数（在lwIP中定义）
extern unsigned int random32(int type);

#define WS_MAGIC_KEY "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
#define WS_RECV_BUF_SIZE (32 * 1024)
#define WS_SEND_BUF_SIZE (16 * 1024)
#define WS_RECV_TIMEOUT_MS 5000

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
    u8 *send_buf;                  // 发送缓冲区
    
    ws_recv_callback_t recv_cb;    // 接收回调
    ws_disconnect_callback_t disconnect_cb; // 断开回调
    
    int recv_thread_id;            // 接收线程ID
    OS_MUTEX mutex;                // 互斥锁
    bool thread_running;          // 接收线程运行标志
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
    int len = snprintf(request, request_size,
        "GET %s HTTP/1.1\r\n"
        "Host: %s:%d\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: %s\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "Origin: http://localhost\r\n"
        "\r\n",
        path, host, port, key);
    
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
    for (int i = 0; i < log_len; i++) {
        if (response[i] >= 32 && response[i] < 127) {
            log_i("%c", response[i]);
        } else {
            log_i("[0x%02x]", (u8)response[i]);
        }
    }
    log_i("\n");
    
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
        strstr(response, "connection: upgrade") == NULL) {
        log_e("[ws_custom]Missing Connection header");
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

/**
 * 执行WebSocket握手（普通TCP）
 */
static int do_tcp_handshake(ws_client_t client, const char *host, int port, const char *path)
{
    struct sockaddr_in server_addr;
    struct hostent *server;
    
    // 解析主机名
    server = gethostbyname(host);
    if (server == NULL) {
        log_e("[ws_custom]Failed to resolve hostname: %s", host);
        return -1;
    }
    
    // 创建socket
    client->sock_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (client->sock_fd < 0) {
        log_e("[ws_custom]Failed to create socket");
        return -1;
    }
    
    // 设置socket选项
    int opt = 1;
    setsockopt(client->sock_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    
    // 设置接收超时
    struct timeval tv;
    tv.tv_sec = WS_RECV_TIMEOUT_MS / 1000;
    tv.tv_usec = (WS_RECV_TIMEOUT_MS % 1000) * 1000;
    setsockopt(client->sock_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    
    // 连接服务器
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    memcpy(&server_addr.sin_addr.s_addr, server->h_addr, server->h_length);
    server_addr.sin_port = htons(port);
    
    if (connect(client->sock_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        log_e("[ws_custom]Failed to connect to %s:%d", host, port);
        close(client->sock_fd);
        client->sock_fd = -1;
        return -1;
    }
    
    log_i("[ws_custom]TCP connected to %s:%d", host, port);
    
    // 生成握手请求
    char ws_key[32];
    char handshake_req[1024];
    int req_len = generate_handshake_request(host, port, path, ws_key, handshake_req, sizeof(handshake_req));
    if (req_len < 0) {
        close(client->sock_fd);
        client->sock_fd = -1;
        return -1;
    }
    
    // 发送握手请求
    int sent = send(client->sock_fd, handshake_req, req_len, 0);
    if (sent != req_len) {
        log_e("[ws_custom]Failed to send handshake request: sent=%d, expected=%d", sent, req_len);
        close(client->sock_fd);
        client->sock_fd = -1;
        return -1;
    }
    
    // 读取握手响应
    char response[2048];
    int resp_len = read_http_response(client->sock_fd, response, sizeof(response), WS_RECV_TIMEOUT_MS);
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
    
    log_i("[ws_custom]WebSocket handshake successful");
    return 0;
}

/**
 * 执行WebSocket握手（SSL/TLS）
 */
static int do_ssl_handshake(ws_client_t client, const char *host, int port, const char *path)
{
    int ret;
    
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
        log_e("[ws_custom]mbedtls_ctr_drbg_seed failed: -0x%04x", -ret);
        goto cleanup;
    }
    
    // 配置SSL
    ret = mbedtls_ssl_config_defaults(&client->ssl_conf,
                                      MBEDTLS_SSL_IS_CLIENT,
                                      MBEDTLS_SSL_TRANSPORT_STREAM,
                                      MBEDTLS_SSL_PRESET_DEFAULT);
    if (ret != 0) {
        log_e("[ws_custom]mbedtls_ssl_config_defaults failed: -0x%04x", -ret);
        goto cleanup;
    }
    
    mbedtls_ssl_conf_authmode(&client->ssl_conf, MBEDTLS_SSL_VERIFY_OPTIONAL);
    mbedtls_ssl_conf_rng(&client->ssl_conf, mbedtls_ctr_drbg_random, &client->ctr_drbg);
    
    // 设置CA证书（如果需要验证）
    // mbedtls_ssl_conf_ca_chain(&client->ssl_conf, &cacert, NULL);
    
    // 设置主机名
    ret = mbedtls_ssl_set_hostname(&client->ssl, host);
    if (ret != 0) {
        log_e("[ws_custom]mbedtls_ssl_set_hostname failed: -0x%04x", -ret);
        goto cleanup;
    }
    
    // 初始化SSL上下文
    ret = mbedtls_ssl_setup(&client->ssl, &client->ssl_conf);
    if (ret != 0) {
        log_e("[ws_custom]mbedtls_ssl_setup failed: -0x%04x", -ret);
        goto cleanup;
    }
    
    // 连接TCP
    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%d", port);
    ret = mbedtls_net_connect(&client->net_ctx, host, port_str, MBEDTLS_NET_PROTO_TCP);
    if (ret != 0) {
        log_e("[ws_custom]mbedtls_net_connect failed: -0x%04x", -ret);
        goto cleanup;
    }
    
    // mbedtls_net_context 使用 hdl 而不是 fd，不需要直接访问
    // SSL连接时，sock_fd设为-1表示使用SSL
    client->sock_fd = -1;
    log_i("[ws_custom]TCP connected to %s:%d (SSL)", host, port);
    
    // 设置bio回调
    mbedtls_ssl_set_bio(&client->ssl, &client->net_ctx, mbedtls_net_send, mbedtls_net_recv, NULL);
    
    // 执行SSL握手
    while ((ret = mbedtls_ssl_handshake(&client->ssl)) != 0) {
        if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
            log_e("[ws_custom]mbedtls_ssl_handshake failed: -0x%04x", -ret);
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
        ret = mbedtls_ssl_write(&client->ssl, (unsigned char *)handshake_req + sent, req_len - sent);
        if (ret <= 0) {
            if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
                log_e("[ws_custom]mbedtls_ssl_write failed: -0x%04x", -ret);
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
        ret = mbedtls_ssl_read(&client->ssl, (unsigned char *)response + resp_len, sizeof(response) - resp_len - 1);
        if (ret <= 0) {
            if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE) {
                continue;
            }
            log_e("[ws_custom]mbedtls_ssl_read failed: -0x%04x", -ret);
            goto cleanup;
        }
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
    
    log_i("[ws_custom]WebSocket handshake successful (SSL)");
    return 0;
    
cleanup:
    if (client->sock_fd >= 0) {
        mbedtls_net_free(&client->net_ctx);
        client->sock_fd = -1;
    }
    mbedtls_ssl_free(&client->ssl);
    mbedtls_ssl_config_free(&client->ssl_conf);
    mbedtls_ctr_drbg_free(&client->ctr_drbg);
    mbedtls_entropy_free(&client->entropy);
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
        u32 mask_key = random32(0);
        // 保存masking key的位置
        u8 *mask_bytes = buf + pos;
        // 使用明确的字节序，避免memcpy的字节序问题
        buf[pos++] = (mask_key >> 0) & 0xFF;
        buf[pos++] = (mask_key >> 8) & 0xFF;
        buf[pos++] = (mask_key >> 16) & 0xFF;
        buf[pos++] = (mask_key >> 24) & 0xFF;
        // 注意：pos已经通过上面的4个pos++增加了4，不需要再加4
        
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
    
    // 专门用于接收线程发送响应的局部缓冲区，避免与主线程竞争 send_buf
    // PING/PONG/CLOSE 帧通常很小，512字节足够
    u8 resp_buf[512]; 
    
    if (!frame_buf) {
        log_e("[ws_custom]Failed to allocate frame buffer");
        client->thread_running = false;
        return;
    }
    
    int frame_buf_len = 0;
    
    log_i("[ws_custom]Receive thread started");
    
    while (client->thread_running && client->state == WS_STATE_CONNECTED) {
        // 读取数据
        int n;
        if (client->use_ssl) {
            // SSL连接：直接使用mbedtls_ssl_read，它会处理阻塞/非阻塞
            n = mbedtls_ssl_read(&client->ssl, client->recv_buf + client->recv_buf_len,
                                WS_RECV_BUF_SIZE - client->recv_buf_len);
            if (n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE) {
                continue;
            }
            if (n <= 0) {
                if (n != MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) {
                    log_e("[ws_custom]SSL read error: -0x%04x", -n);
                }
                break;
            }
        } else {
            // 非SSL连接：使用select检查是否有数据可读
            fd_set read_fds;
            struct timeval tv;
            FD_ZERO(&read_fds);
            FD_SET(client->sock_fd, &read_fds);
            tv.tv_sec = 1;
            tv.tv_usec = 0;
            
            int ret = select(client->sock_fd + 1, &read_fds, NULL, NULL, &tv);
            if (ret <= 0) {
                continue; // 超时或错误，继续循环
            }
            
            n = recv(client->sock_fd, client->recv_buf + client->recv_buf_len,
                    WS_RECV_BUF_SIZE - client->recv_buf_len, 0);
            if (n <= 0) {
                if (n == 0) {
                    log_i("[ws_custom]Connection closed by peer");
                } else {
                    log_e("[ws_custom]TCP recv error: %d", n);
                }
                break;
            }
        }
        
        client->recv_buf_len += n;
        
        // 解析WebSocket帧
        while (client->recv_buf_len > 0) {
            u8 *payload = NULL;
            u32 payload_len = 0;
            ws_frame_type_t frame_type;
            bool is_fin;
            
            int frame_len = parse_ws_frame(client->recv_buf, client->recv_buf_len,
                                          &payload, &payload_len, &frame_type, &is_fin);
            
            if (frame_len < 0) {
                log_e("[ws_custom]Invalid WebSocket frame");
                client->thread_running = false;
                break;
            }
            
            if (frame_len == 0) {
                // 数据不完整，等待更多数据
                break;
            }
            
            // 处理不同类型的帧
            switch (frame_type) {
            case WS_FRAME_TEXT:
            case WS_FRAME_BINARY:
                if (client->recv_cb && payload_len > 0) {
                    client->recv_cb(payload, payload_len, (frame_type == WS_FRAME_TEXT) ? 1 : 0);
                }
                break;
                
            case WS_FRAME_CLOSE:
                log_i("[ws_custom]Received CLOSE frame");
                // 发送CLOSE响应（客户端必须mask）
                // 使用局部缓冲区 resp_buf，避免竞争
                int close_len = build_ws_frame(resp_buf, sizeof(resp_buf),
                                              NULL, 0, WS_FRAME_CLOSE, true);
                if (close_len > 0) {
                    // 发送前验证帧头
                    if (validate_ws_frame_header(resp_buf, close_len) < 0) {
                        log_e("[ws_custom]ERROR: Frame validation failed for CLOSE response");
                    } else {
                        // 必须加锁发送，防止与主线程冲突
                        os_mutex_pend(&client->mutex, 0);
                        if (client->use_ssl) {
                            mbedtls_ssl_write(&client->ssl, resp_buf, close_len);
                        } else {
                            send(client->sock_fd, resp_buf, close_len, 0);
                        }
                        os_mutex_post(&client->mutex);
                    }
                } else {
                    log_e("[ws_custom]ERROR: build_ws_frame failed for CLOSE response, close_len=%d", close_len);
                }
                client->thread_running = false;
                break;
                
            case WS_FRAME_PING:
                log_i("[ws_custom]Received PING frame");
                // 发送PONG响应
                // 使用局部缓冲区 resp_buf，避免竞争
                log_i("[ws_custom]Sending PONG response");
                int pong_len = build_ws_frame(resp_buf, sizeof(resp_buf),
                                             payload, payload_len, WS_FRAME_PONG, true);
                if (pong_len > 0) {
                    // 发送前验证帧头
                    if (validate_ws_frame_header(resp_buf, pong_len) < 0) {
                        log_e("[ws_custom]ERROR: Frame validation failed for PONG response");
                    } else {
                        // 必须加锁发送，防止与主线程冲突
                        os_mutex_pend(&client->mutex, 0);
                        if (client->use_ssl) {
                            mbedtls_ssl_write(&client->ssl, resp_buf, pong_len);
                        } else {
                            send(client->sock_fd, resp_buf, pong_len, 0);
                        }
                        os_mutex_post(&client->mutex);
                    }
                } else {
                    log_e("[ws_custom]ERROR: build_ws_frame failed for PONG response, pong_len=%d", pong_len);
                }
                break;
                
            case WS_FRAME_PONG:
                log_i("[ws_custom]Received PONG frame");
                break;
                
            default:
                log_i("[ws_custom]Received unknown frame type: 0x%02x", frame_type);
                break;
            }
            
            // 移除已处理的帧
            memmove(client->recv_buf, client->recv_buf + frame_len,
                   client->recv_buf_len - frame_len);
            client->recv_buf_len -= frame_len;
        }
    }
    
    log_i("[ws_custom]Receive thread exited");
    free(frame_buf);
    
    // 通知断开
    if (client->disconnect_cb) {
        client->disconnect_cb("Receive thread exited");
    }
    
    client->thread_running = false;
    client->state = WS_STATE_DISCONNECTED;
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
    client->send_buf = malloc(WS_SEND_BUF_SIZE);
    
    if (!client->recv_buf || !client->send_buf) {
        ws_client_destroy(client);
        return NULL;
    }
    
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
    thread_fork("ws_custom_recv", 10, 2048, 0, &client->recv_thread_id,
                ws_recv_thread, client);
    
    os_mutex_post(&client->mutex);
    log_i("[ws_custom]WebSocket connected successfully");
    return 0;
}

int ws_client_send_text(ws_client_t client, const u8 *data, u32 len)
{
    return ws_client_send_binary(client, data, len); // 文本和二进制使用相同的发送逻辑
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
    
    // 构建WebSocket帧（客户端必须mask）
    ws_frame_type_t frame_type = (data[0] < 128) ? WS_FRAME_TEXT : WS_FRAME_BINARY;
    
    // 验证frame_type的有效性
    if (frame_type != WS_FRAME_TEXT && frame_type != WS_FRAME_BINARY) {
        log_e("[ws_custom]ERROR: Invalid frame_type 0x%02x in ws_client_send_binary", (u8)frame_type);
        os_mutex_post(&client->mutex);
        return -1;
    }
    
    int frame_len = build_ws_frame(client->send_buf, WS_SEND_BUF_SIZE,
                                   data, len, frame_type, true);  // 强制mask=true
    
    if (frame_len < 0) {
        log_e("[ws_custom]ERROR: build_ws_frame failed, frame_len=%d", frame_len);
        os_mutex_post(&client->mutex);
        return -1;
    }
    
    // 发送前最终验证帧头
    if (validate_ws_frame_header(client->send_buf, frame_len) < 0) {
        log_e("[ws_custom]ERROR: Frame validation failed before send in ws_client_send_binary");
        os_mutex_post(&client->mutex);
        return -1;
    }
    
    // 发送帧
    log_i("[ws_custom]Sending frame: type=0x%02x, len=%d", frame_type, len);
    if (len > 0 && len < 128) { // 只打印短数据的内容
        char dump_buf[256] = {0};
        int dump_len = len > 128 ? 128 : len;
        for (int i = 0; i < dump_len; i++) {
            sprintf(dump_buf + i * 2, "%02x", data[i]);
        }
        log_i("[ws_custom]Data: %s", dump_buf);
        if (frame_type == WS_FRAME_TEXT) {
            log_i("[ws_custom]Text: %.*s", dump_len, data);
        }
    }

    int sent = 0;
    if (client->use_ssl) {
        while (sent < frame_len) {
            int n = mbedtls_ssl_write(&client->ssl, client->send_buf + sent, frame_len - sent);
            if (n <= 0) {
                if (n != MBEDTLS_ERR_SSL_WANT_READ && n != MBEDTLS_ERR_SSL_WANT_WRITE) {
                    os_mutex_post(&client->mutex);
                    return -1;
                }
                continue;
            }
            sent += n;
        }
    } else {
        sent = send(client->sock_fd, client->send_buf, frame_len, 0);
        if (sent != frame_len) {
            os_mutex_post(&client->mutex);
            return -1;
        }
    }
    
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
    
    log_i("[ws_custom]Sending PING frame");

    int frame_len = build_ws_frame(client->send_buf, WS_SEND_BUF_SIZE,
                                   ping_payload, sizeof(ping_payload) - 1,
                                   WS_FRAME_PING, true);
    
    if (frame_len < 0) {
        log_e("[ws_custom]ERROR: build_ws_frame failed for PING, frame_len=%d", frame_len);
        os_mutex_post(&client->mutex);
        return -1;
    }
    
    // 发送前验证帧头
    if (validate_ws_frame_header(client->send_buf, frame_len) < 0) {
        log_e("[ws_custom]ERROR: Frame validation failed before send in ws_client_send_ping");
        os_mutex_post(&client->mutex);
        return -1;
    }
    
    int sent = 0;
    if (client->use_ssl) {
        sent = mbedtls_ssl_write(&client->ssl, client->send_buf, frame_len);
        if (sent <= 0 && sent != MBEDTLS_ERR_SSL_WANT_READ && sent != MBEDTLS_ERR_SSL_WANT_WRITE) {
            os_mutex_post(&client->mutex);
            return -1;
        }
        sent = frame_len;
    } else {
        sent = send(client->sock_fd, client->send_buf, frame_len, 0);
    }
    
    os_mutex_post(&client->mutex);
    return (sent == frame_len) ? 0 : -1;
}

void ws_client_disconnect(ws_client_t client)
{
    if (!client) {
        return;
    }
    
    os_mutex_pend(&client->mutex, 0);
    
    if (client->state == WS_STATE_CONNECTED) {
        client->state = WS_STATE_CLOSING;
        client->thread_running = false;
        
        // 发送CLOSE帧（客户端必须mask）
        // 使用build_ws_frame确保正确的MASK位和masking key
        int close_len = build_ws_frame(client->send_buf, WS_SEND_BUF_SIZE,
                                      NULL, 0, WS_FRAME_CLOSE, true);
        if (close_len > 0) {
            // 发送前验证帧头
            if (validate_ws_frame_header(client->send_buf, close_len) < 0) {
                log_e("[ws_custom]ERROR: Frame validation failed for CLOSE frame in ws_client_disconnect");
            } else {
                if (client->use_ssl) {
                    mbedtls_ssl_write(&client->ssl, client->send_buf, close_len);
                } else {
                    send(client->sock_fd, client->send_buf, close_len, 0);
                }
            }
        } else {
            log_e("[ws_custom]ERROR: build_ws_frame failed for CLOSE frame, close_len=%d", close_len);
        }
        
        // 等待接收线程退出
        if (client->recv_thread_id > 0) {
            thread_kill(&client->recv_thread_id, KILL_WAIT);
        }
        
        // 关闭socket
        if (client->use_ssl) {
            mbedtls_ssl_close_notify(&client->ssl);
            mbedtls_ssl_free(&client->ssl);
            mbedtls_ssl_config_free(&client->ssl_conf);
            mbedtls_net_free(&client->net_ctx);
            mbedtls_ctr_drbg_free(&client->ctr_drbg);
            mbedtls_entropy_free(&client->entropy);
        } else {
            close(client->sock_fd);
        }
        
        client->sock_fd = -1;
        client->state = WS_STATE_DISCONNECTED;
    }
    
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
    if (client->send_buf) {
        free(client->send_buf);
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

