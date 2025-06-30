#include "odm_secure.h"

#include "http/http_cli.h"
#include "cJSON_common/cJSON.h"
#include "mbedtls/gcm.h"
#include "time.h"

#include "util/cjson_util.h"
#include "syscfg_id.h"
#include "lfs.h"

extern lfs_t g_lfs;

#define BUF_SIZE 128
#define SN_SIZE 15
#define KEY_SIZE 16

// #define WRITE_SN_KEY

#ifdef WRITE_SN_KEY
char snInVm[SN_SIZE+1] = "KCCNC5F12JEEEF7";
char keyInCHAR[KEY_SIZE*2] = "686771725153614a56514b307a617551";
#else
char snInVm[SN_SIZE+1];
#endif

char keyInVm[KEY_SIZE];

// #define SERVER_DOMAIN  "test.coolqq.com/api"
#define SERVER_DOMAIN  "coolqq.com/api"
#define SERVER_API  "%s://%s/"
#define FORMAT_TIME  "%stimestamp"
#define FORMAT_SIGN  "%ssignature"
#define FORMAT_API  "%s://%s/socket?deviceId=%s&token=%s"

static char server_url[BUF_SIZE];
char pt[35];

static void gen_SN();

void get_SN(char* SN){
    if (snInVm[0] == 0) gen_SN();
    snprintf(SN, KEY_SIZE, "%s", snInVm);
}

