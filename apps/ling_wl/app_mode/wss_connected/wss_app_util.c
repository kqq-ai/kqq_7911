#include "wss_app_util.h"

#include "wss/wss_client.h"
#include "protocol/odm/odmp.h"
#include "protocol/lingos/loswssp.h"
#include "app_mode/lwl_app_core.h"

#define LING_UUID_BUFSIZE 37

#define UUID_FIXED "abcdef-123456"
#define NICK_NAME_FIXED "AC7911BB-testss"

/**
 * @name    generate_uuid_my
 * @brief   生成UUID字符串
 * @details
 *      时间戳部分占UUID的前8个字符
 *      随机数部分占后面12个字符
 *      UUID格式为xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx，其中x是随机十六进制数字，y是特定范围的随机十六进制数字
 * 
 * @param[out]  uuid_str    buf大小至少为37字节，返回字符串\0结尾
 */
static void generate_uuid_my(char *uuid_str) {
    uint8_t random_buf[12];
    uint32_t timestamp = timer_get_ms();

    memset(uuid_str, 0xFF, LING_UUID_BUFSIZE);

    // 获取时间戳部分
    snprintf(uuid_str, 9, "%08x", (unsigned long)timestamp);

    // 获取随机数部分
    get_random_number(random_buf, sizeof(random_buf));

    // printf_buf(random_buf, sizeof(random_buf));

    random_buf[2] = (random_buf[2] & 0x0F) | 0x40; // 版本4
    random_buf[4] = (random_buf[4] & 0x3F) | 0x80; // 变体1 (RFC 4122)

    // 格式化UUID字符串
    snprintf(uuid_str + 8, 6, "-%04x", ((unsigned int)(random_buf[0]) << 8) | random_buf[1]);
    snprintf(uuid_str + 13, 6, "-%04x", ((unsigned int)(random_buf[2]) << 8) | random_buf[3]);
    snprintf(uuid_str + 18, 6, "-%04x", ((unsigned int)(random_buf[4]) << 8) | random_buf[5]);
    snprintf(uuid_str + 23, 14, "-%04x%04x%04X", ((unsigned int)(random_buf[6]) << 8)| random_buf[7],
             ((unsigned int)(random_buf[8]) << 8) | random_buf[9],
             ((unsigned int)(random_buf[10]) << 8) | random_buf[11]);

    uuid_str[36] = '\0';

    // printf_buf(uuid_str, LING_UUID_BUFSIZE);
}

int communication_talk_message(char *content)
{
    lwl_app_context_t *ctx = lwl_app_core_context_get_instance();
    if (ctx->websockets_info == NULL)
        return -1;

    char uuid[LING_UUID_BUFSIZE];
    generate_uuid_my(uuid);
    
    char *send_data = NULL;
    loswssp_create_message_event_message_text(UUID_FIXED, content, &send_data);
    if (FALSE == wss_server_send(ctx->websockets_info, send_data, strlen(send_data))) {
        log_e("Error %s %d.", __FILE__, __LINE__);
        loswssp_destroy_message(send_data);

        if (ctx->websockets_info != NULL) {
            wss_server_disconnect(ctx->websockets_info);
            ctx->websockets_info = NULL;
        }
        return -1;
    }
    log_i(send_data);
    loswssp_destroy_message(send_data);
    return 0;
}

int communication_init_role(int ip_id)
{
    lwl_app_context_t *ctx = lwl_app_core_context_get_instance();
    if (ctx->websockets_info == NULL)
        return -1;

    char uuid[LING_UUID_BUFSIZE];
    generate_uuid_my(uuid);
    
    char *send_data = NULL;
    loswssp_create_message_event_session(UUID_FIXED, 
        ip_id, 
        NICK_NAME_FIXED, 
        true, 
        &send_data);
    if (FALSE == wss_server_send(ctx->websockets_info, send_data, strlen(send_data))) {
        log_e("Error %s %d.", __FILE__, __LINE__);
        loswssp_destroy_message(send_data);

        if (ctx->websockets_info != NULL) {
            wss_server_disconnect(ctx->websockets_info);
            ctx->websockets_info = NULL;
        }
        return -1;
    }
    log_i(send_data);
    loswssp_destroy_message(send_data);
    return 0;
}

