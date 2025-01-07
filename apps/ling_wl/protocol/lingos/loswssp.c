#include "loswssp.h"

#include <string.h>
#include "cpu.h"
#include "cJSON_common/cJSON.h"
#include "sys_time.h"

enum ParseErrorCode {
    PARSE_Success,
    PARSE_Error,
};

#define LING_UUID_BUFSIZE 37

g_loswssp_cbk_t g_loswssp_cbk = NULL;

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
void generate_uuid(char *uuid_str) {
    uint8_t random_buf[12];
    uint32_t timestamp = timer_get_ms();

    memset(uuid_str, 0xFF, LING_UUID_BUFSIZE);

    // 获取时间戳部分
    snprintf(uuid_str, 9, "%08x", (unsigned long)timestamp);

    // 获取随机数部分
    get_ramdom_number(random_buf, sizeof(random_buf));

    printf_buf(random_buf, sizeof(random_buf));

    // 格式化UUID字符串
    snprintf(uuid_str + 8, 5, "-%04x", ((unsigned int)(random_buf[0]) << 8) | random_buf[1]);
    snprintf(uuid_str + 13, 5, "-4%03x", ((unsigned int)(random_buf[2]) >> 4));
    snprintf(uuid_str + 18, 5, "-y%03x", ((unsigned int)(random_buf[2]) & 0xf) << 4 | (random_buf[3] >> 4));
    snprintf(uuid_str + 23, 13, "-%04x%04x", ((unsigned int)(random_buf[3]) & 0xf) << 8 | random_buf[4],
             ((unsigned int)(random_buf[5]) << 8) | random_buf[6]);

    uuid_str[36] = '\0';

    printf_buf(uuid_str, LING_UUID_BUFSIZE);
}

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
void loswssp_create_message_event_session(const char* uuid, int ip_id, const char *nickname, bool needs_tts, char **json_str)
{
    cJSON *cjson_send_root = cJSON_CreateObject();
	cJSON_AddStringToObject(cjson_send_root,"uuid", uuid);
	cJSON_AddStringToObject(cjson_send_root,"event", "session");

    cJSON *cjson_send_root_data = cJSON_CreateObject();
    cJSON_AddItemToObject(cjson_send_root, "data", cjson_send_root_data);

    cJSON *cjson_send_root_data_attributes = cJSON_CreateObject();
	cJSON_AddNumberToObject(cjson_send_root_data_attributes,"ip_id", ip_id);
	cJSON_AddStringToObject(cjson_send_root_data_attributes,"nickname", nickname);
	cJSON_AddBoolToObject(cjson_send_root_data_attributes,"needs_tts", needs_tts);
    cJSON_AddItemToObject(cjson_send_root_data, "attributes", cjson_send_root_data_attributes);

	*json_str = cJSON_PrintUnformatted(cjson_send_root);

    if (NULL != cjson_send_root) {
        cJSON_Delete(cjson_send_root);
    }
}

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
void loswssp_create_message_event_session_switch(const char* uuid, int session_id, int ip_id, const char *nickname, bool needs_tts, bool needs_recovery, char **json_str)
{
    cJSON *cjson_send_root = cJSON_CreateObject();
	cJSON_AddStringToObject(cjson_send_root,"uuid", uuid);
	cJSON_AddStringToObject(cjson_send_root,"event", "session");

    cJSON *cjson_send_root_data = cJSON_CreateObject();
    cJSON_AddItemToObject(cjson_send_root, "data", cjson_send_root_data);

    cJSON *cjson_send_root_data_attributes = cJSON_CreateObject();
	cJSON_AddNumberToObject(cjson_send_root_data_attributes,"session_id", session_id);
	cJSON_AddNumberToObject(cjson_send_root_data_attributes,"ip_id", ip_id);
	cJSON_AddStringToObject(cjson_send_root_data_attributes,"nickname", nickname);
	cJSON_AddBoolToObject(cjson_send_root_data_attributes,"needs_tts", needs_tts);
	cJSON_AddBoolToObject(cjson_send_root_data_attributes,"needs_recovery", needs_recovery);
    cJSON_AddItemToObject(cjson_send_root_data, "attributes", cjson_send_root_data_attributes);

	*json_str = cJSON_PrintUnformatted(cjson_send_root);

    if (NULL != cjson_send_root) {
        cJSON_Delete(cjson_send_root);
    }
}

