#ifndef __EEND_BATTERY_LIVE_TEST_H__
#define __EEND_BATTERY_LIVE_TEST_H__

#include "cpu.h"
#include "wss/wss_client.h"

void auto_test_application_init();
void auto_test_application_uninit();
bool auto_test_application_loop();

#endif // __EEND_BATTERY_LIVE_TEST_H__