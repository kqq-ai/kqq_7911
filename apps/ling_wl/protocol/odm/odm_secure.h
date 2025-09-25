#ifndef __ODM_SECURE_H__
#define __ODM_SECURE_H__
#include <cpu.h>


#define TEST_IP_GET "https://kqq.tos-cn-beijing.volces.com/kqq_debug.json"

char *get_mqtt_token();
char *get_wss_connection_url();
char *get_wss_test_url();
bool get_SN(char* target);

size_t hex2bytes(const char *hex_str, uint8_t *bytes);
size_t bytes2hex(const uint8_t *bytes, size_t byte_count, char *hex_str, size_t hex_str_size);

#endif // __ODM_SECURE_H__