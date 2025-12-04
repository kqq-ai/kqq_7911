#include "odm_secure.h"
#include "app_cookie.h"

#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "http/http_cli.h"
#include "cJSON_common/cJSON.h"

#include "util/cjson_util.h"
#include "syscfg_id.h"
#include "lfs.h"
#include "app_mode/lwl_app_framework.h"

extern lfs_t g_lfs;

#define BUF_SIZE 128
#define SN_SIZE 15
#define KEY_SIZE 16

// 通用HTTP请求函数，当context为空则用httpcli_get，否则用httpcli_post
static int http_request_with_body(const char* url, const char* context, http_body_obj* http_body_buf) {
    int ret = 0;
    httpcli_ctx *ctx = (httpcli_ctx *)calloc(1, sizeof(httpcli_ctx));
    if (!ctx) {
        log_i("calloc faile\n");
        return -1;
    }

    memset(http_body_buf, 0x0, sizeof(http_body_obj));
    http_body_buf->recv_len = 0;
    http_body_buf->buf_len = 4 * BUF_SIZE;
    http_body_buf->buf_count = 1;
    http_body_buf->p = (char *) malloc(http_body_buf->buf_len * http_body_buf->buf_count);
    if (!http_body_buf->p) {
        free(ctx);
        return -1;
    }

    ctx->url = (char*)url;
    ctx->timeout_millsec = 1000;
    ctx->priv = http_body_buf;
    ctx->connection = "close";

    if (context && strlen(context) > 0) {
        ctx->post_data = (char*)context;
        ctx->data_len = strlen(ctx->post_data);
        ctx->data_format = "application/json";
        ret = httpcli_post(ctx);
    } else {
        ret = httpcli_get(ctx);
    }

    // 关闭连接
    if (ret == 0) httpcli_close(ctx);

    if (ctx) {
        // ctx->url 指向外部字符串，不需要free
        free(ctx);
    }
    return ret;
}

size_t hex2bytes(const char *hex_str, uint8_t *bytes)
{
    size_t len = strlen(hex_str);
    if (len % 2 != 0 || len > 2*KEY_SIZE) return 0;          // 长度必须是偶数

    size_t byte_count = len / 2;
    for (size_t i = 0; i < byte_count; ++i) {
        char hi = hex_str[i * 2];
        char lo = hex_str[i * 2 + 1];

        // 检查字符合法性
        if (!isxdigit((unsigned char)hi) || !isxdigit((unsigned char)lo))
            return 0;

        // 转换
        uint8_t b = 0;
        if (isdigit((unsigned char)hi)) b = (hi - '0') << 4;
        else                            b = (toupper(hi) - 'A' + 10) << 4;

        if (isdigit((unsigned char)lo)) b |= (lo - '0');
        else                            b |= (toupper(lo) - 'A' + 10);

        bytes[i] = b;
    }
    return byte_count;
}

size_t bytes2hex(const uint8_t *bytes, size_t byte_count, char *hex_str, size_t hex_str_size)
{
    if (!bytes || !hex_str || hex_str_size < (byte_count * 2 + 1)) return 0;

    // 检查指针是否重叠
    const uint8_t *src = bytes;
    uint8_t *tmp_buf = NULL;
    if ((hex_str <= (char*)bytes && hex_str + hex_str_size > (char*)bytes) ||
        ((char*)bytes <= hex_str && (char*)bytes + byte_count > hex_str)) {
        // 有重叠，做临时拷贝
        tmp_buf = malloc(byte_count);
        if (!tmp_buf) return 0;
        memcpy(tmp_buf, bytes, byte_count);
        src = tmp_buf;
    }

    for (size_t i = 0; i < byte_count; ++i) {
        snprintf(hex_str + i * 2, 3, "%02X", src[i]);
    }
    hex_str[byte_count * 2] = '\0';

    if (tmp_buf) free(tmp_buf);
    return byte_count * 2;
}

// 获取本地无SSL的测试IP和端口
// 返回格式为 "ip:port"
char* https_get_test_ip_port(char* url)
{
    char *ip_port = NULL;
    http_body_obj http_body_buf;

    int ret = http_request_with_body(url, NULL, &http_body_buf);

    if (ret != HERROR_OK) {
        if (http_body_buf.p) free(http_body_buf.p);
        return NULL;
    } else {
        if (http_body_buf.recv_len > 0) {
            cJSON *nroot = cjson_parse(http_body_buf.p);
            const char* url_tmp = cjson_get_string_value(nroot, TEST_LOCOL_SERVER);
            if (url_tmp) {
                ip_port = strdup(url_tmp); // 复制一份，防止悬垂指针
                log_i("PARSE_url=%s", ip_port);
            } else {
                log_i("PARSE_url Error!!\n\n");
            }
            cJSON_Delete(nroot); // 用完后释放
        }
    }
    if (http_body_buf.p) free(http_body_buf.p);
    return ip_port;
}

// 获取带Token的会话url
char *get_wss_connection_url()
{
    char * url_api = (char *)malloc(BUF_SIZE);
    if (!url_api) return NULL;

    // 1. 获取IP:PORT
    char* ip_port = https_get_test_ip_port(TEST_IP_GET);
    if (!ip_port) {
        free(url_api);
        return NULL;
    }

    // 2. 直接构造 ws://IP:PORT/ 连接地址
    // 不再请求时间戳和签名，因为测试服务器不支持
    snprintf(url_api, BUF_SIZE, "ws://%s/", ip_port);
    log_i("simple test connection url=%s", url_api);

    free(ip_port); // 释放strdup的内存
    
    return url_api;
}

bool get_SN(char* target) {
    // Dummy implementation if needed by other modules, or remove if safe
    if (target) target[0] = '\0';
    return false; 
}
