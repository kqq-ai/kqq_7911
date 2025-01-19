#ifndef __LWL_APP_CORE_H__
#define __LWL_APP_CORE_H__

#include "cpu.h"
#include "wss/wss_client.h"
#include "audio/recorder.h"

typedef struct {
    char wifi_ssid[33];
    char wifi_passwd[64];
    struct websocket_struct *websockets_info;
    recorder_hdl *recorder;
} lwl_app_context_t;

/**
 * @name app_core_context_get_instance
 * @brief 获取app_context
 * @note 此函数会阻塞当前任务，不可在中断中执行
 */
lwl_app_context_t* lwl_app_core_context_get_instance();

void lwl_app_core_init();
void lwl_app_core_destroy();

#endif