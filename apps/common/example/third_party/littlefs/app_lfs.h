#ifndef _APP_LFS_H_
#define _APP_LFS_H_

#include "lfs.h"
#include "cpu.h"
#include "cJSON_common/cJSON.h"


#define APP_CONFIG_KEY_LEN 32
#define APP_CONFIG_VERSION_LEN 16
#define WIFI_SSID_LEN 32
#define WIFI_PWD_LEN 64

#define MAX_ROLES 10          // 最大角色数量
#define MAX_OPENING_LEN 128   // 开场白文件名字最大长度

typedef struct
{
    char ssid[WIFI_SSID_LEN];
    char pwd[WIFI_PWD_LEN];
    int retry_connect_count;
} app_wifi_config_t;

typedef struct {
    int role_name;
    char opening_filename[MAX_OPENING_LEN];      // 开场白文件名字   /* sprintf(filename, "opening_%d.mp3", name); */
    short role_flag;
} role_opening_t;

typedef struct 
{
    char app_config_key[APP_CONFIG_KEY_LEN];
    char app_config_version[APP_CONFIG_VERSION_LEN];
    app_wifi_config_t app_wifi_config;
    role_opening_t role_openings[MAX_ROLES]
}lfs_app_config_t;



lfs_t * app_lfs_init(const char * flash_path);

/* 程序启动将lfs_cfgfile数据取出来，保存到程序的全局变量 */
void get_flash_app_cfg_data(lfs_t *lfs);

/* 
    只要修改了lfs_cfgfile数据，调用此函数保存到flash
    修改lfs_cfgfile 就意味着要个更新 cfg_file内容 就意味着要保存到 flash
    文件一定存在    
*/
void save_flash_app_cfg_data(lfs_t *lfs);

/* 修改ssid pwd */
void change_wifi_config(lfs_t *lfs, char * ssid, char * pwd);

/* 修改重连次数 */
void change_wifi_retry_connect_count(lfs_t *lfs, int count);

/* 更新或录入 角色 （角色名字、角色开场白文件名）*/
void update_role_opening(lfs_t *lfs, int role_name, char * opening_file_name);

/* 根据角色名字来得到 对应的角色开场白文件名 */

/* 根据角色名字 来写入对应角色开场白文件的文件内容 */
void write_role_opening_file(lfs_t *lfs, int role_name);

/* 初始化 存储角色开场白的文件 */
void store_role_opening_file_init(lfs_t *lfs, char * role_open_file_name);
// /* 下载 存储角色开场白 */
// void store_role_opening(lfs_t lfs, char *role_name, char *opening);



/* 得到存储的wifi ssid pwd */
void get_wifi_config(char * ssid, char * pwd);



#endif