/**
 * @name    loswssp_create_message_event_message_text
 * @brief   创建lingos message消息
 * @param[in]   uuid
 * @param[in]   content
 * @param[out]  json_str
 * @see loswssp_create_message_event_session
 */
void loswssp_create_message_event_message_text(const char* uuid, const char *content, char **json_str)
{
    cJSON *cjson_send_root = cJSON_CreateObject();
	cJSON_AddStringToObject(cjson_send_root,"uuid", uuid);
	cJSON_AddStringToObject(cjson_send_root,"event", "message");

    cJSON *cjson_send_root_data = cJSON_CreateObject();
	cJSON_AddStringToObject(cjson_send_root_data,"type", "text");
    cJSON_AddItemToObject(cjson_send_root, "data", cjson_send_root_data);

    cJSON *cjson_send_root_data_attributes = cJSON_CreateObject();
	cJSON_AddBoolToObject(cjson_send_root_data_attributes,"asr", false);
	cJSON_AddStringToObject(cjson_send_root_data_attributes,"content", content);
	cJSON_AddBoolToObject(cjson_send_root_data_attributes,"needs_tts", true);
    cJSON_AddItemToObject(cjson_send_root_data, "attributes", cjson_send_root_data_attributes);

	*json_str = cJSON_PrintUnformatted(cjson_send_root);

    if (NULL != cjson_send_root) {
        cJSON_Delete(cjson_send_root);
    }
}

/**
 * @name    loswssp_create_message_event_message_asr
 * @brief   创建lingos message_asr消息
 * @param[in]   uuid
 * @param[out]  json_str
 * @see loswssp_create_message_event_session
 */
void loswssp_create_message_event_message_asr(const char* uuid, char **json_str)
{
    cJSON *cjson_send_root = cJSON_CreateObject();
	cJSON_AddStringToObject(cjson_send_root,"uuid", uuid);
	cJSON_AddStringToObject(cjson_send_root,"event", "message");

    cJSON *cjson_send_root_data = cJSON_CreateObject();
	cJSON_AddStringToObject(cjson_send_root_data,"type", "text");
    cJSON_AddItemToObject(cjson_send_root, "data", cjson_send_root_data);

    cJSON *cjson_send_root_data_attributes = cJSON_CreateObject();
	cJSON_AddBoolToObject(cjson_send_root_data_attributes,"asr", true);
    cJSON_AddItemToObject(cjson_send_root_data, "attributes", cjson_send_root_data_attributes);

	*json_str = cJSON_PrintUnformatted(cjson_send_root);

    if (NULL != cjson_send_root) {
        cJSON_Delete(cjson_send_root);
    }
}

/**
 * @name    loswssp_create_message_event_message_asr_start
 * @brief   创建lingos message_asr_start消息
 * @param[in]   uuid
 * @param[out]  json_str
 * @see loswssp_create_message_event_session
 */
void loswssp_create_message_event_message_asr_start(const char* uuid, char **json_str)
{
    cJSON *cjson_send_root = cJSON_CreateObject();
	cJSON_AddStringToObject(cjson_send_root,"uuid", uuid);
	cJSON_AddStringToObject(cjson_send_root,"event", "message_asr_start");

    cJSON *cjson_send_root_data = cJSON_CreateObject();
    cJSON_AddItemToObject(cjson_send_root, "data", cjson_send_root_data);

    cJSON *cjson_send_root_data_attributes = cJSON_CreateObject();
	cJSON_AddBoolToObject(cjson_send_root_data_attributes,"auto", false);
    cJSON_AddItemToObject(cjson_send_root_data, "attributes", cjson_send_root_data_attributes);

	*json_str = cJSON_PrintUnformatted(cjson_send_root);

    if (NULL != cjson_send_root) {
        cJSON_Delete(cjson_send_root);
    }
}

/**
 * @name    loswssp_create_message_event_message_asr_end
 * @brief   创建lingos message_asr_end消息
 * @param[in]   uuid
 * @param[out]  json_str
 * @see loswssp_create_message_event_session
 */
void loswssp_create_message_event_message_asr_end(const char* uuid, char **json_str)
{
    cJSON *cjson_send_root = cJSON_CreateObject();
	cJSON_AddStringToObject(cjson_send_root,"uuid", uuid);
	cJSON_AddStringToObject(cjson_send_root,"event", "message_asr_end");

    cJSON *cjson_send_root_data = cJSON_CreateObject();
    cJSON_AddItemToObject(cjson_send_root, "data", cjson_send_root_data);

	*json_str = cJSON_PrintUnformatted(cjson_send_root);

    if (NULL != cjson_send_root) {
        cJSON_Delete(cjson_send_root);
    }
}

