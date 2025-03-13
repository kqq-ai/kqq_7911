#ifndef __HTTP_OTA_H__
#define __HTTP_OTA_H__

#include "cJSON_common/cJSON.h"

typedef enum {
    OTAERROR_Success = 0,
    OTAERROR_Null_Pointer = -1,
    OTAERROR_Malloc_Failed = -2,
    OTAERROR_OS_Api_Error = -3,
    OTAERROR_Net_Error = -4,
    OTAERROR_Unexpected_Error = -5,
    OTAERROR_Parse_Error = -5,
} OTAErrorType;

typedef struct {
    int   _errno;
    char *errmsg;
    char *debug;
 
    char *file_md5;
    int   file_size;
    char *file_url;
    int   is_force;
    char *package_name;
    char *release;
    char *release_date;
    int   version_code;
    char *version_name;
} ota_info_t;

int http_create_download_task(char *url);

int request_ota_info(char **pota_info);

int parse_ota_info(const cJSON *nroot, ota_info_t *pota_info);

#endif