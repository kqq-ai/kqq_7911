#ifndef ODM_UTIL_H
#define ODM_UTIL_H

#include "cpu.h"
#include "mbedtls_2_2_1/mbedtls/md.h"
#include "mbedtls_2_2_1/mbedtls/base64.h"

#include "odmp.h"

#define ODM_HMAC_SHA1_SECRET_KEY "SbmaqJ9sBJlh79O4oJcckbJjmQ43q-SYkFgmrtof"

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
    const char* headers[]);

/**
 * @brief Generates the HMAC-SHA1 signature and encodes it in Base64.
 * 
 * @param sourceString The source string to be signed.
 * @param secretKey The secret key for HMAC-SHA1.
 * @return Base64-encoded signature. Caller must free the returned string.
 */
char *generateSignature(const char *sourceString, const char *secretKey);

#endif // ODM_UTIL_H