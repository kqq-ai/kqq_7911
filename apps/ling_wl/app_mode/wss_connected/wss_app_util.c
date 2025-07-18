#include "wss_app_util.h"

#include "wss/wss_client.h"
#include "protocol/odm/odmp.h"
#include "protocol/lingos/loswssp.h"
#include "app_mode/lwl_app_core.h"

#define LING_UUID_BUFSIZE 37

#define UUID_FIXED "abcdef-123456"


char sn_c_id[25];

int send_audio_status(int type, int event) 
{
    lwl_app_context_t *ctx = lwl_app_core_context_get_instance();
    if (ctx->websockets_info == NULL)
        return -1;

    get_SN(&sn_c_id);
    char *send_data = odmp_audio_status(sn_c_id, type, event);

    if (FALSE == wss_server_send(ctx->websockets_info, send_data, strlen(send_data))) {
        log_e("Error %s %d.", __FILE__, __LINE__);

        free(send_data);
        
        if (ctx->websockets_info != NULL) {
            wss_server_disconnect(ctx->websockets_info);
            ctx->websockets_info = NULL;
        }
        return -1;
    }
    log_i("wbs send msg : %s", send_data);
    free(send_data);
    return 0;
}

int send_req_message(const char *send_id, bool is_play, int event_type) 
{
    lwl_app_context_t *ctx = lwl_app_core_context_get_instance();
    if (ctx->websockets_info == NULL)
        return -1;

    char **targets = NULL;
    int targets_num = 0;
    make_5m_edev_targets(&targets, &targets_num);

    get_SN(&sn_c_id);
    char *send_data = odmp_req_message(targets, targets_num, sn_c_id, send_id, is_play, event_type);

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
    log_i("wbs send msg : %s", send_data);
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
