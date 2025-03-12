/**
 * @author chuxuanhuang
 * @brief 
 *   +----> IDLE
 *   |
 * POWER_ON -------> NET_CONNECTING <-------> NET_CONNECTED
 *   |                   ^    ^                     ^
 *   |                   |    |                     |
 *   +----> DEFAULT <----+    +---- WSS_CONNECTED <-+
 * 
 * POWER 上电模式
 * IDLE 空闲模式（无任务不代表低功耗）
 * DEFAULT 默认模式：
 *     默认开启BLE ADV，提供配网功能
 * NET_CONNECTING 连接模式
 *     尝试连接WIFI
 * NET_CONNECTED 已连接WIFI的模式
 *     尝试连接后端服务器
 * WSS_CONNECTED 后端服务器连接成功模式
 *     可以进行应用逻辑处理
 */
#include "net_connected.h"
#include "../lwl_app_framework.h"
#include "../lwl_app_core.h"
#include "../wss_connected/wss_application.h"
#include "log.h"
#include "ota/http_ota.h"

#include "wss/wss_client.h"

int app_net_connected_event_handler(struct application *app, struct sys_event *event)
{
    return false;
}

/**** websoket "ws://"  websokets "wss://"  *****/
//#define OBJ_URL 	"wss://lingos.mo.ling.space/ws/chat?x-key=FK3HRefdoeRGBl7BvlME&x-service=chat&x-version=1.0&x-time=1735125777&x-sign=9803adbaf97fc15477ec4fd58f34f764&x-client-type=web&x-client-id=TESTCLIENT&authorization=eyJ0eXAiOiJKV1QiLCJhbGciOiJIUzI1NiJ9.eyJ0aW1lU3RhbXAiOjE3MzM4MTA1NTU3MjUsInVzZXJfaWQiOiJ3bF90ZXN0IiwiZXhwIjoxNzQxNTg2NTU1fQ._xbl1hmKo3KPZhAep4te6L3ax3UWTClaq2zGtoQJE2s" //远程服务器测试
//#define OBJ_URL 	"wss://lingos.mo.ling.space/ws/chat?x-key=FK3HRefdoeRGBl7BvlME&x-service=chat&x-version=1.0&x-time=1737433533&x-sign=9803adbaf97fc15477ec4fd58f34f764&x-client-type=web&x-client-id=TESTCLIENT&authorization=eyJ0eXAiOiJKV1QiLCJhbGciOiJIUzI1NiJ9.eyJ0aW1lU3RhbXAiOjE3Mzc0MzI1NTcwODcsImRldmljZV9pZCI6IkMwMDEiLCJ1c2VyX2lkIjoiMSIsImV4cCI6MTczODI5NjU1N30.V9qTjpPbsoLBdgtCiNjE8oO3tXeCetyBzKmFDOH505M" //远程服务器测试
#define OBJ_URL 	"wss://lingos.mo.ling.space/ws/chat?x-key=FK3HRefdoeRGBl7BvlME&x-service=chat&x-version=1.0&x-time=1738743343&x-sign=5325d317208bcd41c9ba1d5398f026ca&x-client-type=web&x-client-id=TESTCLIENT&authorization=eyJ0eXAiOiJKV1QiLCJhbGciOiJIUzI1NiJ9.eyJ0aW1lU3RhbXAiOjE3Mzg3NDMzMzQwMzMsInVzZXJfaWQiOiJ3bC10ZXN0IiwiZXhwIjoxNzcwMjc5MzM0fQ.41i12uDxkGknn-9SiW8MrVsg8zB8vdr1kWFAJqAdWPc" //远程服务器测试
//#define OBJ_URL 	"wss://www.lingxilink.com:3000" //远程服务器测试
//#define OBJ_URL 	"ws://82.157.123.54:9010/ajaxchattest" //远程服务器测试
//#define OBJ_URL 	"ws://121.40.165.18:8800"
//#define OBJ_URL   "ws://www.example.com/socketserver"
//#define OBJ_URL 	"wss://localhost:8888"                  //本地服务器测试
//#define OBJ_URL 	"wss://172.16.23.28:8888"                  //本地服务器测试

char url[] = OBJ_URL;

void wss_recv_callback(u8 *buf, u32 len, u8 type)
{
    log_i("wbs recv msg : %s\n", buf);

    if (strcmp(buf, "client ping") == 0) {

    } else {
        // loswssp process
        loswssp_rsp_handler(buf, len);
    }
}


static const cJSON *cjson_parse(u8 *buf)
{
    const cJSON *nroot = cJSON_Parse(buf);
    if (nroot == NULL) {
        const char *error_ptr = cJSON_GetErrorPtr();
        if (error_ptr!= NULL) {
            log_i("INFO: failed to analysis JSON string at position %ld: %s", 
                ((u8 *)error_ptr - (u8 *)buf), error_ptr);
        } else {
            log_i("INFO: failed to analysis JSON string, unknown reason");
        }
    }
    return nroot;
}

int do_check_and_ota()
{
    char *ota_info_json = NULL;
    int ret = request_ota_info(&ota_info_json);
    if (ret < 0 || ota_info_json == NULL) {
        log_e("[UPGRADE]failed to request OTA info: %d", ret);
        return -1;
    }
    log_i("ota_info: %s", ota_info_json);

    const cJSON *nroot = cjson_parse(ota_info_json);
    if (nroot == NULL) {
        goto error_ret;
    }

    ota_info_t ota_info;
    if (parse_ota_info(nroot, &ota_info) != OTAERROR_Success) {
        goto error_ret;
    }

    lwl_app_context_t *ctx = lwl_app_core_context_get_instance();
    ctx->lw_ota_ver_code = 62; // test
    if (ctx->lw_ota_ver_code < ota_info.version_code) {
        log_i("%d < %d, need OTA.", ctx->lw_ota_ver_code, ota_info.version_code);
        log_i("download %s", ota_info.file_url);

        size_t url_len = strlen(ota_info.file_url);
        char *ufw_url = malloc(url_len + 1);
        strncpy(ufw_url, ota_info.file_url, url_len + 1);

        http_create_download_task(ufw_url);
    }

error_ret:
    if (nroot != NULL) {
        cJSON_Delete(nroot);
        nroot = NULL;
    }
    if (ota_info_json != NULL) {
        free(ota_info_json);
        ota_info_json = NULL;
    }
    return ret;
}

static int net_connected_init(void)
{
    // OTA check version
    // do_check_and_ota();


    struct websocket_info_t *websockets_info = wss_server_connect(url, wss_recv_callback);

    if (websockets_info == NULL) {
        log_i("websockets init failed\n");
        // TODO 尝试重连WSS
        // lwl_app_mode_switch_to(APP_WSS_CONNECTED_MODE);
    } else {
        /* 将websocket_info保存到全局上下文 */
        lwl_app_context_t *context = lwl_app_core_context_get_instance();
        context->wss_url = url;
        context->websockets_info = websockets_info;
        lwl_app_mode_switch_to(APP_WSS_CONNECTED_MODE);
    }
    return 0;
}

static int net_connected_unint(void)
{
    return 0;
}

void app_net_connected_mode(void)
{
    net_connected_init();

    while (1) {
        // 应用代码
        mdelay(10);
        if (lwl_app_mode_exitting()) {
            net_connected_unint();
            return;
        }
    }
}