/**
 * @name    loswssp_create_message_event_message_asr_data
 * @brief   创建lingos message消息
 * @param[in]   uuid
 * @param[in]   data_base64
 * @param[out]  json_str
 * @see loswssp_create_message_event_session
 */
void loswssp_create_message_event_message_asr_data(const char* uuid, const char *data_base64, char **json_str)
{
    cJSON *cjson_send_root = cJSON_CreateObject();
	cJSON_AddStringToObject(cjson_send_root,"uuid", uuid);
	cJSON_AddStringToObject(cjson_send_root,"event", "message_asr_data");

    cJSON *cjson_send_root_data = cJSON_CreateObject();
    cJSON_AddItemToObject(cjson_send_root, "data", cjson_send_root_data);

    cJSON *cjson_send_root_data_attributes = cJSON_CreateObject();
	cJSON_AddStringToObject(cjson_send_root_data_attributes,"data", data_base64);
    cJSON_AddItemToObject(cjson_send_root_data, "attributes", cjson_send_root_data_attributes);

	*json_str = cJSON_PrintUnformatted(cjson_send_root);

    if (NULL != cjson_send_root) {
        cJSON_Delete(cjson_send_root);
    }
}

/**
 * @name    loswssp_create_message_event_message_stop
 * @brief   创建lingos message_stop消息
 * @param[in]   uuid
 * @param[out]  json_str
 * @see loswssp_create_message_event_session
 */
void loswssp_create_message_event_message_stop(const char* uuid, char **json_str)
{
    cJSON *cjson_send_root = cJSON_CreateObject();
	cJSON_AddStringToObject(cjson_send_root,"uuid", uuid);
	cJSON_AddStringToObject(cjson_send_root,"event", "message_stop");

    cJSON *cjson_send_root_data = cJSON_CreateObject();
    cJSON_AddItemToObject(cjson_send_root, "data", cjson_send_root_data);

	*json_str = cJSON_PrintUnformatted(cjson_send_root);

    if (NULL != cjson_send_root) {
        cJSON_Delete(cjson_send_root);
    }
}

/**
 * @name    loswssp_create_message_event_ping
 * @brief   创建lingos ping消息
 * @param[out]  json_str
 * @see loswssp_create_message_event_session
 */
void loswssp_create_message_event_ping(const char* uuid, char **json_str)
{
    cJSON *cjson_send_root = cJSON_CreateObject();
	cJSON_AddStringToObject(cjson_send_root,"uuid", uuid);
	cJSON_AddStringToObject(cjson_send_root,"event", "ping");

	*json_str = cJSON_PrintUnformatted(cjson_send_root);

    if (NULL != cjson_send_root) {
        cJSON_Delete(cjson_send_root);
    }
}

/**
 * @name    loswssp_destroy_message
 * @brief   销毁lingos ping消息
 * @param[in]  json_str
 * @see loswssp_create_message_event_session
 */
void loswssp_destroy_message(char *json_str) 
{
    if (json_str == NULL)
        return;
    free(json_str);
}


// 假设没有double类型，不处理

const cJSON *cjson_parse(u8 *buf)
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

cJSON *cjson_get_object(cJSON *object, const char *string)
{
    const cJSON *nnode = cJSON_GetObjectItem(object, string);
    if (nnode == NULL || !cJSON_IsObject(nnode)) {
        if (nnode == NULL) {
            log_i("INFO: the '%s' key was not found in the JSON object.", string);
        } else {
            log_i("INFO: the '%s' key exists, but its value is not of object type.", string);
        }
        return NULL;
    }
    return nnode;
}

cJSON *cjson_get_array(cJSON *object, const char *string)
{
    const cJSON *nnode = cJSON_GetObjectItem(object, string);
    if (nnode == NULL || !cJSON_IsArray(nnode)) {
        if (nnode == NULL) {
            log_i("INFO: the '%s' key was not found in the JSON object.", string);
        } else {
            log_i("INFO: the '%s' key exists, but its value is not of array type.", string);
        }
        return NULL;
    }
    return nnode;
}

