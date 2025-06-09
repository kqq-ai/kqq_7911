#include "odm_secure.h"


#include "http/http_cli.h"
#include "cJSON_common/cJSON.h"
#include "mbedtls/gcm.h"
#include "time.h"

#include "util/cjson_util.h"

#define BUF_SIZE 128
#define HTTP_POSTTK_DEMO_URL  "https://test.coolqq.com/api/signature"

static char *kqqToken = NULL;
static char post_sign_time[264];

static void prepare_signature()
{
    int ret;

    //ZLCNC5E21QEEFE1
    char in[] = {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11,
                 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11
                };
    char enc_out[16];

    char dec_out[16];
    /*char key[64] = {0x39, 0x6f, 0x37, 0x6c, 0x4c, 0x6f, 0x48, 0x70,
                    0x7a, 0x67, 0x47, 0x69, 0x37, 0x4e, 0x33, 0x4d
                   };*/
    char key[64] = {0x49, 0x56, 0x4f, 0x75, 0x52, 0x33, 0x6b, 0x52, 0x4a, 0x30, 0x6b, 0x30, 0x30, 0x47, 0x47, 0x4f};
    //char key[64] = {0xa9, 0xf4, 0xc1, 0xd2, 0x8b, 0x7e, 0x3f, 0x05, 0xc6, 0xd2, 0x1a, 0x8e, 0x45, 0xb9, 0xe7, 0x30};
    //0x39, 0x6f, 0x37, 0x6c, 0x4c, 0x6f, 0x48, 0x70,
                    //0x7a, 0x67, 0x47, 0x69, 0x37, 0x4e, 0x33, 0x4d
                   //};

    const size_t iv_len = 12;
    static const unsigned char iv[12] = {
        0x75, 0x00, 0x52, 0x56, 0xdb, 0x21, 0xcc, 0xc7, 0x90, 0x55, 0x28, 0x1b  };
        //0xce, 0x63, 0xf7, 0x1b, 0xe8, 0x85, 0x7f, 0xe3, 0xc0, 0xa8, 0x9e, 0xe3  };
        //0xca, 0xfe, 0xba, 0xbe, 0xfa, 0xce, 0xdb, 0xad,
        //0xde, 0xca, 0xf8, 0x88  };
    
    const size_t add_len = 20;
     unsigned char additional[20] = {
        0xfe, 0xed, 0xfa, 0xce, 0xde, 0xad, 0xbe, 0xef,
        0xfe, 0xed, 0xfa, 0xce, 0xde, 0xad, 0xbe, 0xef,
        0xab, 0xad, 0xda, 0xd2  };

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
    memset(additional, 0, 20);


    mbedtls_gcm_context ctx;
    mbedtls_cipher_id_t cipher = MBEDTLS_CIPHER_ID_AES;

    log_i("aes GCM init begin\n");
    ///struct tm timeinfo = {0};
    time_t timestamp = time(NULL) ; /// 东八区+ 28800
    // 将时间戳直接转换为字符串
    char timestamp_str[21];  // 足够存储64位整数（最长20位数字+终止符）
    snprintf(timestamp_str, sizeof(timestamp_str), "%ld", (long)timestamp);
    // 直接用strlen()获取实际长度（更简单）
    size_t actual_length = strlen(timestamp_str);
    memcpy(pt + 16, timestamp_str, actual_length);  // SN + 1
    memcpy(pt + 16 + actual_length, "910", 3);      // add ms,  3 bytes

    printf("\n\n\n\ntimestamp_len:%d   ~~~~~   %s ~\n\n\n\n", actual_length, pt);

    ///localtime_r(&timestamp, &timeinfo);
    ///https_time_test_task();

    /// 018]passed .  mbedtls_gcm_self_test(1);
    mbedtls_gcm_init(&ctx);
    mbedtls_gcm_setkey(&ctx, cipher, key, 128);
    ///printf("mbedtls_gcm_setkey(&ctx, ciph~\n" );

    
    ret = mbedtls_gcm_crypt_and_tag(&ctx, MBEDTLS_GCM_ENCRYPT,
                                            pt_len,
                                            iv, iv_len,
                                            NULL, 0,
                                            pt, buf, 16, tag_buf);
                            /* (mbedtls_gcm_context *ctx,    固定格式
                              int mode,                      固定格式
                              size_t length,                明文长度
                              const unsigned char *iv,       iv
                              size_t iv_len,                 iv长度
                              const unsigned char *add,      add,, additional, add_len,
                              size_t add_len,                add长度
                              const unsigned char *input,    明文
                              unsigned char *output,        输出
                              size_t tag_len,                固定格式
                              unsigned char *tag)  */
    printf("\n\n  finish iv[0] :" );
    for (int i = 0; i<iv_len;i++) {
        ///printBinary( iv[i] );
    }
    
    ///printf("\n\n  finish buf[0] :" );
    for (int i = 0; i<pt_len;i++) {
        ///printBinary( buf[i] );
    }
    
    ///printf("\n\n  finish tag_buf[0] :" );
    for (int i = 0; i<16;i++) {
        ///printBinary( tag_buf[i] );
    }
    ///printf("%d\n", sizeof(buf));
    memcpy(buf + pt_len, tag_buf, 16);

    mbedtls_gcm_free(&ctx);
    printf("\n\n  finish _gcm_ \n\n" );
    size_t base64_len_iv = 0;
    ret = mbedtls_base64_encode(base64_iv, BUF_SIZE,  &base64_len_iv,  iv, iv_len);
    ///printf("\n\n  %d nonceBase64-IV %s \n\n", base64_len_iv, base64_iv );

    size_t base64_len = 0;
    ret = mbedtls_base64_encode(base64_buf, BUF_SIZE,  &base64_len,  buf, pt_len+16);
    ///printf("\n\n  %d ciphertextBase64 %s \n\n", base64_len, base64_buf);

    base64_buf[base64_len] = '|';
    memcpy(base64_buf + base64_len + 1, base64_iv, base64_len_iv);
        // 计算总长度：查找第一个零字节
    int total_length = 0;
    while (total_length < BUF_SIZE && base64_buf[total_length] != 0) {
        total_length++;
    }
    printf("\n\n\n\n%d   ~~~~~   %s\n\n\n\n", total_length, base64_buf);

    memset(&post_sign_time, 0x0, sizeof(post_sign_time));
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "sign", base64_buf);
    cJSON_AddStringToObject(root, "sn", sn_E1);
    double time_stamp_double = timestamp;
    cJSON_AddNumberToObject(root, "timestamp", time_stamp_double * 1000 + 910 );
    char *json_str = cJSON_PrintUnformatted(root);

    memcpy(post_sign_time, json_str, strlen(json_str)); //整合结果，存储在  post_sign_time
    printf("\n\n\n\n%d   !!!   %s\n\n\n\n", strlen(json_str), post_sign_time);

    // 清理资源
    free(json_str);
    cJSON_Delete(root);
}

