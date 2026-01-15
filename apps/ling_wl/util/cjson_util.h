#ifndef __CJSON_UTIL_H__
#define __CJSON_UTIL_H__

#include "cpu.h"
#include "cJSON_common/cJSON.h"

const cJSON *cjson_parse(u8 *buf);
const cJSON *cjson_parse_len(const u8 *buf, u32 len);
cJSON *cjson_get_object(cJSON *object, const char *string);
cJSON *cjson_get_array(cJSON *object, const char *string);
char *cjson_get_string_value(cJSON *object, const char *string);
int cjson_get_integer_value(cJSON *object, const char *string, int *value);
int cjson_get_boolean_value(cJSON *object, const char *string, bool *value);

#endif /*__CJSON_UTIL_H__*/