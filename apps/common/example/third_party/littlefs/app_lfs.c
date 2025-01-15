#include "app_lfs.h"
#include "system/includes.h"
#include "fs/fs.h"
#include "asm/sfc_norflash_api.h"
#include "lfs.h"
#include "string.h"

#define APP_CONFIG_FILENAME "app_config.json"

static lfs_app_config_t g_lfs_app_config = {
    .app_config_key = "lingxi_ai",
    .app_config_version = "1.0.0",
    .app_wifi_config = {
        .ssid = "hello",
        .pwd = "20020719",
    },
};

// variables used by the filesystem
static lfs_t lfs;

static int lfs_flash_block_read(const struct lfs_config *c, lfs_block_t block,
                                lfs_off_t off, void *buffer, lfs_size_t size)
{
    /*printf("lfs_read,block=%d,off=%d,size=%d\r\n",block,off,size);*/
    norflash_read(NULL, buffer, size, (u32)c->context + block * c->block_size + off);
    return 0;
}

static int lfs_flash_block_prog(const struct lfs_config *c, lfs_block_t block,
                                lfs_off_t off, const void *buffer, lfs_size_t size)
{
    /*printf("lfs_prog,block=%d,off=%d,size=%d\r\n",block,off,size);*/
    norflash_write(NULL, (void *)buffer, size, (u32)c->context + block * c->block_size + off);
    return 0;
}

static int lfs_flash_block_erase(const struct lfs_config *c, lfs_block_t block)
{
    /*printf("lfs_flash_erase=%d\r\n",block);*/
    norflash_ioctl(NULL, IOCTL_ERASE_SECTOR, (u32)c->context + block * c->block_size);
    return 0;
}
static int lfs_flash_block_sync(const struct lfs_config *c)
{
    return 0;
}

// configuration of the filesystem is provided by this struct

static struct lfs_config cfg = {
    // block device operations
    .read = lfs_flash_block_read,
    .prog = lfs_flash_block_prog,
    .erase = lfs_flash_block_erase,
    .sync = lfs_flash_block_sync,

    // block device configuration
    .read_size = 256,
    .prog_size = 256,
    .block_size = 4096,
    .cache_size = 4096,
    .block_cycles = 1000,

};

lfs_t *app_lfs_init(const char *flash_path)
{

    int ret = 0;
    u32 lfs_addr, lfs_space;
    FILE *profile_fp = fopen(flash_path, "r");
    if (profile_fp == NULL)
    {
        puts("lfs_mount ERROR!!!\r\n");
        return NULL;
    }
    struct vfs_attr file_attr;
    fget_attrs(profile_fp, &file_attr);
    lfs_addr = sdfile_cpu_addr2flash_addr(file_attr.sclust);
    lfs_space = file_attr.fsize;
    fclose(profile_fp);
    printf("LFS_addr = 0x%x, size = 0x%x \r\n", lfs_addr, lfs_space);

    // mount the filesystem
    cfg.context = (void *)lfs_addr;
    cfg.block_count = lfs_space / cfg.block_size;
    cfg.lookahead_size = (cfg.block_count / 8) + (((cfg.block_count / 8) & 7) ? ((8) - ((cfg.block_count / 8) & 7)) : 0);

    int err = lfs_mount(&lfs, &cfg);
    printf("=======================Mount result: ================================%d\r\n", err);
    if (err)
    {
        // 处理错误...
        // reformat if we can't mount the filesystem
        // this should only happen on the first boot
        err = lfs_format(&lfs, &cfg);
        if (err)
        {
            printf("Failed to format filesystem: %d\n", err);
            return NULL;
        }
        if (lfs_mount(&lfs, &cfg))
        {
            puts("lfs_mount fail! \r\n");
            return NULL;
        }
    }
    return &lfs;
}


