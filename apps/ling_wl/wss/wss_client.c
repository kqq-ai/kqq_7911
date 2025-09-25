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
        odmp_rsp_handler(buf, len);
        break;
    case WS_BIN_FRAME: // binary frame
        log_i("wbs recv data : %d\n", len);
        odmp_audio_data_handler(buf, len);
        break;
    }
}

void wss_exit_callback(struct websocket_struct *websocket_info)
{
    log_i("websocket exit!!!!!");
    odmp_disconnect_handler();
}

/*******************************************************************************
*   Websocket Client api
*******************************************************************************/
static void websockets_client_reg(struct websocket_struct *websockets_info, char mode)
{
    memset(websockets_info, 0, sizeof(struct websocket_struct));
    websockets_info->_init           = websockets_client_socket_init;
    websockets_info->_exit           = websockets_client_socket_exit;
    websockets_info->_handshack      = webcockets_client_socket_handshack;
    websockets_info->_send           = websockets_client_socket_send;
    websockets_info->_recv_thread    = websockets_client_socket_recv_thread;
    websockets_info->_heart_thread   = websockets_client_socket_heart_thread;
    websockets_info->_recv_cb        = wss_recv_callback;
    websockets_info->_exit_notify    = wss_exit_callback;
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
    return websockets_info->_init(websockets_info);
}

static int websockets_client_handshack(struct websocket_struct *websockets_info)
{
    return websockets_info->_handshack(websockets_info);
}

static int websockets_client_send(struct websocket_struct *websockets_info, u8 *buf, int len, char type)
{
    //SSL加密时一次发送数据不能超过16K，用户需要自己分包
    assert(len < 160000);
    return websockets_info->_send(websockets_info, buf, len, type);
}

static void websockets_client_exit(struct websocket_struct *websockets_info)
{
    websockets_info->_exit(websockets_info);
}

int wss_send_message(struct websocket_struct *websockets_info, u8 *buf, int size)
{
    //确认存在大于16K的数据包时，请修改发送逻辑
    os_mutex_pend(&wss_mutex_bin, 0);
    int ws_ret = websockets_client_send(websockets_info, (u8 *)buf, size, WCT_TXTDATA);
    os_mutex_post(&wss_mutex_bin);
    return ws_ret;
}

int wss_send_bin(struct websocket_struct *websockets_info, u8 *buf, int size)
{
    //确认存在大于16K的数据包时，请修改发送逻辑
    os_mutex_pend(&wss_mutex_bin, 0);
    int ws_ret =  websockets_client_send(websockets_info, (u8 *)buf, size, WCT_BINDATA);
    os_mutex_post(&wss_mutex_bin);
    return ws_ret;
}

struct websocket_struct *wss_server_connect(char *url)
{
    int err;
    const char *origin_str = "jl791n://localhost";
    
    if (os_mutex_valid(&wss_mutex_bin)) return NULL;
    os_mutex_create(&wss_mutex_bin);
    os_mutex_pend(&wss_mutex_bin, 0);
    
    log_i(" . ----------------- Client Websocket ------------------\r\n");
    
    /* 0 . malloc buffer */
    struct websocket_struct *websockets_info = malloc(sizeof(struct websocket_struct));
    assert(websockets_info != NULL);

    /* 1 . register */
    if (url[2] == 's') {
        websockets_client_reg(websockets_info, WEBSOCKETS_MODE);
    } else {
        websockets_client_reg(websockets_info, WEBSOCKET_MODE);
    }

    /* 2 . init */
    err = websockets_client_init(websockets_info, url, origin_str);
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
    os_mutex_post(&wss_mutex_bin);

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