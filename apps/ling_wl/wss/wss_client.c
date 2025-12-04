#include "wss_client.h"
#include "web_socket/websocket_api.h"
#include "os/os_api.h"
#ifdef WEBSOCKET_MEMHEAP_DEBUG
#include "mem_leak_test.h"
#endif

#include "log.h"
#include <string.h>
#include <stdlib.h>
#include <assert.h>

static OS_MUTEX wss_mutex_bin;             //

#define WS_TXT_FRAME 0x81
#define WS_BIN_FRAME 0x82

void wss_recv_callback(u8 *buf, u32 len, u8 type)
{
    switch (type) {
    case WS_TXT_FRAME: // text frame
        // log_i("[wss_client] 接收到服务器 text frame len=%u payload=%s", len, buf);
        odmp_rsp_handler(buf, len);
        break;
    case WS_BIN_FRAME: // binary frame
        // log_i("[wss_client]bin frame len=%u", len);
        odmp_audio_data_handler(buf, len);
        break;
    default:
        // 打印接收到的数据
        log_i("[wss_client]unknown frame type=0x%x len=%u data=%s", type, len, buf);
        break;
    }
}

void wss_exit_callback(struct websocket_struct *websocket_info)
{
    log_i("[wss_client]websocket exit notify (valid=%d)", websocket_info ? websocket_info->websocket_valid : -1);
    // 打印连接上下文信息，辅助定位
    if (websocket_info) {
        log_i("[wss_client]ws info - port: %d", websocket_info->port);
        if (websocket_info->ip_or_url) {
            log_i("[wss_client]ws info - url: %s", websocket_info->ip_or_url);
        }
    }
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
    int ret = websockets_info->_send(websockets_info, buf, len, type);
    if (ret < 0) {
        log_i("[wss_client]send failed type=0x%x len=%d ret=%d", type, len, ret);
    } else if (ret != len) {
        log_i("[wss_client]send partial type=0x%x len=%d ret=%d", type, len, ret);
    }
    return ret;
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
    return (ws_ret == size); // 修改: 返回实际发送结果
}

int wss_send_bin(struct websocket_struct *websockets_info, u8 *buf, int size)
{
    //确认存在大于16K的数据包时，请修改发送逻辑
    os_mutex_pend(&wss_mutex_bin, 0);
    int ws_ret =  websockets_client_send(websockets_info, (u8 *)buf, size, WCT_BINDATA);
    os_mutex_post(&wss_mutex_bin);
    return (ws_ret == size); // 修改: 返回实际发送结果
}

struct websocket_struct *wss_server_connect(char *url)
{
    int err;
    const char *origin_str = "http://localhost";
    
    if (os_mutex_valid(&wss_mutex_bin)) return NULL;
    os_mutex_create(&wss_mutex_bin);
    os_mutex_pend(&wss_mutex_bin, 0);
    
    log_i("[wss_client]connecting to %s", url);
    
    /* 0 . malloc buffer */
    struct websocket_struct *websockets_info = malloc(sizeof(struct websocket_struct));
    assert(websockets_info != NULL);

    /* 1 . register */
    if (url[2] == 's') {
        log_i("[wss_client]register WEBSOCKETS_MODE");
        websockets_client_reg(websockets_info, WEBSOCKETS_MODE);
    } else {
        log_i("[wss_client]register WEBSOCKET_MODE");
        websockets_client_reg(websockets_info, WEBSOCKET_MODE);
    }

    /* 2 . init */
    err = websockets_client_init(websockets_info, url, origin_str);
    if (FALSE == err) {
        log_i("[wss_client]client websocket init error");
        goto exit_ws;
    }

    /* 3 . hanshack */
    err = websockets_client_handshack(websockets_info);
    if (FALSE == err) {
        log_i("[wss_client]handshake error ret=%d", err);
        goto exit_ws;
    }

    /* 4 . CreateThread */
    thread_fork("websocket_client_recv", 10, 512, 0,
                &websockets_info->recv_thread_id,
                websockets_info->_recv_thread,
                websockets_info);
    os_mutex_post(&wss_mutex_bin);

    return websockets_info;

exit_ws:
    /* 6 . exit */
    // thread_kill(&websockets_info->ping_thread_id, KILL_REQ); // 也要注释掉对应的 kill
    thread_kill(&websockets_info->recv_thread_id, KILL_REQ);
    websockets_client_exit(websockets_info);
    free(websockets_info);
    os_mutex_post(&wss_mutex_bin); 
    os_mutex_del(&wss_mutex_bin, 0);

    return NULL;
}

void wss_server_disconnect(struct websocket_struct *websockets_info)
{
    os_mutex_pend(&wss_mutex_bin, 0);
    log_i("[wss_client]server_disconnect threads(ping=%d recv=%d)",
          websockets_info ? websockets_info->ping_thread_id : -1,
          websockets_info ? websockets_info->recv_thread_id : -1);
    // thread_kill(&websockets_info->ping_thread_id, KILL_REQ); // 注释掉
    thread_kill(&websockets_info->recv_thread_id, KILL_REQ);
    websockets_client_exit(websockets_info);
    free(websockets_info);
    os_mutex_post(&wss_mutex_bin);
    os_mutex_del(&wss_mutex_bin, 0);
}
