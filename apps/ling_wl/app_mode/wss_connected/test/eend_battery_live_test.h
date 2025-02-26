#ifndef __EEND_BATTERY_LIVE_TEST_H__
#define __EEND_BATTERY_LIVE_TEST_H__

#include "cpu.h"
#include "wss/wss_client.h"

void test_application_init();
void test_application_uninit();
void test_application_loop(struct websocket_struct *websockets_info);

#endif // __EEND_BATTERY_LIVE_TEST_H__