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
static struct websocket_struct *g_wss_info = NULL;
static u8 ping_payload[2];
static u32 ping_payload_len = 2;

#define WS_TXT_FRAME 0x81
#define WS_BIN_FRAME 0x82
#define WS_CLOSE_FRAME 0x88
#define WS_PING_FRAME  0x89
#define WS_PONG_FRAME  0x8A

static bool recved_ping = 0;

static int websockets_client_send(struct websocket_struct *websockets_info, u8 *buf, int len, char type);

void wss_recv_callback(u8 *buf, u32 len, u8 type)
{
    switch (type) {
    case WS_TXT_FRAME: // text frame
        odmp_rsp_handler(buf, len);
        break;
    case WS_BIN_FRAME: // binary frame
        // log_i("[wss_client]bin frame len=%u", len);
        odmp_audio_data_handler(buf, len);
        break;
    case WS_PING_FRAME: // ping frame
        log_i("[wss_client]recv PING frame len=%u", len);
        // Store ping payload to echo back in pong response
        if (len == ping_payload_len) {
            memcpy(ping_payload, buf, len);
        }
        recved_ping = 1;
        break;
    case WS_PONG_FRAME: // pong frame
        log_i("[wss_client]recv PONG frame len=%u", len);
        break;
    case WS_CLOSE_FRAME: // close frame
        log_i("[wss_client]recv CLOSE frame len=%u", len);
        // Notify higher layer that the peer requested close
        odmp_disconnect_handler();
        break;
    default:
        // Ignore/control frames we don't explicitly handle, avoid noisy logs
        log_i("[wss_client]unknown frame type=0x%x len=%u", type, len);
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
    g_wss_info = NULL;
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


int wss_send_pong(struct websocket_struct *websockets_info)
{
    if (recved_ping) {
        os_mutex_pend(&wss_mutex_bin, 0);
        // Send pong with stored ping payload (echo back the same payload)
        int ret = websockets_client_send(websockets_info, ping_payload, ping_payload_len, WCT_PONG);
        recved_ping = 0;
        os_mutex_post(&wss_mutex_bin);
        log_i("[wss_client]send PONG ret=%d", ret);
    }
    return 0;
}

struct websocket_struct *wss_server_connect(char *url)
{
    int err;
    const char *origin_str = "http://localhost";
    if (!os_mutex_valid(&wss_mutex_bin)) {
        os_mutex_create(&wss_mutex_bin);
    }
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

    /* store global context for callbacks (used to reply to ping) */
    g_wss_info = websockets_info;

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
    g_wss_info = NULL;
    os_mutex_post(&wss_mutex_bin); 

    return NULL;
}

void wss_server_disconnect(struct websocket_struct *websockets_info)
{
    if (!websockets_info) return;
    os_mutex_pend(&wss_mutex_bin, 0);
    log_i("[wss_client]server_disconnect threads(ping=%d recv=%d)",
          websockets_info ? websockets_info->ping_thread_id : -1,
          websockets_info ? websockets_info->recv_thread_id : -1);
    // thread_kill(&websockets_info->ping_thread_id, KILL_REQ); // 注释掉

    if (websockets_info->recv_thread_id) {
        thread_kill(&websockets_info->recv_thread_id, KILL_REQ);
    }
    websockets_client_exit(websockets_info);
    free(websockets_info);
    g_wss_info = NULL;
    os_mutex_post(&wss_mutex_bin);
}