static void encapsulate_config_app_data(const lfs_app_config_t app_config_data)
{
    printf("encapsulate_config_app_data\r\n");

    // 创建根 JSON 对象
    cJSON *root = cJSON_CreateObject();
    if (root == NULL)
    {
        printf("cJSON_CreateObject fail\r\n");
        return;
    }

    // 添加简单字段
    cJSON_AddStringToObject(root, "app_config_key", app_config_data.app_config_key);
    cJSON_AddStringToObject(root, "app_config_version", app_config_data.app_config_version);

    // 创建嵌套对象
    cJSON *wifi_config = cJSON_CreateObject();
    if (wifi_config == NULL)
    {
        cJSON_Delete(root);
        printf("Failed to create nested object\n");
        return;
    }
    cJSON_AddStringToObject(wifi_config, "ssid", app_config_data.app_wifi_config.ssid);
    cJSON_AddStringToObject(wifi_config, "pwd", app_config_data.app_wifi_config.pwd);
    cJSON_AddItemToObject(root, "app_wifi_config", wifi_config);

    // 将 cJSON 对象转换为格式化的字符串
    char *json_string = cJSON_Print(root);
    if (json_string == NULL)
    {
        cJSON_Delete(root);
        printf("Failed to print JSON\n");
        return;
    }

    printf("json_string = %s\r\n", json_string);

    // 打开或创建文件
    lfs_file_t file;
    int err = lfs_file_open(&lfs, &file, APP_CONFIG_FILENAME, LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC);
    if (err)
    {
        printf("Failed to open/create file: %d\n", err);
        cJSON_free(json_string);
        cJSON_Delete(root);
        return;
    }

    // 写入 JSON 数据
    size_t bytes_written = lfs_file_write(&lfs, &file, json_string, strlen(json_string));
    if (bytes_written != strlen(json_string))
    {
        printf("lfs_file_write fail! err code : %zu\n", bytes_written);
        lfs_file_close(&lfs, &file);
        cJSON_free(json_string);
        cJSON_Delete(root);
        return;
    }

    // 同步文件
    err = lfs_file_sync(&lfs, &file);
    if (err)
    {
        printf("Failed to sync file: %d\n", err);
        lfs_file_close(&lfs, &file);
        cJSON_free(json_string);
        cJSON_Delete(root);
        return;
    }

    // 关闭文件
    err = lfs_file_close(&lfs, &file);
    if (err)
    {
        printf("Failed to close file: %d\n", err);
        cJSON_free(json_string);
        cJSON_Delete(root);
        return;
    }

    // 清理资源
    cJSON_free(json_string);
    cJSON_Delete(root);

    printf("Config file created successfully.\n");
}



void app_lfs_cfgfile_init(void *pri)
{
    lfs_file_t file;
    int ret = 0;
    ret = lfs_file_open(&lfs, &file, APP_CONFIG_FILENAME, LFS_O_RDWR | LFS_O_CREAT);
    if (0 != ret)
    {
        printf("lfs_file_open fail! err code : %d\r\n", ret);
    }
    lfs_file_close(&lfs, &file);
    
    encapsulate_config_app_data(g_lfs_app_config);
    
}


