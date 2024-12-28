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

#include "distance/main_ble_distance.h"

#define USE_WEBSOCKET_TEST

#ifdef USE_WEBSOCKET_TEST
/**** websoket "ws://"  websokets "wss://"  *****/

#define OBJ_URL 	"wss://lingos.mo.ling.space/ws/chat?x-key=FK3HRefdoeRGBl7BvlME&x-service=chat&x-version=1.0&x-time=1735125777&x-sign=9803adbaf97fc15477ec4fd58f34f764&x-client-type=web&x-client-id=TESTCLIENT&authorization=eyJ0eXAiOiJKV1QiLCJhbGciOiJIUzI1NiJ9.eyJ0aW1lU3RhbXAiOjE3MzM4MTA1NTU3MjUsInVzZXJfaWQiOiJ3bF90ZXN0IiwiZXhwIjoxNzQxNTg2NTU1fQ._xbl1hmKo3KPZhAep4te6L3ax3UWTClaq2zGtoQJE2s" //远程服务器测试
//#define OBJ_URL 	"wss://www.lingxilink.com:3000" //远程服务器测试
//#define OBJ_URL 	"ws://82.157.123.54:9010/ajaxchattest" //远程服务器测试
//#define OBJ_URL 	"ws://121.40.165.18:8800"
//#define OBJ_URL   "ws://www.example.com/socketserver"
//#define OBJ_URL 	"wss://localhost:8888"                  //本地服务器测试
//#define OBJ_URL 	"wss://172.16.23.28:8888"                  //本地服务器测试

static int websockets_client_send(struct websocket_struct *websockets_info, u8 *buf, int len, char type);

#include "uart.h"
void* uart2_hdl = NULL;

typedef enum {
    STATE_APP_IDLE,
    STATE_APP_SCAN_RSSI,
    STATE_CREATE_SESSION,
    STATE_COMMUNICATION,
} CommunicationAppState;

typedef enum {
    STATE_CREATE_ROLE1_SESSION_STARTUP,
    STATE_CREATE_ROLE1_SESSION_CREATING,
    STATE_CREATE_ROLE1_SESSION_CREATE_OK,
    STATE_CREATE_ROLE1_SESSION_SAY_HELLO,
    STATE_CREATE_ROLE2_SESSION_STARTUP,
    STATE_CREATE_ROLE2_SESSION_CREATING,
    STATE_CREATE_ROLE2_SESSION_CREATE_OK,
    STATE_CREATE_ROLE2_SESSION_SAY_HELLO,
} CommunicationCreateSessionState;

typedef enum {
    STATE_COMMUNICATION_IDLE,
    STATE_COMMUNICATION_SWITCH_ROLE,
    STATE_COMMUNICATION_WAIT_RSP,
    STATE_COMMUNICATION_PLAY_AUDIO,
    STATE_COMMUNICATION_PLAYER_SPEAK,
    STATE_COMMUNICATION_RECORDING,
} CommunicationState;

#define URL_MAX_LEN 256
#define CONTENT_MAX_LEN 256
typedef struct {
    char url[URL_MAX_LEN];
    char content[CONTENT_MAX_LEN];
} communication_audio_event_t;

#define ADV_BUF_SIZE 10
#define ADV_MAX_LEN 16
#define SN_MAX_SIZE 16
typedef struct {
    int ip_id;
    int session_id;
    char adv_name[ADV_MAX_LEN];
    u16 adv_name_len;
    char sn[SN_MAX_SIZE];
    s8 adv_rssi;
} communication_role_t;

typedef enum {
    ROLE1 = 0,
    ROLE2 = 1,
} communication_role_type;

u32 g_curr_time = 0;
#define HEAT_TIMEOUT 5000
u32 g_heat_timer = 0;
#define WAIT_PLAYER_TALK_TIMEOUT 5000
u32 g_wait_player_talk_timer = 0;

int g_communication_app_state = STATE_APP_IDLE;
int g_communication_session_create_state = STATE_CREATE_ROLE1_SESSION_STARTUP;
int g_communication_state = STATE_COMMUNICATION_IDLE;
int g_current_role = ROLE1;
int g_next_role = ROLE2;
int g_session_role1_create_ok = false;
int g_session_role2_create_ok = false;
int g_backend_switch_session_ok = false;
int g_ble_porte_switch_ok = false;
int g_audio_play_end = false;
int g_audio_play_ongoing_num = 0;
int g_backend_message_end_ok = false;
int g_recording = false;
int g_record_end = false;
int g_message_asr_end_ok = false;

#define CONTENT_MAX_LEN 512
char g_content[CONTENT_MAX_LEN];

static const char g_default_content[] = {
    // // 今天森林里有没有下雨？
    // 0xe4, 0xbb, 0x8a, 0xe5, 0xa4, 0xa9, 0xe6, 0xa3, 0xae, 0xe6, 0x9e, 0x97, 0xe9, 0x87, 0x8c, 0xe6, 0x9c, 0x89, 0xe6, 0xb2, 0xa1, 0xe6, 0x9c, 0x89, 0xe4, 0xb8, 0x8b, 0xe9, 0x9b, 0xa8, 0xef, 0xbc, 0x9f,
    // 小熊喜欢吃小兔子的胡萝卜吗？
    0xe5, 0xb0, 0x8f, 0xe7, 0x86, 0x8a, 0xe5, 0x96, 0x9c, 0xe6, 0xac, 0xa2, 0xe5, 0x90, 0x83, 0xe5, 0xb0, 0x8f, 0xe5, 0x85, 0x94, 0xe5, 0xad, 0x90, 0xe7, 0x9a, 0x84, 0xe8, 0x83, 0xa1, 0xe8, 0x90, 0x9d, 0xe5, 0x8d, 0x9c, 0xe5, 0x90, 0x97, 0xef, 0xbc, 0x9f,
};

