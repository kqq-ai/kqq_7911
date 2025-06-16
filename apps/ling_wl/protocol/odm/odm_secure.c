#include "odm_secure.h"


#include "http/http_cli.h"
#include "cJSON_common/cJSON.h"
#include "mbedtls/gcm.h"
#include "time.h"

#include "util/cjson_util.h"

#define BUF_SIZE 128


#define DEVICE_SN "ZLCNC5E21QEEFE1"
#define SERVER_DOMAIN  "test.coolqq.com/api"
#define SERVER_API  "%s://%s/"



#define FORMAT_TIME  "%stimestamp"
#define FORMAT_SIGN  "%ssignature"
#define FORMAT_API  "%s://%s/socket?deviceId=%s&token=%s"


#define HTTP_POSTTK_API  "https://%s/signature"
#define HTTP_POSTTK_API  "https://%s/signature"

static char server_url[BUF_SIZE];


void get_SN(char* target){
    snprintf(target, 16, "%s", DEVICE_SN);
}

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

static void prepare_signature()
{
    char key[64] = {0x49, 0x56, 0x4f, 0x75, 0x52, 0x33, 0x6b, 0x52, 0x4a, 0x30, 0x6b, 0x30, 0x30, 0x47, 0x47, 0x4f};

    const size_t iv_len = 12;
    static const unsigned char iv[12] = {
        0x75, 0x00, 0x52, 0x56, 0xdb, 0x21, 0xcc, 0xc7, 0x90, 0x55, 0x28, 0x1b  };

    const size_t pt_len = 29;
    char sn_E1[] = DEVICE_SN;
    char pt[35] = DEVICE_SN;
    pt[15] = '|';
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

    log_i("aes GCM init begin\n");

    // 将时间戳直接转换为字符串
    double time_web = https_get_timestamp(server_url);
    char timestamp_str[21];   // 足够存储64位整数（最长20位数字+终止符）
    sprintf(timestamp_str, "%.0f", time_web);
    timestamp_str[13] = '\0';
    // 直接用strlen()获取实际长度（更简单）
    size_t actual_length = strlen(timestamp_str);
    memcpy(pt + 16, timestamp_str, actual_length);  // SN + 1
    mbedtls_gcm_init(&ctx);
    mbedtls_gcm_setkey(&ctx, cipher, key, 128);

    
    int ret = mbedtls_gcm_crypt_and_tag(&ctx, MBEDTLS_GCM_ENCRYPT,
                                            pt_len,
                                            iv, iv_len,
                                            NULL, 0,
                                            pt, buf, 16, tag_buf);
    memcpy(buf + pt_len, tag_buf, 16);

    mbedtls_gcm_free(&ctx);
    size_t base64_len_iv = 0;
    mbedtls_base64_encode(base64_iv, BUF_SIZE,  &base64_len_iv,  iv, iv_len);

    size_t base64_len = 0;
    mbedtls_base64_encode(base64_buf, BUF_SIZE,  &base64_len,  buf, pt_len+16);

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
        cJSON_AddStringToObject(root, "sn", sn_E1);
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
    snprintf(url_api, BUF_SIZE, FORMAT_API, "wss", SERVER_DOMAIN, DEVICE_ID,  kqqToken);
    
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
    snprintf(url_api, BUF_SIZE, FORMAT_API, "ws", ip_port, DEVICE_ID,  kqqToken);
    
    return url_api;
}