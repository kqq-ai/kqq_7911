#include "wss_client.h"
#include "web_socket/websocket_api.h"
#include "wifi/wifi_connect.h"
#include "system/includes.h"
#ifdef WEBSOCKET_MEMHEAP_DEBUG
#include "mem_leak_test.h"
#endif

#include "app_config.h"

#include "cJSON_common/cJSON.h"
#include "net/mbedtls_2_2_1/mbedtls/base64.h"
#include "util/cqueue.h"
#include "async_msg_loop.h"

#include "bt_ble/client/ble_distance.h"

#include "bt_ble/client/ble_simple_client.h"
#include "log.h"

static OS_MUTEX wss_mutex_bin;             //

#define WS_TXT_FRAME 0x81
#define WS_BIN_FRAME 0x82

void wss_recv_callback(u8 *buf, u32 len, u8 type)
{
    switch (type) {
    case WS_TXT_FRAME: // text frame
    
        log_i("wbs recv msg : %s\n", buf);
        if (strcmp(buf, "client ping") == 0) {
    
        } else {
            // odmp process
            odmp_rsp_handler(buf, len);
        }
        break;
    case WS_BIN_FRAME: // binary frame
        log_i("wbs recv msg : %d\n", len);
        odmp_audio_data_handler(buf, len);
        break;
    }
}


/*******************************************************************************
*   Websocket Client api
*******************************************************************************/
static void websockets_client_reg(struct websocket_struct *websockets_info, char mode, user_wss_recv_cb_t websockets_callback)
{
    memset(websockets_info, 0, sizeof(struct websocket_struct));
    websockets_info->_init           = websockets_client_socket_init;
    websockets_info->_exit           = websockets_client_socket_exit;
    websockets_info->_handshack      = webcockets_client_socket_handshack;
    websockets_info->_send           = websockets_client_socket_send;
    websockets_info->_recv_thread    = websockets_client_socket_recv_thread;
    websockets_info->_heart_thread   = websockets_client_socket_heart_thread;
    websockets_info->_recv_cb        = websockets_callback;
    websockets_info->_recv           = NULL;

    websockets_info->websocket_mode  = mode;
#if 0
    //应用层指定申请接收内存大小（申请即可，不用释放，内部会释放），默认不使用
    websockets_info->recv_buf_size	 = 30 * 1024;
    websockets_info->recv_buf		 = malloc(websockets_info->recv_buf_size);
#endif
}

static int websockets_client_init(struct websocket_struct *websockets_info, u8 *url, const char *origin_str)
{
    websockets_info->ip_or_url = url;
    websockets_info->origin_str = origin_str;
    websockets_info->recv_time_out = 1000;
    //应用层和库的版本检测，结构体不一样则返回出错
    int err = websockets_struct_check(sizeof(struct websocket_struct));
    if (err == FALSE) {
        return err;
    }
    int ret = websockets_info->_init(websockets_info);
    return ret;
}

static int websockets_client_handshack(struct websocket_struct *websockets_info)
{
    os_mutex_pend(&wss_mutex_bin, 0);
    int ret = websockets_info->_handshack(websockets_info);
    os_mutex_post(&wss_mutex_bin);
    return ret;
}

static int websockets_client_send(struct websocket_struct *websockets_info, u8 *buf, int len, char type)
{
    //SSL加密时一次发送数据不能超过16K，用户需要自己分包
    os_mutex_pend(&wss_mutex_bin, 0);
    int ret = websockets_info->_send(websockets_info, buf, len, type);
    os_mutex_post(&wss_mutex_bin);
    return ret;
}

static void websockets_client_exit(struct websocket_struct *websockets_info)
{
    os_mutex_pend(&wss_mutex_bin, 0);
    websockets_info->_exit(websockets_info);
    os_mutex_post(&wss_mutex_bin);
}

int wss_server_send(struct websocket_struct *websockets_info, u8 *buf, int size)
{
    //SSL加密时一次发送数据不能超过16K，用户需要自己分包
    //确认存在大于16K的数据包时，请修改发送逻辑
    // return websockets_client_send(websockets_info, (u8 *)buf, size, WCT_TXTDATA);
    int ws_ret = websockets_client_send(websockets_info, (u8 *)buf, size, WCT_TXTDATA);
    return ws_ret;
}