communication_role_t g_role_table[] = {
    [ROLE1] = {
        .ip_id = 45,
        .session_id = 0,
        .sn = "WCCN24H28FEEEF7",
        .adv_name = "FR8010H_AT\0",
        .adv_name_len = 11,
        .adv_rssi = -127,
    },
    [ROLE2] = {
        .ip_id = 5,
        .session_id = 0,
        .sn = "WCCN24H28FEEEF8",
        .adv_name = "FR8010H_AT0\0",
        .adv_name_len = 12,
        .adv_rssi = -127,
    },
};
static const uint32_t g_role_table_size = sizeof(g_role_table) / sizeof(g_role_table[0]);

static void communication_talk_message(struct websocket_strcut *websockets_info, char *content)
{
    int err;
    char *send_data = NULL;

    cJSON *cjson_send_root = cJSON_CreateObject();
	cJSON_AddStringToObject(cjson_send_root,"uuid", "abcdefg-123456");
	cJSON_AddStringToObject(cjson_send_root,"event", "message");

    cJSON *cjson_send_root_data = cJSON_CreateObject();
	cJSON_AddStringToObject(cjson_send_root_data,"type", "text");
    cJSON_AddItemToObject(cjson_send_root, "data", cjson_send_root_data);

    cJSON *cjson_send_root_data_attributes = cJSON_CreateObject();
	cJSON_AddBoolToObject(cjson_send_root_data_attributes,"asr", false);
	cJSON_AddStringToObject(cjson_send_root_data_attributes,"content", content);
	cJSON_AddBoolToObject(cjson_send_root_data_attributes,"needs_tts", true);
    cJSON_AddItemToObject(cjson_send_root_data, "attributes", cjson_send_root_data_attributes);

	send_data = cJSON_PrintUnformatted(cjson_send_root);

    printf("send: %s\n", send_data);

    //SSL加密时一次发送数据不能超过16K，用户需要自己分包
    err = websockets_client_send(websockets_info, (u8 *)send_data, strlen(send_data), WCT_TXTDATA);
    if (FALSE == err) {
        printf("  . ! send ping err !!!\r\n");
             ///goto exit_ws;
    }

    free(send_data);
    if (NULL != cjson_send_root) {
        cJSON_Delete(cjson_send_root);
    }
}

static void communication_init_role(struct websocket_struct *websockets_info, int role_type)
{
    int err;
    char *send_data = NULL;

    cJSON *cjson_send_root = cJSON_CreateObject();
	cJSON_AddStringToObject(cjson_send_root,"uuid", "abcdefg-123456");
	cJSON_AddStringToObject(cjson_send_root,"event", "session");

    cJSON *cjson_send_root_data = cJSON_CreateObject();
    cJSON_AddItemToObject(cjson_send_root, "data", cjson_send_root_data);

    cJSON *cjson_send_root_data_attributes = cJSON_CreateObject();
	cJSON_AddNumberToObject(cjson_send_root_data_attributes,"ip_id", g_role_table[role_type].ip_id);
	cJSON_AddStringToObject(cjson_send_root_data_attributes,"nickname", "esp32-testss");
	cJSON_AddBoolToObject(cjson_send_root_data_attributes,"needs_tts", true);
    cJSON_AddItemToObject(cjson_send_root_data, "attributes", cjson_send_root_data_attributes);

	send_data = cJSON_PrintUnformatted(cjson_send_root);

    printf("send: %s\n", send_data);

    //SSL加密时一次发送数据不能超过16K，用户需要自己分包
    err = websockets_client_send(websockets_info, (u8 *)send_data, strlen(send_data), WCT_TXTDATA);
    if (FALSE == err) {
        printf("  . ! send ping err !!!\r\n");
             ///goto exit_ws;
    }

    free(send_data);
    if (NULL != cjson_send_root) {
        cJSON_Delete(cjson_send_root);
    }
}

static void wss_2ip_switch(struct websocket_struct *websockets_info, int role_type)
{
    u8 full_id[16];
    strcpy(full_id, "abcdefg-123456");
#if 1
    get_random_number(&full_id[8], 6);
#endif

    int err;
    char *send_data = NULL;

    cJSON *cjson_send_root = cJSON_CreateObject();
	cJSON_AddStringToObject(cjson_send_root,"uuid", "abcdefg-123456");
	cJSON_AddStringToObject(cjson_send_root,"event", "session");

    cJSON *cjson_send_root_data = cJSON_CreateObject();
    cJSON_AddItemToObject(cjson_send_root, "data", cjson_send_root_data);

    cJSON *cjson_send_root_data_attributes = cJSON_CreateObject();
	cJSON_AddNumberToObject(cjson_send_root_data_attributes,"session_id", g_role_table[role_type].session_id);
	cJSON_AddNumberToObject(cjson_send_root_data_attributes,"ip_id", g_role_table[role_type].ip_id);
	cJSON_AddStringToObject(cjson_send_root_data_attributes,"nickname", "esp32-testss");
	cJSON_AddBoolToObject(cjson_send_root_data_attributes,"needs_tts", true);
	cJSON_AddBoolToObject(cjson_send_root_data_attributes,"needs_recovery", true);
    cJSON_AddItemToObject(cjson_send_root_data, "attributes", cjson_send_root_data_attributes);

	send_data = cJSON_PrintUnformatted(cjson_send_root);

    printf("send: %s\n", send_data);

    //SSL加密时一次发送数据不能超过16K，用户需要自己分包
    err = websockets_client_send(websockets_info, (u8 *)send_data, strlen(send_data), WCT_TXTDATA);
    if (FALSE == err) {
        printf("  . ! send ping err !!!\r\n");
             ///goto exit_ws;
    }

    free(send_data);
    if (NULL != cjson_send_root) {
        cJSON_Delete(cjson_send_root);
    }
}

