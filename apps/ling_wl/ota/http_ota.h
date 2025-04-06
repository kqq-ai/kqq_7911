#ifndef __HTTP_OTA_H__
#define __HTTP_OTA_H__

#include "cJSON_common/cJSON.h"

/**
 * @brief OTA operation error codes
 */
typedef enum {
    OTAERROR_Success = 0,            /**< Operation completed successfully */
    OTAERROR_Null_Pointer = -1,      /**< Null pointer encountered */
    OTAERROR_Malloc_Failed = -2,     /**< Memory allocation failed */
    OTAERROR_OS_Api_Error = -3,      /**< Operating system API error */
    OTAERROR_Net_Error = -4,         /**< Network-related error */
    OTAERROR_Unexpected_Error = -5,  /**< Unexpected error occurred */
    OTAERROR_Parse_Error = -5,       /**< Data parsing error */
} OTAErrorType;

/**
 * @brief Structure containing OTA update information
 */
typedef struct {
    int   _errno;            /**< Error number (0 if success) */
    char *errmsg;            /**< Error message string */
    char *debug;             /**< Debug information string */
 
    char *file_md5;          /**< MD5 checksum of the update file */
    int   file_size;         /**< Size of the update file in bytes */
    char *file_url;          /**< URL to download the update file */
    int   is_force;          /**< Flag indicating forced update (1=true, 0=false) */
    char *package_name;      /**< Name of the update package */
    char *release;           /**< Release notes */
    char *release_date;      /**< Date of the release */
    int   version_code;      /**< Numeric version code */
    char *version_name;      /**< Human-readable version name */
} ota_info_t;

/**
 * @brief Create a download task for OTA update file
 * @param url URL of the file to download
 * @return OTAErrorType error code (OTAERROR_Success on success)
 * @note The download runs asynchronously in a separate task
 */
int http_create_download_task(char *url);

/**
 * @brief Request OTA update information from server
 * @param pota_info Pointer to receive the OTA info JSON string
 * @return OTAErrorType error code (OTAERROR_Success on success)
 * @note The caller is responsible for freeing the returned JSON string
 */
int request_ota_info(char **pota_info);

/**
 * @brief Parse OTA information from JSON response
 * @param nroot Pointer to the root cJSON object
 * @param pota_info Pointer to ota_info_t structure to populate
 * @return OTAErrorType error code (OTAERROR_Success on success)
 * @note This allocates memory for strings in ota_info_t which must be freed
 */
int parse_ota_info(const cJSON *nroot, ota_info_t *pota_info);

#endif