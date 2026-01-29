#include "odm_secure.h"
#include "app_cookie.h"

#include "http/http_cli.h"
#include "cJSON_common/cJSON.h"
#include "mbedtls/gcm.h"
#include "time.h"

#include "util/cjson_util.h"
#include "syscfg_id.h"
#include "lfs.h"
#include "app_mode/lwl_app_framework.h"

extern lfs_t g_lfs;

#define BUF_SIZE 128
#define KEY_SIZE 16

char snInVm[SN_SIZE+1];

char keyInVm[KEY_SIZE];

#define TEST_SERVER_API  "%s://%s/"
#define TEST_FORMAT_API  "%s://%s/socket?deviceId=%s&token=%s"

#define SERVER_API  "%s://%s/api/"
#define FORMAT_MQTT  "%sjwt/permanentEncrypt"
#define FORMAT_TIME  "%stimestamp"
#define FORMAT_SIGN  "%ssignature"
#define FORMAT_API  "%s://%s/api/socket?deviceId=%s&token=%s"
char pt[SN_SIZE+2+21];

char *get_url_api(char* url, char* format)
{
    char * api = (char *)malloc(BUF_SIZE);
    snprintf(api, BUF_SIZE, format, url);
    return api;
}

size_t hex2bytes(const char *hex_str, uint8_t *bytes)
{
    size_t len = strlen(hex_str);
    if (len % 2 != 0 || len > 2*KEY_SIZE) return 0;          // 长度必须是偶数

    size_t byte_count = len / 2;
    for (size_t i = 0; i < byte_count; ++i) {
        // 修复：添加边界检查，防止数组越界
        size_t idx_hi = i * 2;
        size_t idx_lo = i * 2 + 1;
        if (idx_lo >= len) {
            return 0;  // 越界，返回错误
        }
        char hi = hex_str[idx_hi];
        char lo = hex_str[idx_lo];

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

#if 0
void get_Key_from_Char(){
    assert(hex2bytes(keyInCHAR, keyInVm) == KEY_SIZE);
}
#endif 

static void get_SN_from_lfs()
{
    struct lfs_info info;
    lfs_file_t file;
    if (lfs_stat(&g_lfs, "SN_KEY.bin", &info) != LFS_ERR_OK) {
        return;
    }
    // assert(lfs_stat(&g_lfs, "SN_KEY.bin", &info) == LFS_ERR_OK);
    // 如果文件存在，get SN from LittleFS
    int ret_lfs = lfs_file_open(&g_lfs, &file, "SN_KEY.bin", LFS_O_RDONLY);
    if (ret_lfs < 0) {
        lfs_remove(&g_lfs, "SN_KEY.bin");// 如果open失败，删除
        log_e("Failed to open log file B: %d", ret_lfs);
    }else{
        lfs_ssize_t bytes_read = lfs_file_read(&g_lfs, &file, &snInVm, SN_SIZE);
        snInVm[SN_SIZE] = '\0';
        bytes_read = lfs_file_read(&g_lfs, &file, &keyInVm, KEY_SIZE);
        log_i(" read LittleFS snInVm = %s ,  key len = %d \n", snInVm , bytes_read ); //读取成功，第二次启动
    }
    lfs_file_close(&g_lfs, &file);
}

static void get_SN_from_OTP()
{
    struct lfs_info info;
    lfs_file_t file;

    int norflash_read_otp(u8 *buf, const int len, const int zone_len);
    u8 otp_buf[8 + SN_SIZE + KEY_SIZE + 1 + 1] = {0};   //明确最后一个字符为'\0'
    int ret_otp = norflash_read_otp(otp_buf, 8 + SN_SIZE + KEY_SIZE + 1 + 1, 8 + SN_SIZE + KEY_SIZE + 1 + 1);
    if (ret_otp != 0) {
        log_i("gen_SN norflash_read_otp fail \n");
        return;
    }
    if (strncmp((char *)(otp_buf + 8), "KC", 2) != 0) {
        log_i("gen_SN OTP not start with KC\n");
        return;
    }
    //把otp的sn部分copy出来
    memcpy(snInVm, otp_buf + 8, SN_SIZE + 1);
    //把otp的key部分copy出来
    memcpy(keyInVm, otp_buf + 8 + SN_SIZE + 1, KEY_SIZE);
    log_i("gen_SN get from OTP OK,  snInVm = %s ,  key len = %d \n", snInVm , KEY_SIZE );
}

bool get_SN(char* SN){
    if (snInVm[0] == 0) get_SN_from_lfs();
    if (snInVm[0] == 0 || strlen(snInVm) != SN_SIZE || WRITE_SN_KEY) {
        log_i("❌get_SN NULL\n");
        snInVm[0] = 0;
        SN[0] = 0;
        return false;
    }
    snprintf(SN, KEY_SIZE, "%s", snInVm);
    return true;
}

// 从URL中提取host和port
static void extract_host_from_url(const char* url, char* host, size_t host_size, int* port) {
    const char* http_protocol = "http://";
    const char* https_protocol = "https://";
    const char* host_start = NULL;
    const char* protocol = NULL;
    
    if (strncmp(url, https_protocol, strlen(https_protocol)) == 0) {
        protocol = https_protocol;
        *port = 443;  // 默认HTTPS端口
    } else if (strncmp(url, http_protocol, strlen(http_protocol)) == 0) {
        protocol = http_protocol;
        *port = 80;   // 默认HTTP端口
    } else {
        host[0] = '\0';
        return;
    }
    
    host_start = url + strlen(protocol);
    const char* host_end = strpbrk(host_start, ":/");
    
    if (host_end == NULL) {
        host_end = host_start + strlen(host_start);
    }
    
    size_t host_len = host_end - host_start;
    if (host_len >= host_size) {
        host_len = host_size - 1;
    }
    memcpy(host, host_start, host_len);
    host[host_len] = '\0';
    
    // 检查是否有端口号
    if (*host_end == ':') {
        *port = atoi(host_end + 1);
    }
}

// 打印HTTP请求头信息（打印所有headers内容）
static void print_http_headers(const char* url, const char* method, const char* context, httpcli_ctx* ctx) {
    char host[128] = {0};
    int port = 0;
    const char* path = NULL;
    
    extract_host_from_url(url, host, sizeof(host), &port);
    
    // 提取路径
    const char* path_start = strstr(url, host);
    if (path_start) {
        path_start += strlen(host);
        if (*path_start == ':') {
            path_start = strchr(path_start, '/');
        } else if (*path_start != '/') {
            path_start = strchr(path_start, '/');
        }
        path = path_start ? path_start : "/";
    } else {
        path = "/";
    }
    
    log_i("========== HTTP Request Headers ==========\n");
    log_i("Method: %s\n", method);
    log_i("URL: %s\n", url);
    log_i("Path: %s\n", path);
    
    // 打印 Host 头
    log_i("Host: %s", host);
    if ((port != 80 && port != 443) || (port == 80 && strstr(url, "https://") == NULL && strstr(url, "http://") != NULL)) {
        log_i(":%d", port);
    }
    log_i("\n");
    
    // 打印 Connection 头
    if (ctx->connection && strlen(ctx->connection) > 0) {
        log_i("Connection: %s\n", ctx->connection);
    }
    
    // 打印 Content-Type 和 Content-Length（如果是 POST 请求）
    if (context && strlen(context) > 0) {
        if (ctx->data_format && strlen(ctx->data_format) > 0) {
            log_i("Content-Type: %s\n", ctx->data_format);
        }
        if (ctx->data_len > 0) {
            log_i("Content-Length: %d\n", ctx->data_len);
        }
    }
    
    // 打印 Cookie（如果存在）
    if (ctx->cookie && strlen(ctx->cookie) > 0) {
        log_i("Cookie: %s\n", ctx->cookie);
    }
    
    // 打印用户自定义的 HTTP 头（逐行解析并打印所有内容）
    if (ctx->user_http_header && strlen(ctx->user_http_header) > 0) {
        const char* header_ptr = ctx->user_http_header;
        const char* line_start = header_ptr;
        
        while (*header_ptr != '\0') {
            // 查找行结束符（\r\n 或 \n）
            if (*header_ptr == '\r' && *(header_ptr + 1) == '\n') {
                // 打印这一行（如果非空）
                if (header_ptr > line_start) {
                    size_t line_len = header_ptr - line_start;
                    if (line_len > 0) {
                        char line_buf[256];
                        size_t copy_len = (line_len < sizeof(line_buf) - 1) ? line_len : sizeof(line_buf) - 1;
                        memcpy(line_buf, line_start, copy_len);
                        line_buf[copy_len] = '\0';
                        log_i("%s\n", line_buf);
                    }
                }
                header_ptr += 2;  // 跳过 \r\n
                line_start = header_ptr;
            } else if (*header_ptr == '\n') {
                // 打印这一行（如果非空）
                if (header_ptr > line_start) {
                    size_t line_len = header_ptr - line_start;
                    if (line_len > 0) {
                        char line_buf[256];
                        size_t copy_len = (line_len < sizeof(line_buf) - 1) ? line_len : sizeof(line_buf) - 1;
                        memcpy(line_buf, line_start, copy_len);
                        line_buf[copy_len] = '\0';
                        log_i("%s\n", line_buf);
                    }
                }
                header_ptr += 1;  // 跳过 \n
                line_start = header_ptr;
            } else {
                header_ptr++;
            }
        }
        
        // 打印最后一行（如果没有以换行符结尾）
        if (*line_start != '\0' && header_ptr > line_start) {
            log_i("%s\n", line_start);
        }
    }
    
    log_i("==========================================\n");
}

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
    /*
     * HTTPS handshake + first response often exceeds 100ms on real networks.
     * Too small timeout will cause mbedTLS recv to fail with -0x004c.
     */
    ctx->timeout_millsec = 8000;
    ctx->priv = http_body_buf;
    ctx->connection = "close";

    if (context && strlen(context) > 0) {
        ctx->post_data = (char*)context;
        ctx->data_len = strlen(ctx->post_data);
        ctx->data_format = "application/json";
        print_http_headers(url, "POST", context, ctx);
        ret = httpcli_post(ctx);
    } else {
        print_http_headers(url, "GET", NULL, ctx);
        ret = httpcli_get(ctx);
    }

    // 打印原始响应内容
    log_i("[http_request] url=%s, ret=%d, recv_len=%d\n", url, ret, http_body_buf->recv_len);
    if (http_body_buf->recv_len > 0 && http_body_buf->p) {
        log_i("[http_request] raw_resp=%s\n", http_body_buf->p);
    }

    // 关闭连接
    if (ret == 0) httpcli_close(ctx);

    if (ctx) {
        // ctx->url 指向外部字符串，不需要free
        free(ctx);
    }
    return ret;
}

static char* https_post_gettk_task(const char post_url[], char* context)
{
    int ret = 0;
    http_body_obj http_body_buf;
    char* Token = NULL;

    log_i("[https_post_gettk_task] url=%s\n", post_url ? post_url : "(null)");
    log_i("[https_post_gettk_task] req=%s\n", context ? context : "(null)");
    ret = http_request_with_body(post_url, context, &http_body_buf);

    if (ret != HERROR_OK) {
        log_i("[https_post_gettk_task] ret=%d, resp=(null)\n", ret);
        if (http_body_buf.p) free(http_body_buf.p);
        return Token;
    } else {
        if (http_body_buf.recv_len > 0) {
            log_i("[https_post_gettk_task] ret=%d, resp=%s\n", ret, http_body_buf.p ? http_body_buf.p : "(null)");
            const cJSON *nroot = cjson_parse(http_body_buf.p);
            const char* token_tmp = cjson_get_string_value(nroot, "token");
            if (token_tmp) {
                Token = strdup(token_tmp); // 复制一份
            }
            cJSON_Delete(nroot); // 用完后释放
        } else {
            log_i("[https_post_gettk_task] ret=%d, resp_len=0\n", ret);
        }
    }
    if (http_body_buf.p) free(http_body_buf.p);
    return Token;
}

static char* https_post_gettk_task2(const char post_url[], char* context)
{
    int ret = 0;
    http_body_obj http_body_buf;
    char* Token = NULL;

    ret = http_request_with_body(post_url, context, &http_body_buf);

    if (ret != HERROR_OK) {
        if (http_body_buf.p) free(http_body_buf.p);
        return Token;
    } else {
        if (http_body_buf.recv_len > 0) {
            const cJSON *nroot = cjson_parse(http_body_buf.p);
            // 10. 安全提取token
            cJSON *data = cJSON_GetObjectItem(nroot, "data");
            const char* token_tmp = cjson_get_string_value(data, "token");
            if (token_tmp) {
                Token = strdup(token_tmp); // 复制一份
            }
            cJSON_Delete(nroot); // 用完后释放
        }
    }
    if (http_body_buf.p) free(http_body_buf.p);
    return Token;
}

char *get_mqtt_token() {
    char url_api[BUF_SIZE];
    char server_url[BUF_SIZE];
    char context[BUF_SIZE];
    snprintf(server_url, BUF_SIZE, SERVER_API, "https", SERVER_DOMAIN);
    snprintf(url_api, BUF_SIZE, FORMAT_MQTT, server_url);

    
    char sn_c_id[SN_SIZE+1];
    if (!get_SN(sn_c_id)) return NULL;
    
    int body_len = snprintf(context, sizeof(context), "{\"sn\":\"%s\"}", sn_c_id);
    if (body_len < 0 || body_len >= sizeof(context)) {
        log_i("[fetch_mqtt_token] body construction failed\n");
        return NULL;
    }

    return https_post_gettk_task2(url_api, context);
}

static double https_get_timestamp(char* url)
{

    char url_api[BUF_SIZE];
    snprintf(url_api, BUF_SIZE, FORMAT_TIME, url);

    http_body_obj http_body_buf;
    int ret = http_request_with_body(url_api, NULL, &http_body_buf);

    double time_stamp_web = 0;

    if (ret != HERROR_OK) {
        if (http_body_buf.p) free(http_body_buf.p);
        return 0;
    } else {
        if (http_body_buf.recv_len > 0) {
            const cJSON *nroot = cjson_parse(http_body_buf.p);

            cJSON *item = cJSON_GetObjectItem(nroot, "timestamp");
            if (cJSON_IsNumber(item)) {
                time_stamp_web = item->valuedouble;
            } else {
                log_i("[https_get_timestamp]Item is not a number.\n");
            }
            cJSON_Delete(nroot); // 用完后释放
        }
    }
    if (http_body_buf.p) free(http_body_buf.p);
    return time_stamp_web;
}

// 准备签名
static char* prepare_signature(const char server_url[])
{
    if (server_url[0] == '\0') {
        log_i("prepare_signature server_url is NULL\n");
        return NULL;
    }

    const size_t iv_len = 12;
    static const unsigned char iv[12] = {
        0x75, 0x00, 0x52, 0x56, 0xdb, 0x21, 0xcc, 0xc7, 0x90, 0x55, 0x28, 0x1b  };

    const size_t pt_len = 29;
    unsigned char *buf = (unsigned char *)malloc(BUF_SIZE);
    unsigned char *tag_buf = (unsigned char *)malloc(BUF_SIZE);
    unsigned char *base64_buf = (unsigned char *)malloc(BUF_SIZE);
    unsigned char *base64_iv = (unsigned char *)malloc(BUF_SIZE);
    
    if (!buf || !tag_buf || !base64_buf || !base64_iv) {
        log_e("prepare_signature malloc failed\n");
        if (buf) free(buf);
        if (tag_buf) free(tag_buf);
        if (base64_buf) free(base64_buf);
        if (base64_iv) free(base64_iv);
        return NULL;
    }

    memset(buf, 0, BUF_SIZE);
    memset(tag_buf, 0, BUF_SIZE);
    memset(base64_buf, 0, BUF_SIZE);
    memset(base64_iv, 0, BUF_SIZE);
    memset(pt, 0, sizeof(pt));  // 初始化pt数组

    mbedtls_gcm_context ctx;
    mbedtls_cipher_id_t cipher = MBEDTLS_CIPHER_ID_AES;

    // 将时间戳直接转换为字符串
    double time_web = https_get_timestamp((char*)server_url);
    if (time_web == 0) {
        log_i("❌[prepare_signature] https_get_timestamp failed, aborting\n");
        free(buf); free(tag_buf); free(base64_buf); free(base64_iv);
        return NULL;
    }
    // 修复：将double转换为long long再格式化，避免精度问题
    long long timestamp_int = (long long)time_web;
    // 使用snprintf直接构建完整的pt字符串，更安全可靠
    int ret_snprintf = snprintf(pt, sizeof(pt), "%.*s|%lld", SN_SIZE, snInVm, timestamp_int);
    if (ret_snprintf < 0 || ret_snprintf >= (int)sizeof(pt)) {
        log_i("❌[prepare_signature] pt string construction failed, ret=%d, sizeof(pt)=%zu\n", ret_snprintf, sizeof(pt));
        free(buf); free(tag_buf); free(base64_buf); free(base64_iv);
        return NULL;
    }
    size_t actual_length = strlen(pt);
    if (actual_length == 0) {
        log_i("❌[prepare_signature] pt is empty, time_web=%.0f, timestamp_int=%lld\n", time_web, timestamp_int);
        free(buf); free(tag_buf); free(base64_buf); free(base64_iv);
        return NULL;
    }
    mbedtls_gcm_init(&ctx);
    mbedtls_gcm_setkey(&ctx, cipher, (const unsigned char*)keyInVm, 128);
    log_i("pt:%s", pt);
    
    int ret = mbedtls_gcm_crypt_and_tag(&ctx, MBEDTLS_GCM_ENCRYPT,
                                            pt_len,
                                            iv, iv_len,
                                            NULL, 0,
                                            (const unsigned char*)pt, buf, KEY_SIZE, tag_buf);
    memcpy(buf + pt_len, tag_buf, KEY_SIZE);

    mbedtls_gcm_free(&ctx);
    size_t base64_len_iv = 0;
    mbedtls_base64_encode(base64_iv, BUF_SIZE,  &base64_len_iv,  iv, iv_len);

    size_t base64_len = 0;
    mbedtls_base64_encode(base64_buf, BUF_SIZE,  &base64_len,  buf, pt_len + KEY_SIZE);

    // 修复：添加边界检查，防止缓冲区溢出
    if (base64_len + 1 + base64_len_iv > BUF_SIZE) {
        log_i("❌[prepare_signature] base64 buffer overflow: len=%zu, iv_len=%zu, max=%d\n", 
              base64_len, base64_len_iv, BUF_SIZE);
        free(buf); free(tag_buf); free(base64_buf); free(base64_iv);
        return NULL;
    }
    base64_buf[base64_len] = '|';
    memcpy(base64_buf + base64_len + 1, base64_iv, base64_len_iv);
        // 计算总长度：查找第一个零字节
    log_i("base64_buf:%s", (char*)base64_buf);

    char post_sign_time[264];
    memset(&post_sign_time, 0x0, sizeof(post_sign_time));
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "sign", (char*)base64_buf);
    cJSON_AddStringToObject(root, "sn", snInVm);
    double time_stamp_double = time_web;
    cJSON_AddNumberToObject(root, "timestamp", time_stamp_double );
    char *json_str = cJSON_PrintUnformatted(root);

    // 修复：添加边界检查，防止JSON字符串溢出
    if (json_str == NULL) {
        log_i("❌[prepare_signature] cJSON_PrintUnformatted returned NULL\n");
        cJSON_Delete(root);
        free(buf); free(tag_buf); free(base64_buf); free(base64_iv);
        return NULL;
    }
    size_t json_str_len = strlen(json_str);
    if (json_str_len >= sizeof(post_sign_time)) {
        log_i("❌[prepare_signature] JSON string too long: %zu >= %zu\n", 
              json_str_len, sizeof(post_sign_time));
        free(json_str);
        cJSON_Delete(root);
        free(buf); free(tag_buf); free(base64_buf); free(base64_iv);
        return NULL;
    }
    memcpy(post_sign_time, json_str, json_str_len); //整合结果，存储在  post_sign_time
    post_sign_time[json_str_len] = '\0';  // 确保终止符

    // 清理资源
    free(json_str);
    cJSON_Delete(root);
    free(buf);
    free(tag_buf);
    free(base64_buf);
    free(base64_iv);


    char post_url[BUF_SIZE];
    snprintf(post_url, BUF_SIZE, FORMAT_SIGN, server_url);

    return https_post_gettk_task(post_url, post_sign_time);
}


