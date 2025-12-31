// Prevent ESTABLISHED collision
#define ESTABLISHED WS_ESTABLISHED_HIDDEN
#include "wss_app_util.h"
#include "os/os_api.h"

#include "protocol/odm/odmp.h"
#include "app_mode/lwl_app_core.h"
#include "wss/wss_client.h"
#include "wss/wss_client_custom.h"

#include "system/includes.h"
#include "app_cookie.h"

#include <string.h>
#include <stdlib.h>

#define MAX_TARGET_SN_SET 16
// 自定义 WebSocket 实现的临时帧构建缓冲区是 16KB（见 ws_client_custom.c 的 temp_send_buf）
// 为避免超大 JSON 帧导致构建失败或潜在内存问题，这里对文本 payload 做一个保守上限。
#define WSS_CUSTOM_WS_MAX_TEXT_PAYLOAD (32 * 1024)
// 使用自定义WebSocket实现
static ws_client_t g_ws_client_custom = NULL;

char sn_c_id[25];
static int g_exception_exit = true;
static char g_last_disconnect_reason[64] = "boot";
static OS_MUTEX g_wss_mutex;
static char* target_sn_set[MAX_TARGET_SN_SET] = {0};
static int target_sn_count = 0;

// 添加SN到集合
void add_target_sn(const char* sn) {
    if (!sn) return;
    if (strlen(sn)!= PI_SN_SIZE) return;
    os_mutex_pend(&g_wss_mutex, 0);
    // 检查是否已存在
    for (int i = 0; i < target_sn_count; ++i) {
        if (strncmp(target_sn_set[i], sn, PI_SN_SIZE) == 0) {
            os_mutex_post(&g_wss_mutex);
            return; // 已存在
        }
    }
    if (target_sn_count >= MAX_TARGET_SN_SET) {
        os_mutex_post(&g_wss_mutex);
        return;
    }
    target_sn_set[target_sn_count] = (char*)malloc(PI_SN_SIZE +1);
    strncpy(target_sn_set[target_sn_count], sn, PI_SN_SIZE + 1);
    target_sn_count++;
    os_mutex_post(&g_wss_mutex);
}

