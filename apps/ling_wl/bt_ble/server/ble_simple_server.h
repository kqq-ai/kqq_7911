#ifndef __BLE_SIMPLE_SERVER_H__
#define __BLE_SIMPLE_SERVER_H__

#include <stdint.h>
#include "../app_wifi_manage.h"
#include "cpu.h"

void server_profile_init(void);
void server_deal_passkey_input(u32 *key);
void server_cbk_sm_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size);
void server_cbk_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size);

void ble_simple_server_disconnect(void);
// void ble_simple_server_adv_enable(u8 en);
void ble_simple_server_module_enable(u8 en);
void ble_simple_server_adv_enable(u8 en);
void ble_simple_server_init(void);
void ble_simple_server_exit(void);

void on_user_key_confirm();

/*
    hzx add
    for 蓝牙配网任务 ： WiFi 获取到ip之后主动上报给 小程序（APP） 
    for 蓝牙配网任务 :  MQTT 获取后台信息确认之后 完成网络连接任务  之后 主动上报给 小程序（APP）
*/
int app_send_user_data_external(u16 handle, u8 *data, u16 len, u8 handle_type);

/* 获取ble与小程序是否断开 */
int get_ble_app_disconnect_flag(void);

/* 通知 APP（小程序） 相关 函数*/
int notify_app_wifi_ip_succ(void *pri);

int notify_app_wifi_mqtt_succ(void *pri);

#endif
