#ifndef __ODM_SECURE_H__
#define __ODM_SECURE_H__
#include <cpu.h>

// #define TEST_LOCOL_MODE
#define SERVER_DOMAIN  "test.coolqq.com"
// #define SERVER_DOMAIN  "coolqq.com"

#define WRITE_SN_KEY 0 // 设置为1以强制写入SN/Key，忽略已有SN

char *get_mqtt_token();
char *get_wss_connection_url();
char *get_wss_test_url();
bool get_SN(char* target);

size_t hex2bytes(const char *hex_str, uint8_t *bytes);
size_t bytes2hex(const uint8_t *bytes, size_t byte_count, char *hex_str, size_t hex_str_size);

#endif // __ODM_SECURE_H__