static void communication_message_asr(struct websocket_strcut *websockets_info)
{
    int err;
    char *send_data = NULL;

    cJSON *cjson_send_root = cJSON_CreateObject();
	cJSON_AddStringToObject(cjson_send_root,"uuid", "abcdefg-123456");
	cJSON_AddStringToObject(cjson_send_root,"event", "message");

    cJSON *cjson_send_root_data = cJSON_CreateObject();
	cJSON_AddStringToObject(cjson_send_root_data,"type", "text");
    cJSON_AddItemToObject(cjson_send_root, "data", cjson_send_root_data);

    cJSON *cjson_send_root_data_attributes = cJSON_CreateObject();
	cJSON_AddBoolToObject(cjson_send_root_data_attributes,"asr", true);
    cJSON_AddItemToObject(cjson_send_root_data, "attributes", cjson_send_root_data_attributes);

	send_data = cJSON_PrintUnformatted(cjson_send_root);

    printf("send: %s\n", send_data);

    //SSL加密时一次发送数据不能超过16K，用户需要自己分包
    err = websockets_client_send(websockets_info, (u8 *)send_data, strlen(send_data), WCT_TXTDATA);
    if (FALSE == err) {
        printf("  . ! send ping err !!!\r\n");
             ///goto exit_ws;
    }

    free(send_data);
    if (NULL != cjson_send_root) {
        cJSON_Delete(cjson_send_root);
    }
}

static void communication_message_asr_start(struct websocket_strcut *websockets_info)
{
    int err;
    char *send_data = NULL;

    cJSON *cjson_send_root = cJSON_CreateObject();
	cJSON_AddStringToObject(cjson_send_root,"uuid", "abcdefg-123456");
	cJSON_AddStringToObject(cjson_send_root,"event", "message_asr_start");

    cJSON *cjson_send_root_data = cJSON_CreateObject();
    cJSON_AddItemToObject(cjson_send_root, "data", cjson_send_root_data);

    cJSON *cjson_send_root_data_attributes = cJSON_CreateObject();
	cJSON_AddBoolToObject(cjson_send_root_data_attributes,"auto", false);
    cJSON_AddItemToObject(cjson_send_root_data, "attributes", cjson_send_root_data_attributes);

	send_data = cJSON_PrintUnformatted(cjson_send_root);

    printf("send: %s\n", send_data);

    //SSL加密时一次发送数据不能超过16K，用户需要自己分包
    err = websockets_client_send(websockets_info, (u8 *)send_data, strlen(send_data), WCT_TXTDATA);
    if (FALSE == err) {
        printf("  . ! send ping err !!!\r\n");
             ///goto exit_ws;
    }

    free(send_data);
    if (NULL != cjson_send_root) {
        cJSON_Delete(cjson_send_root);
    }
}

static void communication_message_asr_end(struct websocket_strcut *websockets_info)
{
    int err;
    char *send_data = NULL;

    cJSON *cjson_send_root = cJSON_CreateObject();
	cJSON_AddStringToObject(cjson_send_root,"uuid", "abcdefg-123456");
	cJSON_AddStringToObject(cjson_send_root,"event", "message_asr_end");

    cJSON *cjson_send_root_data = cJSON_CreateObject();
    cJSON_AddItemToObject(cjson_send_root, "data", cjson_send_root_data);

	send_data = cJSON_PrintUnformatted(cjson_send_root);

    printf("send: %s\n", send_data);

    //SSL加密时一次发送数据不能超过16K，用户需要自己分包
    err = websockets_client_send(websockets_info, (u8 *)send_data, strlen(send_data), WCT_TXTDATA);
    if (FALSE == err) {
        printf("  . ! send ping err !!!\r\n");
             ///goto exit_ws;
    }

    free(send_data);
    if (NULL != cjson_send_root) {
        cJSON_Delete(cjson_send_root);
    }
}

static void communication_message_asr_data(struct websocket_strcut *websockets_info, char *data_base64)
{
    int err;
    char *send_data = NULL;

    cJSON *cjson_send_root = cJSON_CreateObject();
	cJSON_AddStringToObject(cjson_send_root,"uuid", "abcdefg-123456");
	cJSON_AddStringToObject(cjson_send_root,"event", "message_asr_data");

    cJSON *cjson_send_root_data = cJSON_CreateObject();
    cJSON_AddItemToObject(cjson_send_root, "data", cjson_send_root_data);

    cJSON *cjson_send_root_data_attributes = cJSON_CreateObject();
	cJSON_AddStringToObject(cjson_send_root_data_attributes,"data", data_base64);
    cJSON_AddItemToObject(cjson_send_root_data, "attributes", cjson_send_root_data_attributes);

	send_data = cJSON_PrintUnformatted(cjson_send_root);

    // printf("send: %s\n", send_data);

    //SSL加密时一次发送数据不能超过16K，用户需要自己分包
    err = websockets_client_send(websockets_info, (u8 *)send_data, strlen(send_data), WCT_TXTDATA);
    if (FALSE == err) {
        printf("  . ! send ping err !!!\r\n");
             ///goto exit_ws;
    }

    free(send_data);
    if (NULL != cjson_send_root) {
        cJSON_Delete(cjson_send_root);
    }
}

static void communication_message_stop(struct websocket_strcut *websockets_info)
{
    int err;
    char *send_data = NULL;

    cJSON *cjson_send_root = cJSON_CreateObject();
	cJSON_AddStringToObject(cjson_send_root,"uuid", "abcdefg-123456");
	cJSON_AddStringToObject(cjson_send_root,"event", "message_stop");

    cJSON *cjson_send_root_data = cJSON_CreateObject();
    cJSON_AddItemToObject(cjson_send_root, "data", cjson_send_root_data);

	send_data = cJSON_PrintUnformatted(cjson_send_root);

    printf("send: %s\n", send_data);

    //SSL加密时一次发送数据不能超过16K，用户需要自己分包
    err = websockets_client_send(websockets_info, (u8 *)send_data, strlen(send_data), WCT_TXTDATA);
    if (FALSE == err) {
        printf("  . ! send ping err !!!\r\n");
             ///goto exit_ws;
    }

    free(send_data);
    if (NULL != cjson_send_root) {
        cJSON_Delete(cjson_send_root);
    }
}

communication_audio_event_t g_audio_event_param;

static void audio_play_end_callback(void *priv)
{
    printf("Audio play over\n");
    g_audio_play_end = true;
    g_audio_play_ongoing_num--;
    printf("Audio play ongoing num: %d\n", g_audio_play_ongoing_num);
}

