#include "cjson_util.h"


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

// WebSocket payload 不保证以 '\0' 结尾：必须按长度解析
const cJSON *cjson_parse_len(const u8 *buf, u32 len)
{
    if (buf == NULL || len == 0) {
        return NULL;
    }

    char *tmp = (char *)malloc(len + 1);
    if (!tmp) {
        log_i("INFO: cjson_parse_len malloc failed, len=%u", (unsigned)len);
        return NULL;
    }
    memcpy(tmp, buf, len);
    tmp[len] = '\0';

    const cJSON *nroot = cJSON_Parse(tmp);
    if (nroot == NULL) {
        const char *error_ptr = cJSON_GetErrorPtr();
        if (error_ptr != NULL) {
            log_i("INFO: failed to analysis JSON string at position %ld", (long)((const u8 *)error_ptr - (const u8 *)tmp));
        } else {
            log_i("INFO: failed to analysis JSON string, unknown reason");
        }
    }

    free(tmp);
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
