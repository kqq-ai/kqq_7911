#include "odm_secure.h"


#include "http/http_cli.h"
#include "cJSON_common/cJSON.h"
#include "mbedtls/gcm.h"
#include "time.h"

#include "util/cjson_util.h"

#define BUF_SIZE 128

char pt[35] = "ZLCNC5E21QEEFE1|1749016797555";
char *kqqToken;
#define HTTP_POSTTK_DEMO_URL  "https://test.coolqq.com/api/signature"
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
    ctx->url = HTTP_POSTTK_DEMO_URL;
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
        free(ctx);
        ctx = NULL;
    }
    printf("\n\n got token: %s  ~\n\n", kqqToken);
    return true;//PARSE_Error
}


static double https_get_timestamp(char* url)
{
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
    printf("url %s\n\n", url);
    ctx->url = url;
    ctx->timeout_millsec = 5000;
    ctx->priv = &http_body_buf;
    ctx->connection = "close";

    printf("httpcli_get start\n\n");
    int ret = httpcli_get(ctx);

    double time_stamp_web = 0;

    if (ret != HERROR_OK) {
        printf("httpcli_get false\n");
        free(http_body_buf.p);
        free(ctx);
        return 0;
    } else {
        if (http_body_buf.recv_len > 0) {
            printf("JSON:%s  ~~~~~\n", http_body_buf.p);
            const cJSON *nroot = cjson_parse(http_body_buf.p);

            cJSON *item = cJSON_GetObjectItem(nroot, "timestamp");
            if (cJSON_IsNumber(item)) {
                time_stamp_web = item->valuedouble;
                printf("timestamp: %.0f\n", time_stamp_web);
            } else {
                printf("Item is not a number.\n");
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
    char sn_E1[] = "ZLCNC5E21QEEFE1";
    char pt[35] = "ZLCNC5E21QEEFE1|1749016797555";
    //"ZLCNC5E21QEEEK0|1749005060531";
    ////        123456789012345678901234567890
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
    double time_web = https_get_timestamp("https://coolqq.com/api/timestamp");
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
        msleep(100);
    }
}

char *get_wss_connection_url()
{
    prepare_signature();

    char * url = (char *)malloc(BUF_SIZE);

    snprintf(url, BUF_SIZE, "wss://test.coolqq.com/api/socket?deviceId=%s&token=%s", DEVICE_ID, kqqToken);

    printf(" f link: %s  ~\n", url);
    
    return url;
}