static void switch_end_callback(void *priv)
{
    printf("Switch PortE OK\n");
    g_ble_porte_switch_ok = true;
}

static void record_end_cb(void *priv)
{
    printf("Record end\n");
    g_record_end = true;
}

static void scan_rssi_record_cb(void *priv, char *dev_name, u16 dev_name_len, s8 rssi)
{
    for (uint32_t i = 0; i < g_role_table_size; ++i) {
        if (dev_name_len == g_role_table[i].adv_name_len && 
            memcmp(dev_name, g_role_table[i].adv_name, dev_name_len) == 0) {
            printf("RSSI = %d, len = %d, name: %s", rssi, dev_name_len, dev_name);
            g_role_table[i].adv_rssi = rssi;
        }
    }
}

static void scan_rssi_cb(void *priv)
{
    printf("start scan RSSI call over\n");
}

static void communication_session_event_handler(const cJSON *nroot)
{
    const cJSON *ndata = cJSON_GetObjectItem(nroot, "data");
    ASSERT(ndata != NULL);

    cJSON *nnew_session = cJSON_GetObjectItem(ndata, "new_session");
    cJSON *nsession_id = cJSON_GetObjectItem(ndata, "session_id");

    ASSERT(cJSON_IsBool(nnew_session));
    ASSERT(cJSON_IsNumber(nsession_id));

    if (nnew_session->type == cJSON_True) {
        int session_id = nsession_id->valueint;

        if (g_communication_app_state == STATE_CREATE_SESSION) {
            if (g_communication_session_create_state == STATE_CREATE_ROLE1_SESSION_CREATING) {
                printf("get role1 session id: %d\n", session_id);
                g_role_table[ROLE1].session_id = session_id;
                // g_session_role1_create_ok = true;
            } else if (g_communication_session_create_state == STATE_CREATE_ROLE2_SESSION_CREATING) {
                printf("get role2 session id: %d\n", session_id);
                g_role_table[ROLE2].session_id = session_id;
                // g_session_role2_create_ok = true;
            }
        }
    } else if (nnew_session->type == cJSON_False) {
        /* 请求后端切换session时，若session已存在，则new_session字段为False */
        if (g_communication_app_state == STATE_COMMUNICATION) {
            if (g_communication_state == STATE_COMMUNICATION_SWITCH_ROLE) {
                g_backend_switch_session_ok = true;
            }
        }
    }
}

static void communication_message_end_event_handler(const cJSON *nroot)
{
    printf("Communication message end.\n");

    if (g_communication_app_state == STATE_COMMUNICATION) {
        if (g_communication_state == STATE_COMMUNICATION_PLAY_AUDIO) {
            g_backend_message_end_ok = true;
        }
    } else if (g_communication_app_state == STATE_CREATE_SESSION) {
        if (g_communication_session_create_state == STATE_CREATE_ROLE1_SESSION_CREATING) {
            g_session_role1_create_ok = true;
        } else if (g_communication_session_create_state == STATE_CREATE_ROLE2_SESSION_CREATING) {
            g_session_role2_create_ok = true;
        } else if (g_communication_session_create_state == STATE_CREATE_ROLE1_SESSION_SAY_HELLO) {
            g_backend_message_end_ok = true;
        } else if (g_communication_session_create_state == STATE_CREATE_ROLE2_SESSION_SAY_HELLO) {
            g_backend_message_end_ok = true;
        }
    }
}

static void communication_message_audio_event_handler(const cJSON *nroot)
{
    const cJSON *ndata = cJSON_GetObjectItem(nroot, "data");
    ASSERT(ndata != NULL);
    const cJSON *nmessage_id = cJSON_GetObjectItem(ndata, "message_id");
    ASSERT(cJSON_IsNumber(nmessage_id));
    const int vmessage_id = nmessage_id->valueint;
    printf("message id     :%d", vmessage_id);
    const cJSON *naudios = cJSON_GetObjectItem(ndata, "audios");
    ASSERT(cJSON_IsArray(naudios));
    int audio_num = cJSON_GetArraySize(naudios);
    for (int i = 0; i < audio_num; ++i) {
        cJSON *naudios_item = cJSON_GetArrayItem(naudios, i);

        cJSON *ncontent = cJSON_GetObjectItem(naudios_item, "content");
        cJSON *nurl = cJSON_GetObjectItem(naudios_item, "url");

        char* vcontent = cJSON_GetStringValue(ncontent);
        char* vurl = cJSON_GetStringValue(nurl);

        if (g_communication_app_state == STATE_COMMUNICATION) {
            if (g_communication_state == STATE_COMMUNICATION_PLAY_AUDIO) {
                /* 保存对话内容，用于之后和另一个角色对话 */
                strcpy(g_audio_event_param.content, vcontent);
                strcpy(g_audio_event_param.url, vurl);

                printf("latest content :%s", g_audio_event_param.content);
                printf("latest url     :%s", g_audio_event_param.url);

                /* 执行任务播放音乐，并注册回调，播放完成时处理 */
                my_task_play_one(vmessage_id, vurl, NULL, audio_play_end_callback);
                g_audio_play_ongoing_num++;
            }
        } else if (g_communication_app_state == STATE_CREATE_SESSION) {
            if (g_communication_session_create_state == STATE_CREATE_ROLE1_SESSION_SAY_HELLO) {

                printf("latest content :%s", g_audio_event_param.content);
                printf("latest url     :%s", g_audio_event_param.url);

                /* 执行任务播放音乐，并注册回调，播放完成时处理 */
                my_task_play_one(vmessage_id, vurl, NULL, audio_play_end_callback);
                g_audio_play_ongoing_num++;
            }
        }
    }
}

static void communication_message_asr_end_event_handler(const cJSON *nroot)
{
    printf("Communication message ASR end.\n");

    if (g_communication_app_state == STATE_COMMUNICATION) {
        if (g_communication_state == STATE_COMMUNICATION_WAIT_RSP) {
            g_message_asr_end_ok = true;
        }
    }
}

