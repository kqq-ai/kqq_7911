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

char snInVm[SN_SIZE+1]="\0";

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

#if 0
void get_Key_from_Char(){
    assert(hex2bytes(keyInCHAR, keyInVm) == KEY_SIZE);
}
#endif 

static void gen_SN()
{
    struct lfs_info info;
    lfs_file_t file;

#if 0
    get_Key_from_Char();
    // 新设备，写入pt 和 keyInVm
    int err = lfs_file_open(&g_lfs, &file, "SN_KEY.bin", 
                LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC);
    /* 3. 写入数据 */
    size_t bytes_written = lfs_file_write(&g_lfs, &file, snInVm, SN_SIZE );
    assert(bytes_written == SN_SIZE+1);
    bytes_written        = lfs_file_write(&g_lfs, &file, keyInVm, KEY_SIZE);
    assert(bytes_written == KEY_SIZE);
    log_i(" write LittleFS snInVm = %s ,  key len = %d \n", snInVm , bytes_written );
    lfs_file_close(&g_lfs, &file);

#else
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
#endif
}

bool get_SN(char* SN){
    if (snInVm[0] == 0) gen_SN();
    if (snInVm[0] == 0 || strlen(snInVm) != SN_SIZE || WRITE_SN_KEY) {
        log_i("❌get_SN NULL\n");
        snInVm[0] = 0;
        SN[0] = 0;
        return false;
    }
    snprintf(SN, KEY_SIZE, "%s", snInVm);
    log_i("🚀🚀🚀 get_SN %s\n", snInVm);
    return true;
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

static char* https_post_gettk_task(const char post_url[], char* context)
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
            const char* token_tmp = cjson_get_string_value(nroot, "token");
            if (token_tmp) {
                Token = strdup(token_tmp); // 复制一份
            }
            cJSON_Delete(nroot); // 用完后释放
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
    pt[SN_SIZE] = '|';
    unsigned char buf[BUF_SIZE];
    unsigned char tag_buf[BUF_SIZE];
    unsigned char base64_buf[BUF_SIZE];
    unsigned char base64_iv[BUF_SIZE];
    memset(buf, 0, BUF_SIZE);
    memset(tag_buf, 0, BUF_SIZE);
    memset(base64_buf, 0, BUF_SIZE);
    memset(base64_iv, 0, BUF_SIZE);


    mbedtls_gcm_context ctx;
    mbedtls_cipher_id_t cipher = MBEDTLS_CIPHER_ID_AES;

    memcpy(pt , snInVm, SN_SIZE);

    // 将时间戳直接转换为字符串
    double time_web = https_get_timestamp(server_url);
    char timestamp_str[21];   // 足够存储64位整数（最长20位数字+终止符）
    sprintf(timestamp_str, "%.0f", time_web);
    // 直接用strlen()获取实际长度（更简单）
    size_t actual_length = strlen(timestamp_str);
    memcpy(pt + SN_SIZE + 1, timestamp_str, actual_length);  // SN + 1
    mbedtls_gcm_init(&ctx);
    mbedtls_gcm_setkey(&ctx, cipher, keyInVm, 128);

    
    int ret = mbedtls_gcm_crypt_and_tag(&ctx, MBEDTLS_GCM_ENCRYPT,
                                            pt_len,
                                            iv, iv_len,
                                            NULL, 0,
                                            pt, buf, KEY_SIZE, tag_buf);
    memcpy(buf + pt_len, tag_buf, KEY_SIZE);

    mbedtls_gcm_free(&ctx);
    size_t base64_len_iv = 0;
    mbedtls_base64_encode(base64_iv, BUF_SIZE,  &base64_len_iv,  iv, iv_len);

    size_t base64_len = 0;
    mbedtls_base64_encode(base64_buf, BUF_SIZE,  &base64_len,  buf, pt_len + KEY_SIZE);

    base64_buf[base64_len] = '|';
    memcpy(base64_buf + base64_len + 1, base64_iv, base64_len_iv);
        // 计算总长度：查找第一个零字节
    int total_length = 0;
    while (total_length < BUF_SIZE && base64_buf[total_length] != 0) {
        total_length++;
    }

    char post_sign_time[264];
    memset(&post_sign_time, 0x0, sizeof(post_sign_time));
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "sign", base64_buf);
    cJSON_AddStringToObject(root, "sn", snInVm);
    double time_stamp_double = time_web;
    cJSON_AddNumberToObject(root, "timestamp", time_stamp_double );
    char *json_str = cJSON_PrintUnformatted(root);

    memcpy(post_sign_time, json_str, strlen(json_str)); //整合结果，存储在  post_sign_time

    // 清理资源
    free(json_str);
    cJSON_Delete(root);


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
    if (!get_SN(sn_c_id)) return NULL;
    char * url_api = (char *)malloc(BUF_SIZE);
#ifdef TEST_LOCOL_SERVER
    char* ip_port = https_get_test_ip_port(TEST_IP_GET);
    if (ip_port) {
        snprintf(server_url, BUF_SIZE, TEST_SERVER_API, "http", ip_port);
        log_i("test server_url=%s", server_url);
    }
    char * Token = prepare_signature(server_url);
    if (Token) snprintf(url_api, BUF_SIZE, TEST_FORMAT_API, "ws", ip_port, sn_c_id,  Token);
#else
    snprintf(server_url, BUF_SIZE, SERVER_API, "https", SERVER_DOMAIN);
    char * Token = prepare_signature(server_url);
    if (Token) snprintf(url_api, BUF_SIZE, FORMAT_API, "wss", SERVER_DOMAIN, sn_c_id,  Token);
#endif
    if (!Token) {
        free(url_api);
        return NULL;
    }
    return url_api;
}