#ifdef WRITE_SN_KEY
size_t hex2bytes(const char *hex_str, uint8_t *bytes)
{
    size_t len = strlen(hex_str);
    if (len % 2 != 0) return 0;          // 长度必须是偶数

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

void get_Key_from_Char(){
    assert(hex2bytes(keyInCHAR, keyInVm) == KEY_SIZE);
}
#endif 

char *get_url_api(char* url, char* format)
{
    char * api = (char *)malloc(BUF_SIZE);
    snprintf(api, BUF_SIZE, format, url);
    printf("%s\n", api);
    return api;
}


char *kqqToken;
static bool https_post_gettk_task(char* context)// void *priv
{
    int ret = 0;
    http_body_obj http_body_buf;
    httpcli_ctx *ctx = (httpcli_ctx *)calloc(1, sizeof(httpcli_ctx));
    if (!ctx) {
        printf("calloc faile\n");
        return false;
    }

    memset(&http_body_buf, 0x0, sizeof(http_body_obj));

    http_body_buf.recv_len = 0;
    http_body_buf.buf_len = 4 * 1024;
    http_body_buf.buf_count = 1;
    http_body_buf.p = (char *) malloc(http_body_buf.buf_len * http_body_buf.buf_count);
    if (!http_body_buf.p) {
        free(ctx);
        return false;
    }

    ctx->post_data = context;
    ctx->data_len = strlen(ctx->post_data);
    ctx->data_format = "application/json"; 
    ctx->url = get_url_api(server_url, FORMAT_SIGN);
    ctx->timeout_millsec = 5000;
    ctx->priv = &http_body_buf;
    ctx->connection = "close";

    ret = httpcli_post(ctx);

    if (ret != HERROR_OK) {
        printf("httpcli_post false\n");
        free(http_body_buf.p);
        free(ctx);
        return false;
    } else {
        if (http_body_buf.recv_len > 0) {
            const cJSON *nroot = cjson_parse(http_body_buf.p);
            kqqToken = cjson_get_string_value(nroot, "token");
            if (kqqToken  == NULL) {
                printf("PARSE_token Error!!\n\n");
                free(http_body_buf.p);
                free(ctx);
                return false;//PARSE_Error
            }
        }
    }

    //关闭连接
    httpcli_close(ctx);

    if (http_body_buf.p) {
        free(http_body_buf.p);
        http_body_buf.p = NULL;
    }

    if (ctx) {
        if (ctx->url) free(ctx->url);
        free(ctx);
        ctx = NULL;
    }
    printf("\n\n got token: %s  ~\n\n", kqqToken);
    return true;//PARSE_Error
}


static double https_get_timestamp(char* url)
{

    char url_api[BUF_SIZE];
    snprintf(url_api, BUF_SIZE, FORMAT_TIME, url);
    printf("url %s\n\n", url_api);

    http_body_obj http_body_buf;
    
    httpcli_ctx *ctx = (httpcli_ctx *)calloc(1, sizeof(httpcli_ctx));
    if (!ctx) {
        printf("calloc faile\n");
        return 0;
    }

    memset(&http_body_buf, 0x0, sizeof(http_body_obj));

    http_body_buf.recv_len = 0;
    http_body_buf.buf_len = 4 * 1024;
    http_body_buf.buf_count = 1;
    http_body_buf.p = (char *) malloc(http_body_buf.buf_len * http_body_buf.buf_count);
    if (!http_body_buf.p) {
        free(ctx);
        return 0;
    }
    ctx->url = url_api;
    ctx->timeout_millsec = 5000;
    ctx->priv = &http_body_buf;
    ctx->connection = "close";

    int ret = httpcli_get(ctx);

    double time_stamp_web = 0;

    if (ret != HERROR_OK) {
        free(http_body_buf.p);
        free(ctx);
        return 0;
    } else {
        if (http_body_buf.recv_len > 0) {
            const cJSON *nroot = cjson_parse(http_body_buf.p);

            cJSON *item = cJSON_GetObjectItem(nroot, "timestamp");
            if (cJSON_IsNumber(item)) {
                time_stamp_web = item->valuedouble;
            } else {
                printf("[https_get_timestamp]Item is not a number.\n");
                return 0;
            }
        }
    }
    return time_stamp_web;
}

static void gen_SN()
{

    /// get SN from LittleFS
        // 1. 检查文件SN_KEY.bin是否存在
    struct lfs_info info;
    lfs_file_t file;

    
#ifdef WRITE_SN_KEY
    get_Key_from_Char();
    // 新设备，写入pt 和 keyInVm
    int err = lfs_file_open(&g_lfs, &file, "SN_KEY.bin", 
                LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC);
    /* 3. 写入数据 */
    size_t bytes_written = lfs_file_write(&g_lfs, &file, snInVm, SN_SIZE );
    assert(bytes_written == SN_SIZE);
    bytes_written        = lfs_file_write(&g_lfs, &file, keyInVm, KEY_SIZE);
    assert(bytes_written == KEY_SIZE);
    log_i(" write LittleFS snInVm = %s ,  key len = %d \n", snInVm , bytes_written );
    lfs_file_close(&g_lfs, &file);

#else
    assert(lfs_stat(&g_lfs, "SN_KEY.bin", &info) == LFS_ERR_OK);
    // 如果文件存在，读取
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
#endif
}

static void prepare_signature()
{
    ///{0x49, 0x56, 0x4f, 0x75, 0x52, 0x33, 0x6b, 0x52, 0x4a, 0x30, 0x6b, 0x30, 0x30, 0x47, 0x47, 0x4f

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

    gen_SN();
    memcpy(pt , snInVm, SN_SIZE);

    // 将时间戳直接转换为字符串
    double time_web = https_get_timestamp(server_url);
    char timestamp_str[21];   // 足够存储64位整数（最长20位数字+终止符）
    sprintf(timestamp_str, "%.0f", time_web);
    timestamp_str[13] = '\0';
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
    while (1) { //get Token
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

        if (https_post_gettk_task(post_sign_time)) break;
        printf("https_post_gettk_task false");
        msleep(3000);
    }
}

char *get_wss_connection_url()
{
    snprintf(server_url, BUF_SIZE, SERVER_API, "https", SERVER_DOMAIN);
    prepare_signature();

    char * url_api = (char *)malloc(BUF_SIZE);
    char sn_c_id[SN_SIZE+1];
    get_SN(&sn_c_id);
    snprintf(url_api, BUF_SIZE, FORMAT_API, "wss", SERVER_DOMAIN, sn_c_id,  kqqToken);
    
    return url_api;
}




char* https_get_test_ip_port(char* url)
{
    char * ip_port;

    http_body_obj http_body_buf;
    
    httpcli_ctx *ctx = (httpcli_ctx *)calloc(1, sizeof(httpcli_ctx));
    if (!ctx) {
        printf("calloc faile\n");
        return 0;
    }

    memset(&http_body_buf, 0x0, sizeof(http_body_obj));

    http_body_buf.recv_len = 0;
    http_body_buf.buf_len = 4 * 1024;
    http_body_buf.buf_count = 1;
    http_body_buf.p = (char *) malloc(http_body_buf.buf_len * http_body_buf.buf_count);
    if (!http_body_buf.p) {
        free(ctx);
        return 0;
    }
    ctx->url = url;
    ctx->timeout_millsec = 5000;
    ctx->priv = &http_body_buf;
    ctx->connection = "close";

    int ret = httpcli_get(ctx);

    double time_stamp_web = 0;

    if (ret != HERROR_OK) {
        free(http_body_buf.p);
        free(ctx);
        return 0;
    } else {
        if (http_body_buf.recv_len > 0) {
            const cJSON *nroot = cjson_parse(http_body_buf.p);

            ip_port = cjson_get_string_value(nroot, "url");
            if (ip_port  == NULL) {
                printf("PARSE_url Error!!\n\n");
                free(http_body_buf.p);
                free(ctx);
                return false;//PARSE_Error
            }
        }
    }
    free(http_body_buf.p);
    free(ctx);
    return ip_port;
}

#define TEST_IP_GET "https://kqq.tos-cn-beijing.volces.com/kqq_debug.json"

char *get_wss_test_url()
{

    char* ip_port = https_get_test_ip_port(TEST_IP_GET);
    printf("ip = %s", ip_port);
    snprintf(server_url, BUF_SIZE, SERVER_API, "http", ip_port);
    prepare_signature();

    char * url_api = (char *)malloc(BUF_SIZE);
    char sn_c_id[SN_SIZE+1];
    get_SN(&sn_c_id);
    snprintf(url_api, BUF_SIZE, FORMAT_API, "ws", ip_port, sn_c_id,  kqqToken);
    
    return url_api;
}