static void https_post_gettk_task()// void *priv
{
    int ret = 0;
    http_body_obj http_body_buf;
    httpcli_ctx *ctx = (httpcli_ctx *)calloc(1, sizeof(httpcli_ctx));
    if (!ctx) {
        printf("calloc faile\n");
        return;
    }

    memset(&http_body_buf, 0x0, sizeof(http_body_obj));

    http_body_buf.recv_len = 0;
    http_body_buf.buf_len = 4 * 1024;
    http_body_buf.buf_count = 1;
    http_body_buf.p = (char *) malloc(http_body_buf.buf_len * http_body_buf.buf_count);
    if (!http_body_buf.p) {
        free(ctx);
        return;
    }

    ctx->post_data = post_sign_time;
    //"{\"sign\":\"dcfaaiUTkFAAgLjNtUqjeTPl5vQrLUBRN7+9K9VhjTVZyrC2IMhiBtcW/Abd|dQBSVtshzMeQVSgb\",\"sn\":\"ZLCNC5E21QEEFE1\",\"timestamp\":1749053990010}";
    //ctx->po
    ctx->data_len = strlen(ctx->post_data);
    printf("http get test  %d start\n\n",ctx->data_len);
    ctx->data_format = "application/json"; ///"application/json";text/plain
    ctx->url = HTTP_POSTTK_DEMO_URL;
    ctx->timeout_millsec = 5000;
    ctx->priv = &http_body_buf;
    ctx->connection = "close";

    printf("http get test start\n\n");
    ret = httpcli_post(ctx);
    printf("http post test run.\n\n");

    if (ret != HERROR_OK) {
        printf("http client test faile\n");
    } else {
        if (http_body_buf.recv_len > 0) {
            printf("\nReceive %d Bytes from (%s)\n", http_body_buf.recv_len, HTTP_POSTTK_DEMO_URL);
            printf("token:%s  ~~~~~\n", http_body_buf.p);
            const cJSON *nroot = cjson_parse(http_body_buf.p);
            kqqToken = cjson_get_string_value(nroot, "token");
            //if (kqqToken  == NULL) {
            //    printf("PARSE_token Error!!\n\n");//return ;//PARSE_Error  
            //}
            //printf("token double double: %f\n", coolqq_timestamp);
            //coolqq_timestamp_u64 = coolqq_timestamp;
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
}

char *get_wss_connection_url()
{
    prepare_signature();

    https_post_gettk_task();

    
    char * url = (char *)malloc(330);

    snprintf(url, 330, "wss://test.coolqq.com/api/socket?deviceId=%s&token=%s", DEVICE_ID, kqqToken);

    /*char uri_hmac_sha1[330] = "OBJ_URL";
    // 生成签名
    char *uri_xSig = generateSignature(uri_hmac_sha1, "SbmaqJ9sBJlh79O4oJcckbJjmQ43q-SYkFgmrtof");
    if (uri_xSig == NULL) {
        log_i( "Failed to generate signature\n");
        return ;
    }
    // 释放分配的内存
    free(uri_xSig); */

    printf(" f link: %s  ~\n", url);
    
    return url;
}