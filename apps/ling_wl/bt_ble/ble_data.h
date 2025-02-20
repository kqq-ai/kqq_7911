#ifndef __BLE_DATA_H__
#define __BLE_DATA_H__

#include "cpu.h"
#include "cJSON_common/cJSON.h"
#include "wifi/wifi_connect.h"

#define MAX_PACKET_SIZE 20
#define BUFFER_SIZE 1584  // 假设最大JSON长度为1584字节  : 99 * 16
#define PAYLOAD_SIZE 16  // 每个分包的真实数据大小
#define MAX_PACKETS 99    // 最大分包数

#define MAX_SSID_LEN 33
#define MAX_PWD_LEN 33


void set_current_connected_ssid(char *ssid);

void get_current_connected_ssid(char * ssid);





uint32_t set_wifi_scan_timeout(uint32_t timeout_s);
uint32_t get_wifi_scan_timeout(void * pri);


uint32_t set_wifi_connect_timeout_ms(uint32_t wifi_connect_timeout_ms);
uint32_t get_wifi_connect_timeout_ms(void * pri);
uint32_t set_wifi_connect_start_time_ms(uint32_t wifi_connect_start_time_ms);
uint32_t set_wifi_connect_end_time_ms(uint32_t wifi_connect_end_time);

int get_wifi_scan_ssid_info(struct wifi_scan_ssid_info **ssid_info);

void clear_buffer();
int parse_and_assemble(const uint8_t *data, uint16_t data_length, char *assemble_data);


// 声明处理不同命令的函数
int handle_wifi_scan(const char *cmd, cJSON *args, char * return_json_data);
int handle_wifi_list_ready(const char *cmd, cJSON *args, char * return_json_data);
int handle_wifi_get_list(const char *cmd, cJSON *args, char * return_json_data);
int handle_wifi_stop_scan(const char *cmd, cJSON *args, char * return_json_data);
int handle_wifi_connect(const char *cmd, cJSON *args, char * return_json_data);
int handle_wifi_disconnect(const char *cmd, cJSON *args, char * return_json_data);
int handle_wifi_reconnect(const char *cmd, cJSON *args, char * return_json_data);
int handle_wifi_get_status(const char *cmd, cJSON *args, char * return_json_data);
int handle_wifi_info(const char *cmd, cJSON *args, char * return_json_data);

// 主解析函数
int parse_and_dispatch_json(const char *json_data, char * return_json_data);

#endif