// 从集合中删除SN
void remove_target_sn(const char* sn) {
    if (!sn) return;
    os_mutex_pend(&g_wss_mutex, 0);
    for (int i = 0; i < target_sn_count; ++i) {
        if (strncmp(target_sn_set[i], sn, PI_SN_SIZE) == 0) {
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
        if (strncmp(target_sn_set[i], sn, PI_SN_SIZE) == 0) {
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

bool wss_check_exit(void)
{
    if (g_exception_exit) {
        return true;
    }
    // 使用自定义WebSocket实现时，检查连接状态
    if (g_ws_client_custom == NULL) {
        wss_mark_exception("websocket client NULL");
        log_e("[wss_app_util]websocket client is NULL");
        return true;
    }
    ws_state_t state = ws_client_get_state(g_ws_client_custom);
    if (state != WS_STATE_CONNECTED) {
        wss_mark_exception("websocket not connected");
        log_e("[wss_app_util]websocket state=%d", state);
        return true;
    }
    // os_mutex_pend(&g_wss_mutex, 0);
    // lwl_app_context_t *context = lwl_app_core_context_get_instance();
    // if (context->websockets_info) {
    //     wss_send_pong(context->websockets_info); // 如果收到Ping 回复Pong
    //     // TODO 检查收到的wss消息的时效性，超过10s发送ping
    //     // if (g_curr_time - g_wss_last_time > WSS_TIMEOUT) {
    //     //     send_ping();
    //     //     g_wss_last_time = g_curr_time;
    //     // }
    //     if (context->websockets_info->recv_thread_id == 0) {
    //         log_i("websocket_client_heart have exited!!!");
    //     }
    // } else {
    //     log_i("websockets_info is NULL");
    // }
    // os_mutex_post(&g_wss_mutex);
    return g_exception_exit;
}

char* wss_url = NULL;

// WebSocket接收数据回调（适配自定义实现）
static void wss_custom_recv_callback(u8 *buf, u32 len, u8 is_text)
{
    if (is_text != 0) {
        log_i("wbs recv msg : %.*s\n", len, buf);
        odmp_rsp_handler(buf, len);
    } else {
        odmp_audio_data_handler(buf, len);
    }
}

// WebSocket断开连接回调（适配自定义实现）
static void wss_custom_disconnect_callback(const char *reason)
{
    log_i("[wss_client]websocket exit notify: %s", reason ? reason : "unknown");
    odmp_disconnect_handler();
}

bool wss_connect(void) {
    if (!os_mutex_valid(&g_wss_mutex)) {
        os_mutex_create(&g_wss_mutex);
    }
    os_mutex_pend(&g_wss_mutex, 0);

    log_i("[wss_app_util]wss_connect start (using custom WebSocket implementation)");
    
    // 如果已有连接，先断开
    if (g_ws_client_custom != NULL) {
        ws_client_disconnect(g_ws_client_custom);
        ws_client_destroy(g_ws_client_custom);
        g_ws_client_custom = NULL;
    }
    
    wss_url = get_wss_connection_url();
    if (wss_url == NULL) {
        log_e("[wss_app_util]❌❌❌ get_wss_connection_url failed wss_url=NULL");
        os_mutex_post(&g_wss_mutex);
        return false;
    }
    
    // 创建自定义WebSocket客户端
    g_ws_client_custom = ws_client_create();
    if (g_ws_client_custom == NULL) {
        log_e("[wss_app_util]ws_client_create failed");
        os_mutex_post(&g_wss_mutex);
        return false;
    }
    
    // 连接到服务器
    int ret = ws_client_connect(g_ws_client_custom, wss_url, 
                                wss_custom_recv_callback, 
                                wss_custom_disconnect_callback);
    if (ret < 0) {
        log_e("[wss_app_util]ws_client_connect failed ret=%d", ret);
        ws_client_destroy(g_ws_client_custom);
        g_ws_client_custom = NULL;
        os_mutex_post(&g_wss_mutex);
        return false;
    }
    
    // 为了兼容性，设置websockets_info为非NULL（但实际不使用）
    // 或者可以设置为一个标记值
    lwl_app_context_t *context = lwl_app_core_context_get_instance();
    context->websockets_info = (struct websocket_struct *)g_ws_client_custom; // 标记已连接
    
    g_exception_exit = false;
    strncpy(g_last_disconnect_reason, "connected", sizeof(g_last_disconnect_reason) - 1);
    g_last_disconnect_reason[sizeof(g_last_disconnect_reason) - 1] = '\0';
    log_i("[wss_app_util]✅✅✅ websockets init succ url=%s\n", wss_url);
    get_SN(sn_c_id);
    os_mutex_post(&g_wss_mutex);
    return true;
}

void wss_disconnect() {
    os_mutex_pend(&g_wss_mutex, 0);
    lwl_app_context_t *context = lwl_app_core_context_get_instance();
    if (g_ws_client_custom != NULL) {
        log_e("[wss_app_util]wss_disconnect closing active ws (last_reason=%s)", g_last_disconnect_reason);
        ws_client_disconnect(g_ws_client_custom);
        ws_client_destroy(g_ws_client_custom);
        g_ws_client_custom = NULL;
        context->websockets_info = NULL;
        free(wss_url);
        wss_url = NULL;
    }
    wss_mark_exception("wss_disconnect manual");
    // log_mqtt_stop();//xTaskSwitchContext = 0
    os_mutex_post(&g_wss_mutex);
}

bool wss_reconnect(void) {
    if (!os_mutex_valid(&g_wss_mutex)) {
        os_mutex_create(&g_wss_mutex);
    }
    os_mutex_pend(&g_wss_mutex, 0);
    lwl_app_context_t *context = lwl_app_core_context_get_instance();
    
    // 断开现有连接
    if (g_ws_client_custom != NULL) {
        log_e("[wss_app_util]websockets disconnect by wss_reconnect");
        ws_client_disconnect(g_ws_client_custom);
        ws_client_destroy(g_ws_client_custom);
        g_ws_client_custom = NULL;
        context->websockets_info = NULL;
    }
    
    if (wss_url == NULL) {
        wss_url = get_wss_connection_url();
    } else {
        msleep(100); // 断线后等待100ms再重连
    }
    log_i("[wss_app_util]websockets reconnecting %s", wss_url);

    if (wss_url == NULL) {
        os_mutex_post(&g_wss_mutex);
        return false;
    }
    
    // 创建新的连接
    g_ws_client_custom = ws_client_create();
    if (g_ws_client_custom == NULL) {
        log_e("[wss_app_util]ws_client_create failed in reconnect");
        os_mutex_post(&g_wss_mutex);
        return false;
    }
    
    log_i("[wss_app_util]attempting reconnect %s", wss_url);
    int ret = ws_client_connect(g_ws_client_custom, wss_url,
                                wss_custom_recv_callback,
                                wss_custom_disconnect_callback);
    if (ret < 0) {
        log_e("[wss_app_util]ws_client_connect failed in reconnect ret=%d", ret);
        ws_client_destroy(g_ws_client_custom);
        g_ws_client_custom = NULL;
        os_mutex_post(&g_wss_mutex);
        return false;
    }
    
    context->websockets_info = (struct websocket_struct *)g_ws_client_custom; // 标记已连接
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
    if (g_ws_client_custom == NULL) {
        wss_mark_exception("send_audio_status ws_client NULL");
        os_mutex_post(&g_wss_mutex);
        return -1;
    }

    char *send_data = odmp_audio_status(sn_c_id, type, event);
    if (send_data == NULL) {
        log_e("[wss_app_util]send_audio_status: odmp_audio_status returned NULL");
        os_mutex_post(&g_wss_mutex);
        return -1;
    }
    size_t send_len = strlen(send_data);
    if (send_len > WSS_CUSTOM_WS_MAX_TEXT_PAYLOAD) {
        log_e("[wss_app_util] send_audio_status: payload too large (%u), drop", (u32)send_len);
        free(send_data);
        os_mutex_post(&g_wss_mutex);
        return -1;
    }
    int ret = ws_client_send_text(g_ws_client_custom, (u8 *)send_data, send_len);
    
    if (ret < 0) {
        log_e("⚠️ [wss_app_utils]Error %s %d. type=%d event=%d ret=%d", __FILE__, __LINE__, type, event, ret);
        free(send_data);
        os_mutex_post(&g_wss_mutex);
        return -1;
    }
    log_i("[wss_app_util]wbs send msg : %s", send_data);
    free(send_data);
    os_mutex_post(&g_wss_mutex);
    return 0;
}

// 只在连接成功后发送一次
int send_audio_format(int type) 
{
    os_mutex_pend(&g_wss_mutex, 0);
    if (g_ws_client_custom == NULL) {
        wss_mark_exception("send_audio_format ws_client NULL");
        os_mutex_post(&g_wss_mutex);
        return -1;
    }

    char *send_data = odmp_set_audio_format(sn_c_id, type);
    if (send_data == NULL) {
        log_e("[wss_app_util]send_audio_format: odmp_set_audio_format returned NULL");
        os_mutex_post(&g_wss_mutex);
        return -1;
    }
    size_t send_len = strlen(send_data);
    if (send_len > WSS_CUSTOM_WS_MAX_TEXT_PAYLOAD) {
        log_e("[wss_app_util]send_audio_format: payload too large (%u), drop", (u32)send_len);
        free(send_data);
        os_mutex_post(&g_wss_mutex);
        return -1;
    }
    int ret = ws_client_send_text(g_ws_client_custom, (u8 *)send_data, send_len);

    if (ret < 0) {
        log_e("[wss_app_utils]Error %s %d. type=%d ret=%d", __FILE__, __LINE__, type, ret);
        free(send_data);
        ws_client_disconnect(g_ws_client_custom);
        ws_client_destroy(g_ws_client_custom);
        g_ws_client_custom = NULL;
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
    if (g_ws_client_custom == NULL) {
        wss_mark_exception("send_req_message ws_client NULL");
        os_mutex_post(&g_wss_mutex);
        return -1;
    }
    if (send_id != NULL) {
        add_target_sn(send_id); // 至少包含自己
    }

    char *send_data = odmp_req_message(target_sn_set, target_sn_count, sn_c_id, send_id, is_play, event_type);
    if (send_data == NULL) {
        log_e("[wss_app_util]send_req_message: odmp_req_message returned NULL");
        os_mutex_post(&g_wss_mutex);
        return -1;
    }
    size_t send_len = strlen(send_data);
    if (send_len > WSS_CUSTOM_WS_MAX_TEXT_PAYLOAD) {
        log_e("[wss_app_util]send_req_message: payload too large (%u), drop", (u32)send_len);
        free(send_data);
        os_mutex_post(&g_wss_mutex);
        return -1;
    }
    log_i("[wss_app_util]wbs send msg : %s", send_data);

    int ret = ws_client_send_text(g_ws_client_custom, (u8 *)send_data, send_len);
    if (ret < 0) {
        log_e("[wss_app_utils]Error send_req_message %s %d. event_type=%d ret=%d", __FILE__, __LINE__, event_type, ret);
        free(send_data);
        os_mutex_post(&g_wss_mutex);
        return -1;
    }
    free(send_data);
    os_mutex_post(&g_wss_mutex);
    return 0;
}

int send_device_status(int status)
{
    os_mutex_pend(&g_wss_mutex, 0);
    if (g_ws_client_custom == NULL) {
        log_e("[wss_app_util]send_device_status: ws_client is NULL");
        os_mutex_post(&g_wss_mutex);
        return -1;
    }

    char *send_data = odmp_device_status(status);
    if (send_data == NULL) {
        log_e("[wss_app_util]send_device_status: odmp_device_status returned NULL");
        os_mutex_post(&g_wss_mutex);
        return -1;
    }
    size_t send_len = strlen(send_data);
    if (send_len > WSS_CUSTOM_WS_MAX_TEXT_PAYLOAD) {
        log_e("[wss_app_util]send_device_status: payload too large (%u), drop", (u32)send_len);
        free(send_data);
        os_mutex_post(&g_wss_mutex);
        return -1;
    }
    int ret = ws_client_send_text(g_ws_client_custom, (u8 *)send_data, send_len);
    if (ret < 0) {
        log_e("[wss_app_utils]Error send_device_status %s %d. status=%d ret=%d", __FILE__, __LINE__, status, ret);
        free(send_data);
        os_mutex_post(&g_wss_mutex);
        return -1;
    }
    //根据status打印不同的日志
    switch (status) {
        case 0:
            log_i("🚺️➡️➡️➡️【DEVICE_STATUS】cid=%s status=在线", sn_c_id);
            break;
        case 1:
            log_i("🚺️➡️➡️➡️【DEVICE_STATUS】cid=%s status=正在播放", sn_c_id);
            break;
        case 2:
            log_i("🚺️➡️➡️➡️【DEVICE_STATUS】cid=%s status=正在录音", sn_c_id);
            break;
        case 3:
            log_i("🚺️➡️➡️➡️【DEVICE_STATUS】cid=%s status=播放结束", sn_c_id);
            break;
    }
    free(send_data);
    os_mutex_post(&g_wss_mutex);
    return 0;
}

int send_bin_data(uint8_t *data, uint32_t len)
{
    os_mutex_pend(&g_wss_mutex, 0);
    if (g_ws_client_custom == NULL) {
        wss_mark_exception("send_bin_data ws_client NULL");
        os_mutex_post(&g_wss_mutex);
        return -1;
    }

    // 检查连接状态
    ws_state_t state = ws_client_get_state(g_ws_client_custom);
    if (state != WS_STATE_CONNECTED) {
        log_e("[wss_app_util]WebSocket not connected, state=%d", state);
        os_mutex_post(&g_wss_mutex);
        return -1;
    }

    // 发送二进制数据
    int ret = ws_client_send_binary(g_ws_client_custom, data, len);
    if (ret < 0) {
        // 当队列满时，丢弃也会返回-1，所以不处理
        log_e("[wss_app_util]Error %s %d. len=%u ret=%d", __FILE__, __LINE__, len, ret);
        // ws_client_disconnect(g_ws_client_custom);
        // ws_client_destroy(g_ws_client_custom);
        // g_ws_client_custom = NULL;
        // 不标记异常，允许重连
    }
    // log_i("[wss_app_util]wbs send bin : %d", len);
    os_mutex_post(&g_wss_mutex);
    return 0;
}


int send_ping()
{
    os_mutex_pend(&g_wss_mutex, 0);
    if (g_ws_client_custom == NULL) {
        wss_mark_exception("send_ping ws_client NULL");
        os_mutex_post(&g_wss_mutex);
        return -1;
    }

    // 发送应用层ping消息（通过文本消息）
    char *send_data = odmp_send_ping();
    if (send_data == NULL) {
        log_e("[wss_app_util]send_ping: odmp_send_ping returned NULL");
        os_mutex_post(&g_wss_mutex);
        return -1;
    }
    size_t send_len = strlen(send_data);
    if (send_len > WSS_CUSTOM_WS_MAX_TEXT_PAYLOAD) {
        log_e("[wss_app_util]send_ping: payload too large (%u), drop", (u32)send_len);
        free(send_data);
        os_mutex_post(&g_wss_mutex);
        return -1;
    }
    log_i("[wss_app_util]wbs send msg : %s", send_data);

    int ret = ws_client_send_text(g_ws_client_custom, (u8 *)send_data, send_len);
    if (ret < 0) {
        log_e("[wss_app_utils]Error %s %d. event_type=ping ret=%d", __FILE__, __LINE__, ret);
        free(send_data);
        // ws_client_disconnect(g_ws_client_custom);
        // ws_client_destroy(g_ws_client_custom);
        // g_ws_client_custom = NULL;
        // wss_mark_exception("send_ping send failed");
        os_mutex_post(&g_wss_mutex);
        return -1;
    }
    free(send_data);
    
    // 可选：同时发送WebSocket协议层的Ping帧
    // ws_client_send_ping(g_ws_client_custom);
    
    os_mutex_post(&g_wss_mutex);
    return 0;
}