int communication_session_switch(int session_id, int ip_id)
{
    lwl_app_context_t *ctx = lwl_app_core_context_get_instance();
    if (ctx->websockets_info == NULL)
        return -1;

    char uuid[LING_UUID_BUFSIZE];
    generate_uuid_my(uuid);
    
    char *send_data = NULL;
    loswssp_create_message_event_session_switch(UUID_FIXED, 
        session_id, 
        ip_id, 
        NICK_NAME_FIXED, 
        true, 
        true, 
        &send_data);
    if (FALSE == wss_server_send(ctx->websockets_info, send_data, strlen(send_data))) {
        log_e("Error %s %d.", __FILE__, __LINE__);
        loswssp_destroy_message(send_data);

        if (ctx->websockets_info != NULL) {
            wss_server_disconnect(ctx->websockets_info);
            ctx->websockets_info = NULL;
        }
        return -1;
    }
    log_i(send_data);
    loswssp_destroy_message(send_data);
    return 0;
}

int communication_message_asr()
{
    lwl_app_context_t *ctx = lwl_app_core_context_get_instance();
    if (ctx->websockets_info == NULL)
        return -1;

    char uuid[LING_UUID_BUFSIZE];
    generate_uuid_my(uuid);
    
    char *send_data = NULL;
    loswssp_create_message_event_message_asr(UUID_FIXED, &send_data);
    if (FALSE == wss_server_send(ctx->websockets_info, send_data, strlen(send_data))) {
        log_e("Error %s %d.", __FILE__, __LINE__);
        loswssp_destroy_message(send_data);

        if (ctx->websockets_info != NULL) {
            wss_server_disconnect(ctx->websockets_info);
            ctx->websockets_info = NULL;
        }
        return -1;
    }
    log_i(send_data);
    loswssp_destroy_message(send_data);
    return 0;
}

int communication_message_asr_start()
{
    lwl_app_context_t *ctx = lwl_app_core_context_get_instance();
    if (ctx->websockets_info == NULL)
        return -1;

    char uuid[LING_UUID_BUFSIZE];
    generate_uuid_my(uuid);
    
    char *send_data = NULL;
    loswssp_create_message_event_message_asr_start(UUID_FIXED, &send_data);
    if (FALSE == wss_server_send(ctx->websockets_info, send_data, strlen(send_data))) {
        log_e("Error %s %d.", __FILE__, __LINE__);
        loswssp_destroy_message(send_data);

        if (ctx->websockets_info != NULL) {
            wss_server_disconnect(ctx->websockets_info);
            ctx->websockets_info = NULL;
        }
        return -1;
    }
    log_i(send_data);
    loswssp_destroy_message(send_data);
    return 0;
}

int communication_message_asr_end()
{
    lwl_app_context_t *ctx = lwl_app_core_context_get_instance();
    if (ctx->websockets_info == NULL)
        return -1;

    char uuid[LING_UUID_BUFSIZE];
    generate_uuid_my(uuid);
    
    char *send_data = NULL;
    loswssp_create_message_event_message_asr_end(UUID_FIXED, &send_data);
    if (FALSE == wss_server_send(ctx->websockets_info, send_data, strlen(send_data))) {
        log_e("Error %s %d.", __FILE__, __LINE__);
        loswssp_destroy_message(send_data);

        if (ctx->websockets_info != NULL) {
            wss_server_disconnect(ctx->websockets_info);
            ctx->websockets_info = NULL;
        }
        return -1;
    }
    log_i(send_data);
    loswssp_destroy_message(send_data);
    return 0;
}

int communication_message_asr_data(char *data_base64)
{
    lwl_app_context_t *ctx = lwl_app_core_context_get_instance();
    if (ctx->websockets_info == NULL)
        return -1;

    char uuid[LING_UUID_BUFSIZE];
    generate_uuid_my(uuid);
    
    char *send_data = NULL;
    loswssp_create_message_event_message_asr_data(UUID_FIXED, data_base64, &send_data);
    if (FALSE == wss_server_send(ctx->websockets_info, send_data, strlen(send_data))) {
        log_e("Error %s %d.", __FILE__, __LINE__);
        loswssp_destroy_message(send_data);

        if (ctx->websockets_info != NULL) {
            wss_server_disconnect(ctx->websockets_info);
            ctx->websockets_info = NULL;
        }
        return -1;
    }
    loswssp_destroy_message(send_data);
    return 0;
}