char *cjson_get_string_value(cJSON *object, const char *string)
{
    const cJSON *nnode = cJSON_GetObjectItem(object, string);
    if (nnode == NULL || !cJSON_IsString(nnode)) {
        if (nnode == NULL) {
            log_i("INFO: the '%s' key was not found in the JSON object.", string);
        } else {
            log_i("INFO: the '%s' key exists, but its value is not of string type.", string);
        }
        return NULL;
    }
    return cJSON_GetStringValue(nnode);
}

int cjson_get_integer_value(cJSON *object, const char *string, int *value)
{
    const cJSON *nnode = cJSON_GetObjectItem(object, string);
    if (nnode == NULL || !cJSON_IsNumber(nnode)) {
        if (nnode == NULL) {
            log_i("INFO: the '%s' key was not found in the JSON object.", string);
        } else {
            log_i("INFO: the '%s' key exists, but its value is not of number type.", string);
        }
        return 1;
    }
    *value = nnode->valueint;
    return 0;
}

int cjson_get_boolean_value(cJSON *object, const char *string, bool *value)
{
    const cJSON *nnode = cJSON_GetObjectItem(object, string);
    if (nnode == NULL || !cJSON_IsBool(nnode)) {
        if (nnode == NULL) {
            log_i("INFO: the '%s' key was not found in the JSON object.", string);
        } else {
            log_i("INFO: the '%s' key exists, but its value is not of boolean type.", string);
        }
        return 1;
    }
    *value = (nnode->type == cJSON_True);
    return 0;
}


static int parse_audios(const cJSON *object, const char *string, 
    int *audio_num, loswssp_pkg_data_audio_t **audios)
{
    // audios 为非必填项，使用audio_num或audios指示是否存在此字段
    cJSON *naudios = cjson_get_array(object, string);
    if (naudios == NULL || cJSON_GetArraySize(naudios) == 0) {
        *audio_num = 0;
        *audios = NULL;
    } else {
        int num = cJSON_GetArraySize(naudios);
        loswssp_pkg_data_audio_t *audio_array = 
            (loswssp_pkg_data_audio_t *)malloc(num * sizeof(loswssp_pkg_data_audio_t));
        ASSERT(audio_array);

        *audio_num = num;
        *audios = audio_array;

        // 解析数组内容
        for (int i = 0; i < num; ++i) {
            cJSON *naudios_item = cJSON_GetArrayItem(naudios, i);

            audio_array[i].content = cjson_get_string_value(naudios_item, "content");
            audio_array[i].url = cjson_get_string_value(naudios_item, "url");

            if (audio_array[i].content == NULL || audio_array[i].url == NULL) {
                free(audio_array);
                *audios = NULL;
                return 1;
            }
        }
    }
    return 0;
}

static int parse_files(const cJSON *object, const char *string, 
    int *file_num, loswssp_pkg_data_file_t **files)
{
    // files 为非必填项，使用file_num或files指示是否存在此字段
    cJSON *nfiles = cjson_get_array(object, string);
    if (nfiles == NULL || cJSON_GetArraySize(nfiles) == 0) {
        *file_num = 0;
        *files = NULL;
    } else {
        int num = cJSON_GetArraySize(nfiles);
        loswssp_pkg_data_file_t *file_array = 
            (loswssp_pkg_data_file_t *)malloc(num * sizeof(loswssp_pkg_data_file_t));
        ASSERT(file_array);

        *file_num = num;
        *files = file_array;

        // 解析数组内容
        for (int i = 0; i < num; ++i) {
            cJSON *nfiles_item = cJSON_GetArrayItem(nfiles, i);

            file_array[i].type = cjson_get_string_value(nfiles_item, "type");
            file_array[i].url = cjson_get_string_value(nfiles_item, "url");

            if (file_array[i].type == NULL || file_array[i].url == NULL) {
                free(file_array);
                *files = NULL;
                return 1;
            }
        }
    }
    return 0;
}

static int session_event_handler(const cJSON *nroot, loswssp_package_t *pkg)
{
    const cJSON *ndata = cjson_get_object(nroot, "data");
    if (ndata == NULL) {
        return PARSE_Error;
    }
    if (cjson_get_integer_value(ndata, "session_id", &pkg->session.session_id)) {
        return PARSE_Error;
    }
    if (cjson_get_boolean_value(ndata, "new_session", &pkg->session.new_session)) {
        return PARSE_Error;
    }
    return PARSE_Success;
}

