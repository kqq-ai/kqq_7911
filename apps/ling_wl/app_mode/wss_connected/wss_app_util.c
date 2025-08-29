#include "wss_app_util.h"
#include "os/os_api.h"

#include "protocol/odm/odmp.h"
#include "app_mode/lwl_app_core.h"
#include "wss/wss_client.h"

#include "system/includes.h"
#include "app_lingwl.h"

#include <string.h>
#include <stdlib.h>

#define MAX_TARGET_SN_SET 16
#define SN_LEN 16

char sn_c_id[25];
static int g_exception_exit = false;
static OS_MUTEX g_wss_mutex;
static char* target_sn_set[MAX_TARGET_SN_SET] = {0};
static int target_sn_count = 0;

// 添加SN到集合
void add_target_sn(const char* sn) {
    if (!sn) return;
    os_mutex_pend(&g_wss_mutex, 0);
    // 检查是否已存在
    for (int i = 0; i < target_sn_count; ++i) {
        if (strncmp(target_sn_set[i], sn, SN_LEN) == 0) {
            os_mutex_post(&g_wss_mutex);
            return; // 已存在
        }
    }
    if (target_sn_count >= MAX_TARGET_SN_SET) {
        os_mutex_post(&g_wss_mutex);
        return;
    }
    target_sn_set[target_sn_count] = (char*)malloc(SN_LEN);
    strncpy(target_sn_set[target_sn_count], sn, SN_LEN - 1);
    target_sn_set[target_sn_count][SN_LEN - 1] = '\0';
    target_sn_count++;
    os_mutex_post(&g_wss_mutex);
}

// 从集合中删除SN
void remove_target_sn(const char* sn) {
    if (!sn) return;
    os_mutex_pend(&g_wss_mutex, 0);
    for (int i = 0; i < target_sn_count; ++i) {
        if (strncmp(target_sn_set[i], sn, SN_LEN) == 0) {
            free(target_sn_set[i]);
            for (int j = i; j < target_sn_count - 1; ++j) {
                target_sn_set[j] = target_sn_set[j + 1];
            }
            target_sn_set[target_sn_count - 1] = NULL;
            target_sn_count--;
            break;
        }
    }
    os_mutex_post(&g_wss_mutex);
}

// 清空集合
void clear_target_sn_set() {
    os_mutex_pend(&g_wss_mutex, 0);
    for (int i = 0; i < target_sn_count; ++i) {
        free(target_sn_set[i]);
        target_sn_set[i] = NULL;
    }
    target_sn_count = 0;
    os_mutex_post(&g_wss_mutex);
}

// 查询SN是否在集合中
int is_target_sn_in_set(const char* sn) {
    if (!sn) return 0;
    int found = 0;
    os_mutex_pend(&g_wss_mutex, 0);
    for (int i = 0; i < target_sn_count; ++i) {
        if (strncmp(target_sn_set[i], sn, SN_LEN) == 0) {
            found = 1;
            break;
        }
    }
    os_mutex_post(&g_wss_mutex);
    return found;
}

// 获取当前集合及数量
char** get_target_sn_set(int* count) {
    os_mutex_pend(&g_wss_mutex, 0);
    if (count) *count = target_sn_count;
    char** set = target_sn_set;
    os_mutex_post(&g_wss_mutex);
    return set;
}

void wss_exception_exit()
{
    g_exception_exit = true;
}

bool wss_check_exit()
{
    lwl_app_context_t *context = lwl_app_core_context_get_instance();
    struct websocket_struct *websockets_info = context->websockets_info;
    if (!websockets_info) {
        wss_exception_exit();
        log_i("websockets_info is NULL");
    }
    if (websockets_info->ping_thread_id == 0) {
        wss_exception_exit();
        log_i("websockets_client_recv_thread have exited!!!");
    }
    if (websockets_info->recv_thread_id == 0) {
        wss_exception_exit();
        log_i("websocket_client_heart have exited!!!");
    }
    return g_exception_exit;
}

