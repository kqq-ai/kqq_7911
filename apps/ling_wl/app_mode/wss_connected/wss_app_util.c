// Prevent ESTABLISHED collision
#define ESTABLISHED WS_ESTABLISHED_HIDDEN
#include "wss_app_util.h"
#include "wss/wss_client.h"
#undef ESTABLISHED
#include "lwip/tcp.h"
#include "lwip/priv/tcp_priv.h"
#include "os/os_api.h"
#include "protocol/odm/odmp.h"
#include "app_mode/lwl_app_core.h"
#include "system/includes.h"
#include "protocol/odm/odm_secure.h"
#include <string.h>
#include <stdlib.h>

#define MAX_TARGET_SN_SET 16
#define SN_LEN 16

char sn_c_id[25];
static int g_exception_exit = true;
static char g_last_disconnect_reason[64] = "boot";
static OS_MUTEX g_wss_mutex;
static char* target_sn_set[MAX_TARGET_SN_SET] = {0};
static int target_sn_count = 0;

// 添加SN到集合
void add_target_sn(const char* sn) {
    if (!sn) return;
    if (strlen(sn)!= SN_SIZE-1) return;
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

static void wss_mark_exception(const char *reason)
{
    strncpy(g_last_disconnect_reason, reason ? reason : "unknown", sizeof(g_last_disconnect_reason) - 1);
    g_last_disconnect_reason[sizeof(g_last_disconnect_reason) - 1] = '\0';
    log_i("[wss_app_util]exception flag set: %s", g_last_disconnect_reason);
    g_exception_exit = true;
}

void wss_exception_exit()
{
    wss_mark_exception("generic");
}

const char *wss_get_last_disconnect_reason(void)
{
    return g_last_disconnect_reason;
}

bool wss_check_exit()
{
    if (g_exception_exit) {
        return true;
    }
    lwl_app_context_t *context = lwl_app_core_context_get_instance();
    struct websocket_struct *websockets_info = context->websockets_info;
    if (!websockets_info) {
        wss_mark_exception("websocket info NULL");
        log_e("[wss_app_util]websockets_info is NULL");
    } else {
        if (websockets_info->recv_thread_id == 0) {
            wss_mark_exception("recv_thread_id=0");
            log_e("[wss_app_util]websocket_client_heart have exited!!!");
        }
    }
    return g_exception_exit;
}

char* wss_url = NULL;

bool wss_connect() {
    if (!os_mutex_valid(&g_wss_mutex)) {
        os_mutex_create(&g_wss_mutex);
    }
    os_mutex_pend(&g_wss_mutex, 0);
    lwl_app_context_t *context = lwl_app_core_context_get_instance();

    log_i("[wss_app_util]wss_connect start");
    wss_url = get_wss_connection_url();
    if (wss_url == NULL) {
        log_e("[wss_app_util]get_wss_connection_url failed wss_url=NULL");
        os_mutex_post(&g_wss_mutex);
        return false;
    }
    context->websockets_info = wss_server_connect(wss_url);
    if (context->websockets_info == NULL) {
        log_e("[wss_app_util]wss_server_connect failed websockets_info=NULL");
        os_mutex_post(&g_wss_mutex);
        return false;
    }
    g_exception_exit = false;
    strncpy(g_last_disconnect_reason, "connected", sizeof(g_last_disconnect_reason) - 1);
    g_last_disconnect_reason[sizeof(g_last_disconnect_reason) - 1] = '\0';
    log_i("[wss_app_util]websockets init succ url=%s\n", wss_url);
    get_SN(sn_c_id);
    os_mutex_post(&g_wss_mutex);
    return true;
}

void wss_disconnect() {
    os_mutex_pend(&g_wss_mutex, 0);
    lwl_app_context_t *context = lwl_app_core_context_get_instance();
    if (context->websockets_info != NULL) {
        log_e("[wss_app_util]wss_disconnect closing active ws (last_reason=%s)", g_last_disconnect_reason);
        wss_server_disconnect(context->websockets_info);
        context->websockets_info = NULL;
        free(wss_url);
        wss_url = NULL;
    }
    wss_mark_exception("wss_disconnect manual");
    // log_mqtt_stop();//xTaskSwitchContext = 0
    os_mutex_post(&g_wss_mutex);
}

bool wss_reconnect() {
    if (!os_mutex_valid(&g_wss_mutex)) {
        os_mutex_create(&g_wss_mutex);
    }
    os_mutex_pend(&g_wss_mutex, 0);
    lwl_app_context_t *context = lwl_app_core_context_get_instance();
    if (context->websockets_info != NULL) {
        log_e("[wss_app_util]websockets disconnect by wss_reconnect");
        wss_server_disconnect(context->websockets_info);
        context->websockets_info = NULL;
    }
    if (wss_url == NULL) {
        wss_url = get_wss_connection_url();
    } else {
        msleep(100); // 断线后等待100ms再重连
    }
    log_i("[wss_app_util]websockets reconnected %s",wss_url);

    if (wss_url == NULL) {
        os_mutex_post(&g_wss_mutex);
        return false;
    }
    log_i("[wss_app_util]attempting reconnect %s", wss_url);
    context->websockets_info = wss_server_connect(wss_url);
    if (context->websockets_info == NULL) {
        os_mutex_post(&g_wss_mutex);
        return false;
    }
    g_exception_exit = false;
    strncpy(g_last_disconnect_reason, "reconnected", sizeof(g_last_disconnect_reason) - 1);
    g_last_disconnect_reason[sizeof(g_last_disconnect_reason) - 1] = '\0';
    log_i("[wss_app_util]websockets reconnected succ");
    os_mutex_post(&g_wss_mutex);
    return true;
}

int send_audio_status(int type, int event) 
{
    os_mutex_pend(&g_wss_mutex, 0);
    lwl_app_context_t *ctx = lwl_app_core_context_get_instance();
    if (ctx->websockets_info == NULL) {
        wss_mark_exception("send_audio_status ctx->websockets_info NULL");
        os_mutex_post(&g_wss_mutex);
        return -1;
    }

    char *send_data = odmp_audio_status(sn_c_id, type, event);

    if (FALSE == wss_send_message(ctx->websockets_info, (u8 *)send_data, strlen(send_data))) {
        log_e("[wss_app_utils]Error %s %d. type=%d event=%d", __FILE__, __LINE__, type, event);

        free(send_data);
        
        if (ctx->websockets_info != NULL) {
            wss_server_disconnect(ctx->websockets_info);
            ctx->websockets_info = NULL;
        }
        wss_mark_exception("send_audio_status send failed");
        os_mutex_post(&g_wss_mutex);
        return -1;
    }
    log_i("[wss_app_util]wbs send msg : %s", send_data);
    free(send_data);
    os_mutex_post(&g_wss_mutex);
    return 0;
}

int send_audio_format(int type) 
{
    os_mutex_pend(&g_wss_mutex, 0);
    lwl_app_context_t *ctx = lwl_app_core_context_get_instance();
    if (ctx->websockets_info == NULL) {
        wss_mark_exception("send_audio_format ctx->websockets_info NULL");
        os_mutex_post(&g_wss_mutex);
        return -1;
    }

    char *send_data = odmp_set_audio_format(sn_c_id, type);

    if (FALSE == wss_send_message(ctx->websockets_info, (u8 *)send_data, strlen(send_data))) {
        log_e("[wss_app_utils]Error %s %d. type=%d", __FILE__, __LINE__, type);

        free(send_data);
        
        if (ctx->websockets_info != NULL) {
            wss_server_disconnect(ctx->websockets_info);
            ctx->websockets_info = NULL;
        }
        wss_mark_exception("send_audio_format send failed");
        os_mutex_post(&g_wss_mutex);
        return -1;
    }
    log_i("[wss_app_util]wbs send msg : %s", send_data);
    free(send_data);
    os_mutex_post(&g_wss_mutex);
    return 0;
}

int send_req_message(const char *send_id, bool is_play, int event_type) 
{
    os_mutex_pend(&g_wss_mutex, 0);
    lwl_app_context_t *ctx = lwl_app_core_context_get_instance();
    if (ctx->websockets_info == NULL) {
        wss_mark_exception("send_req_message ctx->websockets_info NULL");
        os_mutex_post(&g_wss_mutex);
        return -1;
    }
    if (send_id != NULL) {
        add_target_sn(send_id); // 至少包含自己
    }

    char *send_data = odmp_req_message(target_sn_set, target_sn_count, sn_c_id, send_id, is_play, event_type);
    log_i("[wss_app_util]wbs send msg : %s", send_data);

    if (FALSE == wss_send_message(ctx->websockets_info, (u8 *)send_data, strlen(send_data))) {
        log_e("[wss_app_utils]Error %s %d. event_type=%d", __FILE__, __LINE__, event_type);

        free(send_data);

        if (ctx->websockets_info != NULL) {
            wss_server_disconnect(ctx->websockets_info);
            ctx->websockets_info = NULL;
        }
        wss_mark_exception("send_req_message send failed");
        os_mutex_post(&g_wss_mutex);
        return -1;
    }
    free(send_data);
    os_mutex_post(&g_wss_mutex);
    return 0;
}

int send_bin_data(uint8_t *data, uint32_t len)
{
    os_mutex_pend(&g_wss_mutex, 0);
    lwl_app_context_t *ctx = lwl_app_core_context_get_instance();
    if (ctx->websockets_info == NULL) {
        wss_mark_exception("send_bin_data ctx->websockets_info NULL");
        os_mutex_post(&g_wss_mutex);
        return -1;
    }

    // 检查队列深度，如果超过阈值则主动丢包，保护连接
    if (ctx->websockets_info->sk_fd) {
        // 这里假设 sk_fd 是 mbedtls context 或 socket fd，我们需要通过某种方式获取 qlen
        // 由于 sk_fd 是 void* 类型，且 websocket_struct 定义中没有直接暴露 pcb
        // 我们通过遍历 tcp_active_pcbs 来找到当前连接的 QLen
        extern struct tcp_pcb *tcp_active_pcbs;
        struct tcp_pcb *pcb;
        int found = 0;
        for (pcb = tcp_active_pcbs; pcb != NULL; pcb = pcb->next) {
            // 简单通过端口匹配，可能不够严谨但有效
            if (pcb->local_port == ctx->websockets_info->port || 
                pcb->remote_port == 8765) { // 假设服务器端口是 8765，或者根据实际 url 解析
                
                if (pcb->snd_queuelen > 20) { // 阈值设为 20
                        log_i("⚠️ Dropping audio frame to prevent blockage QLen=%d", pcb->snd_queuelen);
                        os_mutex_post(&g_wss_mutex);
                        return 0; // 伪装发送成功，实际上丢弃
                }
                found = 1;
                break;
            }
        }
    }

    if (FALSE == wss_send_bin(ctx->websockets_info, data, len)) {
        log_e("[wss_app_util]Error %s %d. len=%u", __FILE__, __LINE__, len);
        
        if (ctx->websockets_info != NULL) {
            wss_server_disconnect(ctx->websockets_info);
            ctx->websockets_info = NULL;
        }
        // wss_mark_exception("send_bin_data send failed");
        // os_mutex_post(&g_wss_mutex);
        // return -1;
    }
    // log_i("[wss_app_util]wbs send msg : %d", len);
    os_mutex_post(&g_wss_mutex);
    return 0;
}


int send_ping()
{
    os_mutex_pend(&g_wss_mutex, 0);
    lwl_app_context_t *ctx = lwl_app_core_context_get_instance();
    if (ctx->websockets_info == NULL) {
        wss_mark_exception("send_ping ctx->websockets_info NULL");
        os_mutex_post(&g_wss_mutex);
        return -1;
    }

    char *send_data = odmp_send_ping();
    log_i("[wss_app_util]wbs send msg : %s", send_data);

    if (FALSE == wss_send_message(ctx->websockets_info, (u8 *)send_data, strlen(send_data))) {
        log_e("[wss_app_utils]Error %s %d. event_type=ping", __FILE__, __LINE__);

        free(send_data);

        if (ctx->websockets_info != NULL) {
            wss_server_disconnect(ctx->websockets_info);
            ctx->websockets_info = NULL;
        }
        wss_mark_exception("send_ping send failed");
        os_mutex_post(&g_wss_mutex);
        return -1;
    }
    free(send_data);
    os_mutex_post(&g_wss_mutex);
    return 0;
}