#ifdef TEST_LOCOL_SERVER
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
#endif

// 获取带Token的会话url
char *get_wss_connection_url()
{
    char server_url[BUF_SIZE] = "";
    char sn_c_id[SN_SIZE+1];
    char * Token = NULL;
    char * url_api = NULL;

    if (!get_SN(sn_c_id)) return NULL;
    
    url_api = (char *)malloc(512);
    if (!url_api) return NULL;

#ifdef TEST_LOCOL_SERVER
    log_i("🚀🚀🚀 get_wss_connection_url DEBUG MODE\n");
    char* ip_port = https_get_test_ip_port(TEST_IP_GET);
    if (ip_port) {
        snprintf(server_url, BUF_SIZE, TEST_SERVER_API, "http", ip_port);
        log_i("test server_url=%s", server_url);
    }
    Token = prepare_signature(server_url);
    if (Token) {
        snprintf(url_api, 512, TEST_FORMAT_API, "ws", ip_port ? ip_port : "", sn_c_id, Token);
    }
    if (ip_port) free(ip_port); // 释放ip_port
#else
    log_i("🚀🚀🚀 get_wss_connection_url PROD MODE\n");
    snprintf(server_url, BUF_SIZE, SERVER_API, "https", SERVER_DOMAIN);
    Token = prepare_signature(server_url);
    if (Token) {
        snprintf(url_api, 512, FORMAT_API, "wss", SERVER_DOMAIN, sn_c_id, Token);
    }
#endif

    if (!Token) {
        free(url_api);
        return NULL;
    }
    
    free(Token); // 使用完Token后必须释放，它是prepare_signature内部strdup出来的
    return url_api;
}