int communication_message_stop()
{
    lwl_app_context_t *ctx = lwl_app_core_context_get_instance();
    if (ctx->websockets_info == NULL)
        return -1;

    char uuid[LING_UUID_BUFSIZE];
    generate_uuid_my(uuid);
    
    char *send_data = NULL;
    loswssp_create_message_event_message_stop(UUID_FIXED, &send_data);
    if (FALSE == wss_server_send(ctx->websockets_info, send_data, strlen(send_data))) {
        log_e("Error %s %d.", __FILE__, __LINE__);
        loswssp_destroy_message(send_data);

        if (ctx->websockets_info != NULL) {
            wss_server_disconnect(ctx->websockets_info);
            ctx->websockets_info = NULL;
        }
        return -1;
    }
    log_i(send_data);
    loswssp_destroy_message(send_data);
    return 0;
}

int communication_ping()
{
    lwl_app_context_t *ctx = lwl_app_core_context_get_instance();
    if (ctx->websockets_info == NULL)
        return -1;

    char uuid[LING_UUID_BUFSIZE];
    generate_uuid_my(uuid);

    char *send_data = NULL;
    loswssp_create_message_event_ping(UUID_FIXED, &send_data);
    if (FALSE == wss_server_send(ctx->websockets_info, send_data, strlen(send_data))) {
        log_e("Error %s %d.", __FILE__, __LINE__);
        loswssp_destroy_message(send_data);

        if (ctx->websockets_info != NULL) {
            wss_server_disconnect(ctx->websockets_info);
            ctx->websockets_info = NULL;
        }
        return -1;
    }
    log_i(send_data);
    loswssp_destroy_message(send_data);
    return 0;
}


int send_audio_status(const char *c_id, int type, int event) 
{
    lwl_app_context_t *ctx = lwl_app_core_context_get_instance();
    if (ctx->websockets_info == NULL)
        return -1;

    char *send_data = odmp_audio_status(c_id, type, event);

    if (FALSE == wss_server_send(ctx->websockets_info, send_data, strlen(send_data))) {
        log_e("Error %s %d.", __FILE__, __LINE__);

        free(send_data);
        
        if (ctx->websockets_info != NULL) {
            wss_server_disconnect(ctx->websockets_info);
            ctx->websockets_info = NULL;
        }
        return -1;
    }
    free(send_data);
    return 0;
}

int send_req_message(const char *c_id, const char *send_id, bool is_play, int event_type) 
{
    lwl_app_context_t *ctx = lwl_app_core_context_get_instance();
    if (ctx->websockets_info == NULL)
        return -1;

    char **targets = NULL;
    int targets_num = 0;
    make_5m_edev_targets(&targets, &targets_num);

    char *send_data = odmp_req_message(targets, targets_num, c_id, send_id, is_play, event_type);

    if (FALSE == wss_server_send(ctx->websockets_info, send_data, strlen(send_data))) {
        log_e("Error %s %d.", __FILE__, __LINE__);

        free(send_data);
        free_5m_edev_targets(targets, targets_num);

        if (ctx->websockets_info != NULL) {
            wss_server_disconnect(ctx->websockets_info);
            ctx->websockets_info = NULL;
        }
        return -1;
    }
    free(send_data);
    free_5m_edev_targets(targets, targets_num);
    return 0;
}

int send_bin_data(uint8_t *data, uint32_t len)
{
    lwl_app_context_t *ctx = lwl_app_core_context_get_instance();
    if (ctx->websockets_info == NULL)
        return -1;

    if (FALSE == wss_server_send_bin(ctx->websockets_info, data, len)) {
        log_e("Error %s %d.", __FILE__, __LINE__);
        
        if (ctx->websockets_info != NULL) {
            wss_server_disconnect(ctx->websockets_info);
            ctx->websockets_info = NULL;
        }
        return -1;
    }
    return 0;
}
