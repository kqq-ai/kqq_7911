#include "odm_util.h"

/**
 * @brief Constructs the source string for signature verification.
 * 
 * @param method HTTP request method (e.g., GET, POST, PUT, DELETE).
 * @param uri URI path (e.g., /robot/info).
 * @param params Request parameters as a key-value array (key1, value1, key2, value2, ..., NULL).
 * @param headers Headers containing x-app-key, x-device-code, and x-timestamp as key-value array (key1, value1, ..., NULL).
 * @return Constructed source string. Caller must free the returned string.
 */
char* constructSourceString(
    const char* method,
    const char* uri,
    const char* params[],
    const char* headers[])
{
    size_t bufferSize = 1024;
    char* sourceString = (char*)malloc(bufferSize);
    if (!sourceString) return NULL;

    snprintf(sourceString, bufferSize, "%s&uri=%s", method, uri);

    // Append params
    if (params) {
        strcat(sourceString, "&");
        for (int i = 0; params[i] && params[i + 1]; i += 2) {
            if (i > 0) strcat(sourceString, "&");
            strcat(sourceString, params[i]);
            strcat(sourceString, "=");
            strcat(sourceString, params[i + 1]);
        }
    }

    // Append headers
    if (headers) {
        strcat(sourceString, "&");
        for (int i = 0; headers[i] && headers[i + 1]; i += 2) {
            if (i > 0) strcat(sourceString, "&");
            strcat(sourceString, headers[i]);
            strcat(sourceString, "=");
            strcat(sourceString, headers[i + 1]);
        }
    }

    return sourceString;
}

#define HMAX_SHA1_BUF_MAX_SIZE 20
#define BASE64_BUF_MAX_SIZE 29    // ((HMAX_SHA1_BUF_MAX_SIZE + 2) / 3) * 4 = 28

/**
 * @brief Generates the HMAC-SHA1 signature and encodes it in Base64.
 * 
 * @param sourceString The source string to be signed.
 * @param secretKey The secret key for HMAC-SHA1.
 * @return Base64-encoded signature. Caller must free the returned string.
 */
char *generateSignature(const char *sourceString, const char *secretKey) 
{
    size_t write_len = 0;
    unsigned char hmac_result[HMAX_SHA1_BUF_MAX_SIZE];

    const mbedtls_md_info_t *md_info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA1);
    ASSERT(md_info);

    int ret = mbedtls_md_hmac(md_info, secretKey, strlen(secretKey), 
            sourceString, strlen(sourceString), hmac_result);
    ASSERT(ret == 0);

    char *base64_result = malloc(BASE64_BUF_MAX_SIZE);
    ret = mbedtls_base64_encode(base64_result, BASE64_BUF_MAX_SIZE, &write_len, hmac_result, HMAX_SHA1_BUF_MAX_SIZE);
    ASSERT(ret == 0);
    ASSERT(write_len == (BASE64_BUF_MAX_SIZE - 1));
    base64_result[write_len] = '\0';
    return base64_result;
}



