#ifndef __LOSWSSP_H__
#define __LOSWSSP_H__

#include "cpu.h"

#undef errno

enum LTX_EVENT_TYPE {
    LT_NONE,
    LT_SESSION,
    LT_MESSAGE,
    LT_MESSAGE_ASR_START,
    LT_MESSAGE_ASR_DATA,
    LT_MESSAGE_ASR_END,
    LT_PING,
};

enum LRX_EVENT_TYPE {
    LR_NONE,
    LR_SESSION,
    LR_MESSAGE_ASR_DATA,
    LR_MESSAGE_ASR_END,
    LR_MESSAGE_DELTA,
    LR_MESSAGE_AUDIO,
    LR_MESSAGE_FILE,
    LR_MESSAGE_END,
    LR_MESSAGE_STOP,
    LR_MESSAGE_POINT_DELTA,
    LR_PONG,
};

typedef struct {
    char *content;
    char *url;
} loswssp_pkg_data_audio_t;

typedef struct {
    char *type;
    char *url;
} loswssp_pkg_data_file_t;

typedef struct {
    int session_id;
    bool new_session;
} loswssp_pkg_session_t;

typedef struct {
    int message_id;
    char *content;
} loswssp_pkg_message_asr_data_t;

typedef struct {
    int message_id;
} loswssp_pkg_message_asr_end_t;

typedef struct {
    int message_id;
    char *content;
} loswssp_pkg_message_delta_t;

typedef struct {
    int message_id;
    int audio_num;
    loswssp_pkg_data_audio_t *audios;
} loswssp_pkg_message_audio_t;

typedef struct {
    int message_id;
    int file_num;
    loswssp_pkg_data_file_t *files;
} loswssp_pkg_message_file_t;

typedef struct {
    int message_id;
    char *content;
    bool has_cached_prologue_id;
    int cached_prologue_id;
    int audio_num;
    loswssp_pkg_data_audio_t *audios;
    int file_num;
    loswssp_pkg_data_file_t *files;
} loswssp_pkg_message_end_t;

typedef struct {
    int message_id;
} loswssp_pkg_message_stop_t;

typedef struct {
    int message_id;
    int points;
} loswssp_pkg_message_point_delta_t;

typedef struct {
} loswssp_pkg_pong_t;

typedef struct {
    u8 type;
    int errno;
    char *uuid;
    char *event;
    char *errmsg;
    char *debug;
    union {
        loswssp_pkg_session_t session;
        loswssp_pkg_message_asr_data_t message_asr_data;
        loswssp_pkg_message_asr_end_t message_asr_end;
        loswssp_pkg_message_delta_t message_delta;
        loswssp_pkg_message_audio_t message_audio;
        loswssp_pkg_message_file_t message_file;
        loswssp_pkg_message_end_t message_end;
        loswssp_pkg_message_stop_t message_stop;
        loswssp_pkg_message_point_delta_t message_point_delta;
        loswssp_pkg_pong_t pong;
    };
} loswssp_package_t;

typedef int (*g_loswssp_cbk_t)(int pkg_type, loswssp_package_t *pkg);

/**
 * @name    generate_uuid
 * @brief   生成UUID字符串
 * @details
 *      时间戳部分占UUID的前8个字符
 *      随机数部分占后面12个字符
 *      UUID格式为xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx，其中x是随机十六进制数字，y是特定范围的随机十六进制数字
 * 
 * @param[out]  uuid_str    buf大小至少为37字节，返回字符串\0结尾
 */
void generate_uuid(char *uuid_str);

/**
 * @name    loswssp_create_message_event_session
 * @brief   创建lingos session消息
 * @details
 *      返回指针指向创建的字符串，需要使用loswssp_destroy_message释放内存
 *      char *json_str = NULL;
 *      loswssp_create_message_event_session(5, "test", true, &json_str);
 *      if (json_str == NULL) {
 *          // error
 *      }
 *      ...
 *      loswssp_destroy_message(json_str);
 * @param[in]   uuid
 * @param[in]   ip_id
 * @param[in]   nickname
 * @param[in]   needs_tts
 * @param[out]  json_str
 */
void loswssp_create_message_event_session(const char* uuid, int ip_id, const char *nickname, bool needs_tts, char **json_str);

/**
 * @name    loswssp_create_message_event_session_switch
 * @brief   创建lingos session消息
 * @param[in]   uuid
 * @param[in]   session_id
 * @param[in]   ip_id
 * @param[in]   nickname
 * @param[in]   needs_tts
 * @param[in]   needs_recovery
 * @param[out]  json_str
 * @see loswssp_create_message_event_session
 */
void loswssp_create_message_event_session_switch(const char* uuid, int session_id, int ip_id, const char *nickname, bool needs_tts, bool needs_recovery, char **json_str);

/**
 * @name    loswssp_create_message_event_message_text
 * @brief   创建lingos message消息
 * @param[in]   uuid
 * @param[in]   content
 * @param[out]  json_str
 * @see loswssp_create_message_event_session
 */
void loswssp_create_message_event_message_text(const char* uuid, const char *content, char **json_str);

/**
 * @name    loswssp_create_message_event_message_asr
 * @brief   创建lingos message_asr消息
 * @param[in]   uuid
 * @param[out]  json_str
 * @see loswssp_create_message_event_session
 */
void loswssp_create_message_event_message_asr(const char* uuid, char **json_str);

/**
 * @name    loswssp_create_message_event_message_asr_start
 * @brief   创建lingos message_asr_start消息
 * @param[in]   uuid
 * @param[out]  json_str
 * @see loswssp_create_message_event_session
 */
void loswssp_create_message_event_message_asr_start(const char* uuid, char **json_str);

/**
 * @name    loswssp_create_message_event_message_asr_end
 * @brief   创建lingos message_asr_end消息
 * @param[in]   uuid
 * @param[out]  json_str
 * @see loswssp_create_message_event_session
 */
void loswssp_create_message_event_message_asr_end(const char* uuid, char **json_str);

/**
 * @name    loswssp_create_message_event_message_asr_data
 * @brief   创建lingos message消息
 * @param[in]   uuid
 * @param[in]   data_base64
 * @param[out]  json_str
 * @see loswssp_create_message_event_session
 */
void loswssp_create_message_event_message_asr_data(const char* uuid, const char *data_base64, char **json_str);

/**
 * @name    loswssp_create_message_event_message_stop
 * @brief   创建lingos message_stop消息
 * @param[in]   uuid
 * @param[out]  json_str
 * @see loswssp_create_message_event_session
 */
void loswssp_create_message_event_message_stop(const char* uuid, char **json_str);

/**
 * @name    loswssp_create_message_event_ping
 * @brief   创建lingos ping消息
 * @param[out]  json_str
 * @see loswssp_create_message_event_session
 */
void loswssp_create_message_event_ping(const char* uuid, char **json_str);

/**
 * @name    loswssp_destroy_message
 * @brief   销毁lingos ping消息
 * @param[in]  json_str
 * @see loswssp_create_message_event_session
 */
void loswssp_destroy_message(char *json_str);

void loswssp_register_rsp_cbk(g_loswssp_cbk_t loswssp_cbk);

void loswssp_rsp_handler(u8 *buf, u32 len);

#endif