static int message_asr_data_event_handler(const cJSON *nroot, loswssp_package_t *pkg)
{
    const cJSON *ndata = cjson_get_object(nroot, "data");
    if (ndata == NULL) {
        return PARSE_Error;
    }
    if (cjson_get_integer_value(ndata, "message_id", &pkg->message_asr_data.message_id)) {
        return PARSE_Error;
    }
    pkg->message_asr_data.content = cjson_get_string_value(ndata, "content");
    if (pkg->message_asr_data.content == NULL) {
        return PARSE_Error;
    }
    return PARSE_Success;
}

static int message_asr_end_event_handler(const cJSON *nroot, loswssp_package_t *pkg)
{
    const cJSON *ndata = cjson_get_object(nroot, "data");
    if (ndata == NULL) {
        return PARSE_Error;
    }
    if (cjson_get_integer_value(ndata, "message_id", &pkg->message_asr_end.message_id)) {
        return PARSE_Error;
    }
    return PARSE_Success;
}

static int message_delta_event_handler(const cJSON *nroot, loswssp_package_t *pkg)
{
    const cJSON *ndata = cjson_get_object(nroot, "data");
    if (ndata == NULL) {
        return PARSE_Error;
    }
    if (cjson_get_integer_value(ndata, "message_id", &pkg->message_delta.message_id)) {
        return PARSE_Error;
    }
    char *vcontent = cjson_get_string_value(ndata, "content");
    if (vcontent == NULL) {
        return PARSE_Error;
    }
    pkg->message_delta.content = vcontent;
    return PARSE_Success;
}

static int message_audio_event_handler(const cJSON *nroot, loswssp_package_t *pkg)
{
    const cJSON *ndata = cjson_get_object(nroot, "data");
    if (ndata == NULL) {
        return PARSE_Error;
    }
    if (cjson_get_integer_value(ndata, "message_id", &pkg->message_audio.message_id)) {
        return PARSE_Error;
    }
    // audios 为非必填项，使用audio_num或audios指示是否存在此字段
    if (parse_audios(ndata, "audios", 
        &pkg->message_audio.audio_num, &pkg->message_audio.audios)) {
        return PARSE_Error;
    }
    return PARSE_Success;
}

static void message_audio_package_release(loswssp_package_t *pkg)
{
    ASSERT(pkg != NULL);
    if (pkg->message_audio.audios != NULL) {
        free(pkg->message_audio.audios);
        pkg->message_audio.audios = NULL;
    }
}

static int message_file_event_handler(const cJSON *nroot, loswssp_package_t *pkg)
{
    const cJSON *ndata = cjson_get_object(nroot, "data");
    if (ndata == NULL) {
        return PARSE_Error;
    }
    if (cjson_get_integer_value(ndata, "message_id", &pkg->message_file.message_id)) {
        return PARSE_Error;
    }
    // files 为非必填项，使用file_num或files指示是否存在此字段
    if (parse_files(ndata, "files", 
        &pkg->message_file.file_num, &pkg->message_file.files)) {
        return PARSE_Error;
    }
    return PARSE_Success;
}

static void message_file_package_release(loswssp_package_t *pkg)
{
    ASSERT(pkg != NULL);
    if (pkg->message_file.files != NULL) {
        free(pkg->message_file.files);
        pkg->message_file.files = NULL;
    }
}

static int message_end_event_handler(const cJSON *nroot, loswssp_package_t *pkg)
{
    const cJSON *ndata = cjson_get_object(nroot, "data");
    if (ndata == NULL) {
        return PARSE_Error;
    }
    if (cjson_get_integer_value(ndata, "message_id", &pkg->message_end.message_id)) {
        return PARSE_Error;
    }
    // TODO: content虽说是必填项，但是实际收到的Json中没有此属性，先认作content为非必填项
    pkg->message_end.content = cjson_get_string_value(ndata, "content");
    // if (pkg->message_end.content == NULL) {
    //     return PARSE_Error;
    // }
    // cached_prologue_id 为非必填项，使用 has_cached_prologue_id 指示是否存在此字段
    if (cjson_get_integer_value(ndata, "cached_prologue_id", &pkg->message_end.cached_prologue_id)) {
        pkg->message_end.has_cached_prologue_id = false;
    } else {
        pkg->message_end.has_cached_prologue_id = true;
    }
    // audios 为非必填项，使用audio_num或audios指示是否存在此字段
    if (parse_audios(ndata, "audios", 
        &pkg->message_end.audio_num, &pkg->message_end.audios)) {
        return PARSE_Error;
    }
    // files 为非必填项，使用file_num或files指示是否存在此字段
    if (parse_files(ndata, "files", 
        &pkg->message_end.file_num, &pkg->message_end.files)) {
        return PARSE_Error;
    }
    return PARSE_Success;
}