static void communication_message_stop_event_handler(const cJSON *nroot)
{
    const cJSON *ndata = cJSON_GetObjectItem(nroot, "data");
    ASSERT(ndata != NULL);
    const cJSON *nmessage_id = cJSON_GetObjectItem(ndata, "message_id");
    ASSERT(cJSON_IsNumber(nmessage_id));
    const int vmessage_id = nmessage_id->valueint;
    printf("stop message id: %d\n", vmessage_id);
}


static void websockets_callback(u8 *buf, u32 len, u8 type)
{
    printf("wbs recv msg : %s\n", buf);

    const cJSON *nroot = cJSON_Parse(buf);
    const cJSON *nevent = cJSON_GetObjectItem(nroot, "event");
    if (nevent != NULL) {
        const char *vevent = cJSON_GetStringValue(nevent);
        ASSERT(vevent != NULL);
        if (strcmp(vevent, "message_audio") == 0) {
            communication_message_audio_event_handler(nroot);
        } else if (strcmp(vevent, "session") == 0) {
            communication_session_event_handler(nroot);
        } else if (strcmp(vevent, "message_end") == 0) {
            communication_message_end_event_handler(nroot);
        } else if (strcmp(vevent, "message_asr_end") == 0) {
            communication_message_asr_end_event_handler(nroot);
        } else if (strcmp(vevent, "message_stop") == 0) {
            communication_message_stop_event_handler(nroot);
        }
    }
    cJSON_Delete(nroot);
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
    return websockets_info->_init(websockets_info);
}

static int websockets_client_handshack(struct websocket_struct *websockets_info)
{
    return websockets_info->_handshack(websockets_info);
}

static int websockets_client_send(struct websocket_struct *websockets_info, u8 *buf, int len, char type)
{
    //SSL加密时一次发送数据不能超过16K，用户需要自己分包
    return websockets_info->_send(websockets_info, buf, len, type);
}

static void websockets_client_exit(struct websocket_struct *websockets_info)
{
    websockets_info->_exit(websockets_info);
}

static int wss_ping_send(struct websocket_struct *websockets_info)
{
    int err;

    cJSON* cjson_ret_root = NULL;
	//cJSON* cjson_data = NULL;
	char* send_data = NULL;

	cjson_ret_root = cJSON_CreateObject();
	cJSON_AddStringToObject(cjson_ret_root,"uuid","abcdefg-123456");
	cJSON_AddStringToObject(cjson_ret_root,"event","ping");

	send_data = cJSON_PrintUnformatted(cjson_ret_root);
	///tb_audio_data_send(send_data,strlen(send_data));


    //SSL加密时一次发送数据不能超过16K，用户需要自己分包
    err = websockets_client_send(websockets_info, (u8 *)send_data, strlen(send_data), WCT_TXTDATA);
    if (FALSE == err) {
        printf("  . ! send ping err !!!\r\n");
             ///goto exit_ws;
    }
	free(send_data);
    if (NULL != cjson_ret_root) {
        cJSON_Delete(cjson_ret_root);
    }

    return 0;
}

enum {
    WSS_TASK_MESSAGE_ASR_START,
    WSS_TASK_MESSAGE_ASR_DATA,
    WSS_TASK_MESSAGE_ASR_END,

    WSS_TASK_MESSAGE_MAX = 0xFF,
};

#define ASR_DATA_CHUNK_SIZE 1280
#define BASE64_BUF_MAX_SIZE 1709    // ((ASR_DATA_CHUNK_SIZE + 2) / 3) * 4 = 1708
static char base64_buf[BASE64_BUF_MAX_SIZE];

typedef struct {
    int dummy;
} WssMessageAsrStart;

typedef struct {
    u8 data[ASR_DATA_CHUNK_SIZE];
} WssMessageAsrData;

typedef struct {
    int dummy;
} WssMessageAsrEnd;

typedef struct {
    int msg;
    union {
        WssMessageAsrStart asr_start;
        WssMessageAsrData asr_data;
        WssMessageAsrEnd asr_end;
    };
} WssTaskMessage;

#define MY_TASK_MESSAGE_QUEUE_CAPACITY 10
static MyBlockingQueue g_message_queue; // MyBlockingQueue<WssTaskMessage>

static void wss_task_message_asr_start_handler(struct websocket_strcut *websockets_info, WssTaskMessage *message)
{
    ASSERT(message->msg == WSS_TASK_MESSAGE_ASR_START);
    WssMessageAsrStart *param = &message->asr_start;

    g_recording = true;

    communication_message_stop(websockets_info);
    communication_message_asr(websockets_info);
    communication_message_asr_start(websockets_info);
}

static void wss_task_message_asr_data_handler(struct websocket_strcut *websockets_info, WssTaskMessage *message)
{
    ASSERT(message->msg == WSS_TASK_MESSAGE_ASR_DATA);
    WssMessageAsrData *param = &message->asr_data;
    
    // printf_buf(param->data, ASR_DATA_CHUNK_SIZE);
    if (uart2_hdl) {
        dev_write(uart2_hdl, param->data, ASR_DATA_CHUNK_SIZE);
    }

    size_t write_len = 0;
    memset(base64_buf, 0, sizeof(base64_buf));
    int ret = mbedtls_base64_encode(base64_buf, BASE64_BUF_MAX_SIZE, &write_len, param->data, ASR_DATA_CHUNK_SIZE);
    ASSERT(ret == 0);
    ASSERT(write_len == (BASE64_BUF_MAX_SIZE - 1));
    communication_message_asr_data(websockets_info, base64_buf);

    static int cnt1 = 0;
    cnt1 += ASR_DATA_CHUNK_SIZE;
    printf("cnt1: %d\n", cnt1);
}

static void wss_task_message_asr_end_handler(struct websocket_strcut *websockets_info, WssTaskMessage *message)
{
    ASSERT(message->msg == WSS_TASK_MESSAGE_ASR_END);
    WssMessageAsrStart *param = &message->asr_start;

    communication_message_asr_end(websockets_info);

    g_recording = false;
}

