#ifndef __ODM_SECURE_H__
#define __ODM_SECURE_H__


// #define TEST_LOCOL_MODE
// #define SERVER_DOMAIN  "test.coolqq.com"
#define SERVER_DOMAIN  "coolqq.com"

char *get_wss_connection_url();
char *get_wss_test_url();
void get_SN(char* target);

#endif // __ODM_SECURE_H__