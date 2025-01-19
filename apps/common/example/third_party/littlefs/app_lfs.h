#ifndef _APP_LFS_H_
#define _APP_LFS_H_

#include "lfs.h"
#include "cpu.h"
#include "cJSON_common/cJSON.h"


#define APP_CONFIG_KEY_LEN 32
#define APP_CONFIG_VERSION_LEN 16
#define WIFI_SSID_LEN 32
#define WIFI_PWD_LEN 64
typedef struct
{
    char ssid[WIFI_SSID_LEN];
    char pwd[WIFI_PWD_LEN];
    int retry_connect_count;
} app_wifi_config_t;

typedef struct 
{
    char app_config_key[APP_CONFIG_KEY_LEN];
    char app_config_version[APP_CONFIG_VERSION_LEN];
    app_wifi_config_t app_wifi_config;
}lfs_app_config_t;

lfs_t * app_lfs_init(const char * flash_path);

void change_wifi_config(const char * ssid, const char * pwd);
void change_wifi_retry_connect_count(int count);
void get_flash_app_raw_cfg_data(void * pri);
void app_lfs_cfgfile_init(void *pri);



#endif