WssTaskMessage message_temp;

void wss_asr_start(void)
{
    message_temp.msg = WSS_TASK_MESSAGE_ASR_START;
    my_blocking_queue_enqueue(&g_message_queue, &message_temp);
}

void wss_asr_data(void *data, u32 size)
{
    static int cnt2 = 0;
    cnt2 += ASR_DATA_CHUNK_SIZE;
    printf("cnt2: %d\n", cnt2);
    ASSERT(size == ASR_DATA_CHUNK_SIZE);
    message_temp.msg = WSS_TASK_MESSAGE_ASR_DATA;
    memcpy(message_temp.asr_data.data, data, size);
    my_blocking_queue_enqueue(&g_message_queue, &message_temp);
}

void wss_asr_end(void)
{
    message_temp.msg = WSS_TASK_MESSAGE_ASR_END;
    my_blocking_queue_enqueue(&g_message_queue, &message_temp);
}

void app_function(disatance_info_t *distance_info)
{
    printf("notify_function my\n");

    printf("adv name: %s\n", distance_info->adv_name);
    printf("adv name len: %d\n", distance_info->adv_name_len);

    for (int i = 0; i < g_role_table_size; ++i) {
        if (memcmp(g_role_table[i].adv_name, distance_info->adv_name, g_role_table[i].adv_name_len) == 0) {
            printf("sn: %s\n", g_role_table[i].sn);

            // my_task_switch_only(g_role_table[i].sn, NULL, switch_end_callback);
        }
    }
}

void self_function(disatance_info_t *arg, int distance_type)
{
    printf("self_function my\n");
}

#include "bt_ble/client/ble_simple_client.h"

WssTaskMessage message;
static void communication_app_loop(struct websocket_struct *websockets_info)
{
    g_curr_time = timer_get_ms();

    if (g_message_queue.size > 0) {
        /* 从消息队列取消息，不允许发生阻塞 */
        my_blocking_queue_dequeue(&g_message_queue, &message);

        printf("wss task get message: %d\n", message.msg);

        switch (message.msg) {
        case WSS_TASK_MESSAGE_ASR_START:
            wss_task_message_asr_start_handler(websockets_info, &message);
            break;
        case WSS_TASK_MESSAGE_ASR_DATA:
            wss_task_message_asr_data_handler(websockets_info, &message);
            break;
        case WSS_TASK_MESSAGE_ASR_END:
            wss_task_message_asr_end_handler(websockets_info, &message);
            break;
        }
        printf("Message over\n");
    }

    switch (g_communication_app_state) {
        case STATE_APP_IDLE:
            #if 0
            // 开始检测RSSI
            monitor_init(NULL);

            monitor_ble_ops ops;
            ops.start_scan = my_task_scan_rssi;
            ops.stop_scan = NULL;
            monitor_register_ble_ops(ops);

            monitor_register_device(g_role_table[ROLE1].adv_name, g_role_table[ROLE1].adv_name_len);
            monitor_register_device(g_role_table[ROLE2].adv_name, g_role_table[ROLE2].adv_name_len);

            monitor_start(NULL);

            g_communication_app_state = STATE_APP_SCAN_RSSI;

            #else 
            g_communication_app_state = STATE_APP_SCAN_RSSI;

            #endif
            break;
        case STATE_APP_SCAN_RSSI:
            break;
        case STATE_CREATE_SESSION:
            switch (g_communication_session_create_state) {
                case STATE_CREATE_ROLE1_SESSION_STARTUP:
                    communication_init_role(websockets_info, ROLE1);
                    g_session_role1_create_ok = false;
                    g_communication_session_create_state = STATE_CREATE_ROLE1_SESSION_CREATING;
                    break;
                case STATE_CREATE_ROLE1_SESSION_CREATING:
                    if (g_session_role1_create_ok) {
                        printf("role1 session create ok");
                        g_communication_session_create_state = STATE_CREATE_ROLE1_SESSION_CREATE_OK;
                    }
                    break;
                case STATE_CREATE_ROLE1_SESSION_CREATE_OK:
                    // g_audio_play_end = false;
                    // g_backend_message_end_ok = false;
                    // g_audio_play_ongoing_num = 0;

                    g_communication_session_create_state = STATE_CREATE_ROLE2_SESSION_STARTUP;
                    break;
                case STATE_CREATE_ROLE1_SESSION_SAY_HELLO:
                    if (g_backend_message_end_ok && g_audio_play_ongoing_num == 0) {
                        g_communication_session_create_state = STATE_CREATE_ROLE2_SESSION_STARTUP;
                    }
                    break;
                case STATE_CREATE_ROLE2_SESSION_STARTUP:
                    communication_init_role(websockets_info, ROLE2);
                    g_session_role2_create_ok = false;
                    g_communication_session_create_state = STATE_CREATE_ROLE2_SESSION_CREATING;
                    break;
                case STATE_CREATE_ROLE2_SESSION_CREATING:
                    if (g_session_role2_create_ok) {
                        printf("role2 session create ok");
                        g_communication_session_create_state = STATE_CREATE_ROLE2_SESSION_CREATE_OK;
                    }
                    break;
                case STATE_CREATE_ROLE2_SESSION_CREATE_OK:
                    g_communication_session_create_state = STATE_CREATE_ROLE1_SESSION_STARTUP;
                    g_communication_state = STATE_COMMUNICATION_IDLE;
                    g_communication_app_state = STATE_COMMUNICATION;

                    // g_audio_play_end = false;
                    // g_backend_message_end_ok = false;
                    // g_audio_play_ongoing_num = 0;
                    // g_communication_session_create_state = STATE_CREATE_ROLE2_SESSION_SAY_HELLO;
                    break;
                case STATE_CREATE_ROLE2_SESSION_SAY_HELLO:
                    if (g_backend_message_end_ok && g_audio_play_ongoing_num == 0) {
                        g_communication_session_create_state = STATE_CREATE_ROLE1_SESSION_STARTUP;
                        g_communication_state = STATE_COMMUNICATION_IDLE;
                        g_communication_app_state = STATE_COMMUNICATION;
                    }
                    break;
            }
            break;
        case STATE_COMMUNICATION:
            switch (g_communication_state) {
                case STATE_COMMUNICATION_IDLE:
                    /* set context */
                    memcpy(g_content, g_default_content, sizeof(g_default_content));
                    g_backend_switch_session_ok = false;
                    g_ble_porte_switch_ok = false;
                    g_next_role = ROLE1;

                    /* backend switch session */
                    wss_2ip_switch(websockets_info, g_next_role);

                    /* BLE switch Port E */
                    my_task_switch_only(g_role_table[g_next_role].sn, NULL, switch_end_callback);

                    printf("IDLE -> SWITCH_ROLE\n");
                    g_communication_state = STATE_COMMUNICATION_SWITCH_ROLE;
                    break;
                case STATE_COMMUNICATION_SWITCH_ROLE:
                    // if (g_backend_switch_session_ok && g_ble_porte_switch_ok) {
                    if (g_backend_switch_session_ok) {
                        g_current_role = g_next_role;
                        /* Send message event request audio */
                        communication_talk_message(websockets_info, g_content);

                        g_audio_play_end = false;
                        g_backend_message_end_ok = false;
                        g_audio_play_ongoing_num = 0;
                        printf("SWITCH_ROLE -> PLAY_AUDIO\n");
                        g_communication_state = STATE_COMMUNICATION_PLAY_AUDIO;
                    }
                    break;
                case STATE_COMMUNICATION_PLAY_AUDIO:
                    if (g_recording == true) {
                        /* 播放期间发生按键录音则进入录音状态 */
                        printf("PLAY_AUDIO -> RECORDING\n");
                        g_communication_state = STATE_COMMUNICATION_RECORDING;
                    }
                    if (g_backend_message_end_ok && g_audio_play_ongoing_num == 0) {
                        /* 播放完成等待按键录音 */
                        g_wait_player_talk_timer = g_curr_time;
                        printf("PLAY_AUDIO -> PLAYER_SPEAK\n");
                        g_communication_state = STATE_COMMUNICATION_PLAYER_SPEAK;
                    }
                    break;
                case STATE_COMMUNICATION_PLAYER_SPEAK:
                    if (g_recording == true) {
                        /* 等待按键录音期间发生按键录音则进入录音状态 */
                        printf("PLAYER_SPEAK -> RECORDING\n");
                        g_communication_state = STATE_COMMUNICATION_RECORDING;
                    }
                    if (g_curr_time - g_wait_player_talk_timer >= WAIT_PLAYER_TALK_TIMEOUT) {
                        /* 等待按键录音期间没有发生按键录音则自动切换另一个角色 */
                        /* Store content */
                        strcpy(g_content, g_audio_event_param.content);
                        g_backend_switch_session_ok = false;
                        g_ble_porte_switch_ok = false;
                        g_next_role = (g_current_role == ROLE1) ? ROLE2 : ROLE1;

                        /* backend switch session */
                        wss_2ip_switch(websockets_info, g_next_role);

                        /* BLE switch Port E */
                        my_task_switch_only(g_role_table[g_next_role].sn, NULL, switch_end_callback);

                        printf("ROLE: %d -> %d, content: %s\n", g_current_role, g_next_role, g_content);
                        printf("PLAYER_SPEAK -> SWITCH_ROLE\n");
                        g_communication_state = STATE_COMMUNICATION_SWITCH_ROLE;
                    }
                    break;
                case STATE_COMMUNICATION_RECORDING:
                    if (g_recording == false) {
                        /* 录音结束后重新进入播放音频阶段 */
                        g_audio_play_end = false;
                        g_backend_message_end_ok = false;
                        g_audio_play_ongoing_num = 0;
                        printf("RECORDING -> PLAY_AUDIO\n");
                        g_communication_state = STATE_COMMUNICATION_PLAY_AUDIO;
                    }
                    break;
            }
            break;
    }

    if (g_curr_time - g_heat_timer >= HEAT_TIMEOUT) {
        wss_ping_send(websockets_info);
        g_heat_timer = g_curr_time;
    }
    mdelay(10);
}