bool wss_connect() {
    if (!os_mutex_valid(&g_wss_mutex)) {
        os_mutex_create(&g_wss_mutex);
    }
    os_mutex_pend(&g_wss_mutex, 0);
    lwl_app_context_t *context = lwl_app_core_context_get_instance();

    char* wss_url = get_wss_connection_url();
    if (wss_url == NULL) {
        os_mutex_post(&g_wss_mutex);
        return false;
    }
    context->websockets_info = wss_server_connect(wss_url);
    free(wss_url);
    if (context->websockets_info == NULL) {
        os_mutex_post(&g_wss_mutex);
        return false;
    }
    g_exception_exit = false;
    log_i("websockets init succ\n");
    get_SN(sn_c_id);
    os_mutex_post(&g_wss_mutex);
    return true;
}

void wss_disconnect() {
    os_mutex_pend(&g_wss_mutex, 0);
    lwl_app_context_t *context = lwl_app_core_context_get_instance();
    if (context->websockets_info != NULL) {
        wss_server_disconnect(context->websockets_info);
        context->websockets_info = NULL;
    }
    log_mqtt_stop();
    os_mutex_post(&g_wss_mutex);
}

int send_audio_status(int type, int event) 
{
    os_mutex_pend(&g_wss_mutex, 0);
    lwl_app_context_t *ctx = lwl_app_core_context_get_instance();
    if (ctx->websockets_info == NULL) {
        wss_exception_exit();
        os_mutex_post(&g_wss_mutex);
        return -1;
    }

    char *send_data = odmp_audio_status(sn_c_id, type, event);

    if (FALSE == wss_server_send(ctx->websockets_info, send_data, strlen(send_data))) {
        log_e("Error %s %d.", __FILE__, __LINE__);

        free(send_data);
        
        if (ctx->websockets_info != NULL) {
            wss_server_disconnect(ctx->websockets_info);
            ctx->websockets_info = NULL;
        }
        wss_exception_exit();
        os_mutex_post(&g_wss_mutex);
        return -1;
    }
    log_i("wbs send msg : %s", send_data);
    free(send_data);
    os_mutex_post(&g_wss_mutex);
    return 0;
}

int send_req_message(const char *send_id, bool is_play, int event_type) 
{
    os_mutex_pend(&g_wss_mutex, 0);
    lwl_app_context_t *ctx = lwl_app_core_context_get_instance();
    if (ctx->websockets_info == NULL) {
        wss_exception_exit();
        os_mutex_post(&g_wss_mutex);
        return -1;
    }

    char *send_data = odmp_req_message(target_sn_set, target_sn_count, sn_c_id, send_id, is_play, event_type);

    if (FALSE == wss_server_send(ctx->websockets_info, send_data, strlen(send_data))) {
        log_e("Error %s %d.", __FILE__, __LINE__);

        free(send_data);

        if (ctx->websockets_info != NULL) {
            wss_server_disconnect(ctx->websockets_info);
            ctx->websockets_info = NULL;
        }
        wss_exception_exit();
        os_mutex_post(&g_wss_mutex);
        return -1;
    }
    log_i("wbs send msg : %s", send_data);
    free(send_data);
    os_mutex_post(&g_wss_mutex);
    return 0;
}

int send_bin_data(uint8_t *data, uint32_t len)
{
    os_mutex_pend(&g_wss_mutex, 0);
    lwl_app_context_t *ctx = lwl_app_core_context_get_instance();
    if (ctx->websockets_info == NULL) {
        wss_exception_exit();
        os_mutex_post(&g_wss_mutex);
        return -1;
    }

    if (FALSE == wss_server_send_bin(ctx->websockets_info, data, len)) {
        log_e("Error %s %d.", __FILE__, __LINE__);
        
        if (ctx->websockets_info != NULL) {
            wss_server_disconnect(ctx->websockets_info);
            ctx->websockets_info = NULL;
        }
        wss_exception_exit();
        os_mutex_post(&g_wss_mutex);
        return -1;
    }
    // log_i("wbs send msg : %d", len);
    os_mutex_post(&g_wss_mutex);
    return 0;
}