int wss_server_send_bin(struct websocket_struct *websockets_info, u8 *buf, int size)
{
    // return websockets_client_send(websockets_info, (u8 *)buf, size, WCT_BINDATA);
    int ws_ret =  websockets_client_send(websockets_info, (u8 *)buf, size, WCT_BINDATA);
    return ws_ret;
}

struct websocket_struct *wss_server_connect(char *url)
{
    int err;
    const char *origin_str = "jl791n://localhost";
    
    if (!os_mutex_valid(&wss_mutex_bin)) {
        os_mutex_create(&wss_mutex_bin);
    }
    os_mutex_pend(&wss_mutex_bin, 0);
    
    log_i(" . ----------------- Client Websocket ------------------\r\n");
    
    /* 0 . malloc buffer */
    struct websocket_struct *websockets_info = malloc(sizeof(struct websocket_struct));
    memset(websockets_info, 0, sizeof(struct websocket_struct));
    if (!websockets_info) {
        os_mutex_post(&wss_mutex_bin);
        os_mutex_del(&wss_mutex_bin, 0);
        return NULL;
    }
    /* 1 . register */
    if (url[2] == 's') {
        websockets_client_reg(websockets_info, WEBSOCKETS_MODE, wss_recv_callback);
    } else {
        websockets_client_reg(websockets_info, WEBSOCKET_MODE, wss_recv_callback);
    }

    /* 2 . init */
    err = websockets_client_init(websockets_info, url, origin_str);
    os_mutex_post(&wss_mutex_bin);
    if (FALSE == err) {
        log_i("  . ! Cilent websocket init error !!!\r\n");
        goto exit_ws;
    }

    /* 3 . hanshack */
    err = websockets_client_handshack(websockets_info);
    if (FALSE == err) {
        log_i("  . ! Handshake error !!!\r\n");
        goto exit_ws;
    }

    /* 4 . CreateThread */
    thread_fork("websocket_client_heart", 19, 512, 0,
                &websockets_info->ping_thread_id,
                websockets_info->_heart_thread,
                websockets_info);
    thread_fork("websocket_client_recv", 18, 512, 0,
                &websockets_info->recv_thread_id,
                websockets_info->_recv_thread,
                websockets_info);

    return websockets_info;

exit_ws:
    /* 6 . exit */
    thread_kill(&websockets_info->ping_thread_id, KILL_REQ);
    thread_kill(&websockets_info->recv_thread_id, KILL_REQ);
    websockets_client_exit(websockets_info);
    free(websockets_info);

    return NULL;
}

void wss_server_disconnect(struct websocket_struct *websockets_info)
{
    os_mutex_pend(&wss_mutex_bin, 0);
    thread_kill(&websockets_info->ping_thread_id, KILL_REQ);
    thread_kill(&websockets_info->recv_thread_id, KILL_REQ);
    websockets_client_exit(websockets_info);
    free(websockets_info);
    os_mutex_post(&wss_mutex_bin);
    os_mutex_del(&wss_mutex_bin, 0);    
}


// void wss_server_reconnect()
// {
//     lwl_app_context_t *context = lwl_app_core_context_get_instance();
//     if (context->wss_url == NULL) {
//         log_e("websockets has no URL!!!!\n");
//         return;
//     }
//     if (context->websockets_info != NULL) {
//         wss_server_disconnect(context->websockets_info);
//         context->websockets_info = NULL;
//     }
//     while (1) {
//         log_i("websockets try to reconnect\n");
//         lwl_app_context_t *context = lwl_app_core_context_get_instance();
//         char *secure_url = context->wss_url;

//         struct websocket_info_t *websockets_info = wss_server_connect(secure_url);

//         if (websockets_info == NULL) {
//             log_i("websockets init failed\n");
//             free(websockets_info);
//             // TODO 尝试重连WSS
//             // lwl_app_mode_switch_to(APP_WSS_CONNECTED_MODE);
//         } else {
//             /* 将websocket_info保存到全局上下文 */
//         log_i("websockets reconnected\n");
//             context->websockets_info = websockets_info;
//             return;
//             // lwl_app_mode_switch_to(APP_WSS_CONNECTED_MODE);
//         }
//         msleep(10);
//     }
//     return;
// }