static void message_end_package_release(loswssp_package_t *pkg)
{
    ASSERT(pkg != NULL);
    if (pkg->message_end.audios != NULL) {
        free(pkg->message_end.audios);
        pkg->message_end.audios = NULL;
    }
    if (pkg->message_end.files != NULL) {
        free(pkg->message_end.files);
        pkg->message_end.files = NULL;
    }
}

static int message_stop_event_handler(const cJSON *nroot, loswssp_package_t *pkg)
{
    const cJSON *ndata = cjson_get_object(nroot, "data");
    if (ndata == NULL) {
        return PARSE_Error;
    }
    if (cjson_get_integer_value(ndata, "message_id", &pkg->message_stop.message_id)) {
        return PARSE_Error;
    }
    return PARSE_Success;
}

static int message_point_delta_event_handler(const cJSON *nroot, loswssp_package_t *pkg)
{
    const cJSON *ndata = cjson_get_object(nroot, "data");
    if (ndata == NULL) {
        return PARSE_Error;
    }
    if (cjson_get_integer_value(ndata, "message_id", &pkg->message_point_delta.message_id)) {
        return PARSE_Error;
    }
    if (cjson_get_integer_value(ndata, "points", &pkg->message_point_delta.points)) {
        return PARSE_Error;
    }
    return PARSE_Success;
}

static int pong_event_handler(const cJSON *nroot, loswssp_package_t *pkg)
{
    return PARSE_Success;
}

void loswssp_register_rsp_cbk(g_loswssp_cbk_t loswssp_cbk)
{
    g_loswssp_cbk = loswssp_cbk;
}

struct loswssp_rsp_cb_entry_t {
    const char* event_name;
    int type;
    int (*handler)(const cJSON *nroot, loswssp_package_t *pkg);
    void (*release)(loswssp_package_t *pkg);
};

static struct loswssp_rsp_cb_entry_t g_rsp_cb_table[] = {
    {"session", LR_SESSION, session_event_handler, NULL}, 
    {"message_asr_data", LR_MESSAGE_ASR_DATA, message_asr_data_event_handler, NULL}, 
    {"message_asr_end", LR_MESSAGE_ASR_END, message_asr_end_event_handler, NULL}, 
    {"message_delta", LR_MESSAGE_DELTA, message_delta_event_handler, NULL}, 
    {"message_audio", LR_MESSAGE_AUDIO, message_audio_event_handler, message_audio_package_release}, 
    {"message_file", LR_MESSAGE_FILE, message_file_event_handler, message_file_package_release}, 
    {"message_end", LR_MESSAGE_END, message_end_event_handler, message_end_package_release}, 
    {"message_stop", LR_MESSAGE_STOP, message_stop_event_handler, NULL}, 
    {"message_point_delta", LR_MESSAGE_POINT_DELTA, message_point_delta_event_handler, NULL}, 
    {"pong", LR_PONG, pong_event_handler}, 
};
static int g_rsp_cb_table_size = sizeof(g_rsp_cb_table) / sizeof(g_rsp_cb_table[0]);

void loswssp_rsp_handler(u8 *buf, u32 len)
{
    loswssp_package_t pkg;
    memset(&pkg, 0, sizeof(loswssp_package_t));

    const cJSON *nroot = cjson_parse(buf);
    if (nroot == NULL) {
        goto error_ret;
    }

    const char *vevent = cjson_get_string_value(nroot, "event");
    if (vevent == NULL) {
        goto error_ret;
    }

    struct loswssp_rsp_cb_entry_t *entry = NULL;
    for (int i = 0; i < g_rsp_cb_table_size; ++i) {
        if (strcmp(vevent, g_rsp_cb_table[i].event_name) == 0) {
            entry = &g_rsp_cb_table[i];
            break;
        }
    }
    if (entry == NULL) {
        log_i("Error: can't find event handler: %s", vevent);
        goto error_ret;
    }

    if (entry->handler != NULL) {
        int ret = entry->handler(nroot, &pkg);
        if (ret == PARSE_Success && g_loswssp_cbk != NULL) {
            g_loswssp_cbk(entry->type, &pkg);
        }
    }

    if (entry->release != NULL) {
        entry->release(&pkg);
    }

error_ret:
    if (nroot != NULL) {
        cJSON_Delete(nroot);
        nroot = NULL;
    }
    return;
}

