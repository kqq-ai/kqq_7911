#include "wss_app_util.h"
#include "os/os_api.h"

#include "protocol/odm/odmp.h"
#include "app_mode/lwl_app_core.h"
#include "wss/wss_client.h"

char sn_c_id[25];
static int g_exception_exit = false;
static OS_MUTEX g_wss_mutex;

void wss_exception_exit()
{
    g_exception_exit = true;
}

bool wss_check_exit()
{
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
    get_SN(&sn_c_id);
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

    char **targets = NULL;
    int targets_num = 0;
    make_5m_edev_targets(&targets, &targets_num);

    char *send_data = odmp_req_message(targets, targets_num, sn_c_id, send_id, is_play, event_type);

    if (FALSE == wss_server_send(ctx->websockets_info, send_data, strlen(send_data))) {
        log_e("Error %s %d.", __FILE__, __LINE__);

        free(send_data);
        free_5m_edev_targets(targets, targets_num);

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
    free_5m_edev_targets(targets, targets_num);
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
    os_mutex_post(&g_wss_mutex);
    return 0;
}