void get_flash_app_raw_cfg_data(void *pri)
{
    int ret = 0;
    lfs_file_t file;
    u32 file_size = 0;

    printf("get_flash_app_raw_cfg_data\r\n");

    ret = lfs_file_open(&lfs, &file, APP_CONFIG_FILENAME, LFS_O_RDONLY | LFS_O_CREAT);
    if (ret)
    {
        printf("lfs_file_open fail! err code : %d\r\n", ret);
        return;
    }
    printf("=============== lfs_file_open success! ================\r\n");
    file_size = lfs_file_size(&lfs, &file);
    if (file_size < 0)
    {
        printf("Failed to get file size: %d\r\n", (int)file_size);
        lfs_file_close(&lfs, &file);
        return;
    }
    printf("file size : %d\r\n", file_size);
    
    char *all_file_data = (char *)malloc(file_size + 1);
    memset(all_file_data, 0, file_size + 1);
    if (all_file_data == NULL)
    {
        printf("malloc fail!\r\n");
        return;
    }
    ret = lfs_file_read(&lfs, &file, all_file_data, file_size);
    if (ret != file_size)
    {
        printf("lfs_file_read fail! err code : %d\r\n", ret);
        free(all_file_data);
        return;
    }
    printf("========================lfs_file_read success!   the data : %s\r\n",all_file_data);
    // 解析JSON数据
    cJSON *root = cJSON_Parse(all_file_data);
    if (root == NULL)
    {
        printf("cJSON_Parse fail\r\n");
        const char *error_ptr = cJSON_GetErrorPtr();
        if (error_ptr != NULL)
        {
            printf("INFO: failed to analysis JSON string at position %ld: %s",
                  ((u8 *)error_ptr - (u8 *)all_file_data), error_ptr);
        }
        else
        {
            log_i("INFO: failed to analysis JSON string, unknown reason");
        }
        free(all_file_data);
        return;
    }
    cJSON *app_config_key = cJSON_GetObjectItem(root, "app_config_key");
    if (app_config_key == NULL)
    {
        printf("cJSON_GetObjectItem fail\r\n");
        cJSON_Delete(root);
        free(all_file_data);
        return;
    }
    printf("app_config_key = %s\r\n", app_config_key->valuestring);
    cJSON *app_config_version = cJSON_GetObjectItem(root, "app_config_version");
    if (app_config_version == NULL)
    {
        printf("cJSON_GetObjectItem fail\r\n");
        cJSON_Delete(root);
        free(all_file_data);
        return;
    }
    printf("app_config_version = %s\r\n", app_config_version->valuestring);
    cJSON *app_wifi_config = cJSON_GetObjectItem(root, "app_wifi_config");
    if (app_wifi_config == NULL)
    {
        printf("cJSON_GetObjectItem fail\r\n");
        cJSON_Delete(root);
        free(all_file_data);
        return;
    }
    cJSON *ssid = cJSON_GetObjectItem(app_wifi_config, "ssid");
    if (ssid == NULL)
    {
        printf("cJSON_GetObjectItem fail\r\n");
        cJSON_Delete(root);
        free(all_file_data);
        return;
    }
    printf("ssid = %s\r\n", ssid->valuestring);
    cJSON *pwd = cJSON_GetObjectItem(app_wifi_config, "pwd");
    if (pwd == NULL)
    {
        printf("cJSON_GetObjectItem fail\r\n");
        cJSON_Delete(root);
        free(all_file_data);
        return;
    }
    printf("pwd = %s\r\n", pwd->valuestring);
    // 将这些数据赋值给g_lfs_app_config
    memset(&g_lfs_app_config, 0, sizeof(g_lfs_app_config));
    memcpy(g_lfs_app_config.app_config_key, app_config_key->valuestring, strlen(app_config_key->valuestring));
    memcpy(g_lfs_app_config.app_config_version, app_config_version->valuestring, strlen(app_config_version->valuestring));
    memcpy(g_lfs_app_config.app_wifi_config.ssid, ssid->valuestring, strlen(ssid->valuestring));
    memcpy(g_lfs_app_config.app_wifi_config.pwd, pwd->valuestring, strlen(pwd->valuestring));
    cJSON_Delete(root);
    free(all_file_data);
    lfs_file_close(&lfs, &file);
    printf("get_flash_app_raw_cfg_data success!\r\n");
    return;
}

void change_wifi_config(const char *ssid, const char *pwd)
{
    get_flash_app_raw_cfg_data(NULL);
    memset(&g_lfs_app_config.app_wifi_config, 0, sizeof(g_lfs_app_config.app_wifi_config));
    memcpy(g_lfs_app_config.app_wifi_config.ssid, ssid, strlen(ssid));
    memcpy(g_lfs_app_config.app_wifi_config.pwd, pwd, strlen(pwd));
    encapsulate_config_app_data(g_lfs_app_config);
    return;
}