/*******************************************************************************
*   Websocket Client.c
*   Just one example for test
*******************************************************************************/
static void websockets_client_main_thread(void *priv)
{
    int err;
    char mode = WEBSOCKETS_MODE;
#ifdef WIN32
    void *heart_thread_hdl = NULL;
    void *recv_thread_hdl = NULL;
#endif
    u8 url[512] = OBJ_URL;
    const char *input_str = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
    const char *origin_str = "myapp://127.0.0.1";

    printf(" . ----------------- Client Websocket ------------------\r\n");

#ifdef WIN32
    printf(" . Please input the mode for websocket : 1 . websocket , 2 . websockets (ssl)\r\n");
    scanf("%d", &mode);
    if (mode == 2) {
        mode = WEBSOCKETS_MODE;
        printf(" . Please input the URL , for example: wss://localhost:8888\r\n");
    } else {
        mode = WEBSOCKETS_MODE;
        printf(" . Please input the URL , for example: ws://localhost:8888\r\n");
    }

    scanf("%d", &url);
#endif

    /* 0 . malloc buffer */
    struct websocket_struct *websockets_info = malloc(sizeof(struct websocket_struct));
    memset(websockets_info, 0, sizeof(struct websocket_struct));
    if (!websockets_info) {
        return;
    }
    /* 1 . register */
    websockets_client_reg(websockets_info, mode);

//     const char ssl_cert[]  = \
//         "-----BEGIN CERTIFICATE-----\n" \
//         "MIICqTCCAZECCQDG9wED68PrZDANBgkqhkiG9w0BAQUFADASMRAwDgYDVQQDDAdn\n" \
//         "YW9mYW5nMB4XDTE1MTIwMTA2NDc1MVoXDTI5MDgwOTA2NDc1MVowGzEZMBcGA1UE\n" \
//         "AwwQd3d3Lm5vdGV4aXN0LmNvbTCCASIwDQYJKoZIhvcNAQEBBQADggEPADCCAQoC\n" \
//         "ggEBAOITLT3cNp6TrxjiGhTjfpRXTKMutMysPEmrhYIWnSjimDkLe8mXP96jk4IB\n" \
//         "3tOMv2Jtqn+YbMKm2AgM4oHs5u9YB/7MZCpUPityObN8G+73PHVO5npW0EUQcZaf\n" \
//         "dqHB153ykKRurpXLpTRG+s2NdzWpN8pDJ1H2JNbHcd5wq6tzz/vD5maRgAkxkXhp\n" \
//         "RZptG0OxgYCqv+NFXC+uMINBFSPvOkkqxJckLggZc/Mueh9F7PaWA72lKoXbV4g8\n" \
//         "nBjcYOlPXDJKir6kD050OpVMLBeaqQZNHsbsoY9ietuf7TT58srRNpEogPSIbK9Y\n" \
//         "0PXvudeuiupNEzH/fFf5wucOOaUCAwEAATANBgkqhkiG9w0BAQUFAAOCAQEAGjiD\n" \
//         "v+DfLHOl606QFHTW60XQek5xcCRDpeGnwGS6ytTnANiK7Z19lkk38hZPVHxEArd0\n" \
//         "62EcYPWd2Oy1vs6MTXcJSfN9Z7rZ9fmxWr2BFN2XbahgnsSXM48ixZJ4krc+1M+j\n" \
//         "2kcubVpsE2cgHdj4v8H6jUz9Ji4mr7vMNS6dXv8PUkl/qoDeNGCNdyTS5NIL5ir+\n" \
//         "g92cL8IGOkjgvhlqt9vc65Cgb4mL+n5+DV9uOyTZTW/MojmlgfUekC2xiXa54nxJ\n" \
//         "f17Y1TADGSbyJbsC0Q9nIrHsPl8YKkvRWvIAqYxXZ7wRwWWmv4TMxFhWRiNY7yZI\n" \
//         "o2ZUhl02SIDNggIEeg==\n" \
//         "-----END CERTIFICATE-----\n";
//     printf("sizeof(ssl_cert) %d\r\n", sizeof(ssl_cert));
//     websockets_mbtls_client_set_ca(&websockets_info->websockets_mbtls_info, ssl_cert, sizeof(ssl_cert));

    /* 2 . init */
    err = websockets_client_init(websockets_info, url, origin_str);
    if (FALSE == err) {
        printf("  . ! Cilent websocket init error !!!\r\n");
        goto exit_ws;
    }

    /* 3 . hanshack */
    err = websockets_client_handshack(websockets_info);
    if (FALSE == err) {
        printf("  . ! Handshake error !!!\r\n");
        goto exit_ws;
    }
    printf(" . Handshake success  %s \r\n",url);

    /* 4 . CreateThread */
#ifdef WIN32
    heart_thread_hdl = CreateThread(NULL, 0, websockets_info->_heart_thread, websockets_info, 0, &websockets_info->ping_thread_id);
    recv_thread_hdl = CreateThread(NULL, 0, websockets_info->_recv_thread,  websockets_info, 0, &websockets_info->ping_thread_id);
#else
    thread_fork("websocket_client_heart", 19, 512, 0,
                &websockets_info->ping_thread_id,
                websockets_info->_heart_thread,
                websockets_info);
    thread_fork("websocket_client_recv", 18, 512, 0,
                &websockets_info->recv_thread_id,
                websockets_info->_recv_thread,
                websockets_info);
#endif
    websockets_sleep(1000);

    u8 full_id[6];
    get_random_number(full_id, 6);
    printf_buf(full_id, 6);

    printf("Init message queue\n");
    my_blocking_queue_init(&g_message_queue, sizeof(WssTaskMessage), MY_TASK_MESSAGE_QUEUE_CAPACITY);

    uart2_hdl = dev_open("uart2", NULL);
    if (!uart2_hdl) { 
        printf("open uart err !!!\n");
        return ;
    }

    /* 使能特殊串口,启动收发数据 */
    dev_ioctl(uart2_hdl, UART_START, 0);

    /* 5 . recv or send data */
    while (1) {
        communication_app_loop(websockets_info);

        // err = websockets_client_send(websockets_info, (u8 *)input_str, strlen(input_str), WCT_TXTDATA);
        // if (FALSE == err) {
        //     printf("  . ! send err !!!\r\n");
        //     goto exit_ws;
        // }
        // printf("  . Send massage : %s \r\n", input_str);
#ifdef WIN32
        printf(" . Please input the send massage\r\n");
        memset(input_str, 0, sizeof(input_str));
        scanf("%s", input_str);
#endif
    }

exit_ws:
    /* 6 . exit */
#ifdef WIN32
    TerminateThread(heart_thread_hdl, 0);
    TerminateThread(recv_thread_hdl, 0);
#else
    thread_kill(&websockets_info->ping_thread_id, KILL_REQ);
    thread_kill(&websockets_info->recv_thread_id, KILL_REQ);
#endif
    websockets_client_exit(websockets_info);
    free(websockets_info);
}

static void websocket_client_thread_create(void)
{
    thread_fork("websockets_client_main", 15, 512 * 3, 0, 0, websockets_client_main_thread, NULL);
}

static void http_websocket_start(void *priv)
{
    enum wifi_sta_connect_state state;
    while (1) {
        printf("Connecting to the network...\n");
        state = wifi_get_sta_connect_state();
        if (WIFI_STA_NETWORK_STACK_DHCP_SUCC == state) {
            printf("Network connection is successful!\n");
            break;
        }
        os_time_dly(500);
    }

    websocket_client_thread_create();
}

//应用程序入口,需要运行在STA模式下
void c_main(void *priv)
{
    if (thread_fork("http_websocket_start", 10, 512, 0, NULL, http_websocket_start, NULL) != OS_NO_ERR) {
        printf("thread fork fail\n");
    }
}

late_initcall(c_main);

#endif//USE_WEBSOCKET_TEST
