#include "app_lfs.h"
#include "system/includes.h"
#include "fs/fs.h"
#include "asm/sfc_norflash_api.h"
#include "lfs.h"
#include "string.h"

#define APP_CONFIG_FILENAME "app_config.json"
#define ROLE_OPENING_FILENAME "role_opening.json"

static lfs_app_config_t g_lfs_app_config = {
    .app_config_key = "lingxi_ai",
    .app_config_version = "1.0.0",
    .app_wifi_config = {
        .ssid = "hello",
        .pwd = "20020719",
        .retry_connect_count = 0,
    },
    .role_openings = {
        {.opening_filename = "opening_name.mp3", .role_name = 1, .role_flag = 0},
    }
};

// variables used by the filesystem
static lfs_t g_lfs;

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

    int err = lfs_mount(&g_lfs, &cfg);
    printf("=======================Mount result: ================================%d\r\n", err);
    if (err)
    {
        // 处理错误...
        // reformat if we can't mount the filesystem
        // this should only happen on the first boot
        err = lfs_format(&g_lfs, &cfg);
        if (err)
        {
            printf("Failed to format filesystem: %d\n", err);
            return NULL;
        }
        if (lfs_mount(&g_lfs, &cfg))
        {
            puts("lfs_mount fail! \r\n");
            return NULL;
        }
    }
    return &g_lfs;
}

/* 程序启动将lfs_cfgfile数据取出来，保存到程序的全局变量 */
void get_flash_app_cfg_data(lfs_t *lfs)
{
    int err = 0;
    int32_t file_size = 0;
    /* 1.检查lfs */
    if (lfs == NULL)
    {
        printf("lfs is NULL\r\n");
        return;
    }

    /* 2.读取lfs文件，获取app_config_data数据 */
    lfs_file_t file;
    err = lfs_file_open(lfs, &file, APP_CONFIG_FILENAME, LFS_O_RDONLY | LFS_O_CREAT);
    if (err)
    {
        printf("lfs_file_open fail! err code : %d\n", err);
        return;
    }

    /* 3.看看文件中是否有内容、有内容就读出数据到全局变量 */
    file_size = lfs_file_size(lfs, &file);
    if (file_size < 0)
    {
        printf("lfs_file_size fail! err code : %d\n", file_size);
        lfs_file_close(lfs, &file);
        return;
    }
    else if (file_size == 0)
    {
        printf("lfs_file_size is 0\r\n");
        lfs_file_close(lfs, &file);
        return;
    }
    else if (file_size > 0)
    {
        char *all_file_data = (char *)malloc(file_size + 1);
        memset(all_file_data, 0, file_size + 1);
        if (all_file_data == NULL)
        {
            printf("malloc fail!\r\n");
            lfs_file_close(lfs, &file);
            return;
        }
        err = lfs_file_read(lfs, &file, all_file_data, file_size);
        if (err != file_size)
        {
            printf("lfs_file_read fail! err code : %d\r\n", err);
            free(all_file_data);
            lfs_file_close(lfs, &file);
            return;
        }
        printf("========================lfs_file_read success!   the data : %s\r\n", all_file_data);
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
            lfs_file_close(lfs, &file);
            return;
        }
        cJSON *app_config_key = cJSON_GetObjectItem(root, "app_config_key");
        if (app_config_key == NULL)
        {
            printf("cJSON_GetObjectItem fail\r\n");
            cJSON_Delete(root);
            free(all_file_data);
            lfs_file_close(lfs, &file);
            return;
        }
        printf("app_config_key = %s\r\n", app_config_key->valuestring);
        cJSON *app_config_version = cJSON_GetObjectItem(root, "app_config_version");
        if (app_config_version == NULL)
        {
            printf("cJSON_GetObjectItem fail\r\n");
            cJSON_Delete(root);
            free(all_file_data);
            lfs_file_close(lfs, &file);
            return;
        }
        printf("app_config_version = %s\r\n", app_config_version->valuestring);
        cJSON *app_wifi_config = cJSON_GetObjectItem(root, "app_wifi_config");
        if (app_wifi_config == NULL)
        {
            printf("cJSON_GetObjectItem fail\r\n");
            cJSON_Delete(root);
            free(all_file_data);
            lfs_file_close(lfs, &file);
            return;
        }
        cJSON *ssid = cJSON_GetObjectItem(app_wifi_config, "ssid");
        if (ssid == NULL)
        {
            printf("cJSON_GetObjectItem fail\r\n");
            cJSON_Delete(root);
            free(all_file_data);
            lfs_file_close(lfs, &file);
            return;
        }
        printf("ssid = %s\r\n", ssid->valuestring);
        cJSON *pwd = cJSON_GetObjectItem(app_wifi_config, "pwd");
        if (pwd == NULL)
        {
            printf("cJSON_GetObjectItem fail\r\n");
            cJSON_Delete(root);
            free(all_file_data);
            lfs_file_close(lfs, &file);
            return;
        }
        printf("pwd = %s\r\n", pwd->valuestring);
        cJSON *retry_connect_count = cJSON_GetObjectItem(app_wifi_config, "retry_connect_count");
        if (retry_connect_count == NULL)
        {
            printf("cJSON_GetObjectItem fail\r\n");
            cJSON_Delete(root);
            free(all_file_data);
            lfs_file_close(lfs, &file);
            return;
        }
        printf("retry_connect_count = %d\r\n", retry_connect_count->valueint);

        cJSON *role_openings = cJSON_GetObjectItem(root, "role_openings");
        if (role_openings == NULL)
        {
            printf("cJSON_GetObjectItem fail\r\n");
            cJSON_Delete(root);
            free(all_file_data);
            lfs_file_close(lfs, &file);
            return;
        }
        int role_count = cJSON_GetArraySize(role_openings);
        if (role_count > MAX_ROLES)
        {
            printf("Too many role_openings, max allowed is 10\r\n");
            cJSON_Delete(root);
            free(all_file_data);
            lfs_file_close(lfs, &file);
            return;
        }
        for (int i = 0; i < role_count; i++)
        {
            cJSON *role_opening = cJSON_GetArrayItem(role_openings, i);
            if (role_opening == NULL)
            {
                printf("cJSON_GetArrayItem fail\r\n");
                cJSON_Delete(root);
                free(all_file_data);
                lfs_file_close(lfs, &file);
                return;
            }
            cJSON *opening_filename = cJSON_GetObjectItem(role_opening, "opening_filename");
            if (opening_filename == NULL)
            {
                printf("cJSON_GetObjectItem fail\r\n");
                cJSON_Delete(root);
                free(all_file_data);
                lfs_file_close(lfs, &file);
                return;
            }
            cJSON *role_name = cJSON_GetObjectItem(role_opening, "role_name");
            if (role_name == NULL)
            {
                printf("cJSON_GetObjectItem fail\r\n");
                cJSON_Delete(root);
                free(all_file_data);
                lfs_file_close(lfs, &file);
                return;
            }
            cJSON *role_flag = cJSON_GetObjectItem(role_opening, "role_flag");
            if (role_flag == NULL)
            {
                printf("cJSON_GetObjectItem fail\r\n");
                cJSON_Delete(root);
                free(all_file_data);
                lfs_file_close(lfs, &file);
                return;
            }
            printf("opening_filename = %s, role_name = %d, role_flag = %d\r\n", opening_filename->valuestring, role_name->valueint, role_flag->valueint);
            memcpy(g_lfs_app_config.role_openings[i].opening_filename, opening_filename->valuestring, strlen(opening_filename->valuestring));
            g_lfs_app_config.role_openings[i].role_name = role_name->valueint;
            g_lfs_app_config.role_openings[i].role_flag = role_flag->valueint;
        }

        // 将这些数据赋值给g_lfs_app_config
        memset(&g_lfs_app_config, 0, sizeof(g_lfs_app_config));
        memcpy(g_lfs_app_config.app_config_key, app_config_key->valuestring, strlen(app_config_key->valuestring));
        memcpy(g_lfs_app_config.app_config_version, app_config_version->valuestring, strlen(app_config_version->valuestring));
        memcpy(g_lfs_app_config.app_wifi_config.ssid, ssid->valuestring, strlen(ssid->valuestring));
        memcpy(g_lfs_app_config.app_wifi_config.pwd, pwd->valuestring, strlen(pwd->valuestring));
        g_lfs_app_config.app_wifi_config.retry_connect_count = retry_connect_count->valueint;

        cJSON_Delete(root);
        free(all_file_data);
        lfs_file_close(lfs, &file);
        printf("get_flash_app_raw_cfg_data success!\r\n");
        return;
    }
}


/* 
    只要修改了lfs_cfgfile数据，调用此函数保存到flash
    修改lfs_cfgfile 就意味着要个更新 cfg_file内容 就意味着要保存到 flash
    文件一定存在    
*/
void save_flash_app_cfg_data(lfs_t *lfs)
{
    /* 0. 检查 lfs */
    if (lfs == NULL)
    {
        printf("lfs is NULL\r\n");
        return;
    }

    /* 1. 创建写入的json数据 */
    cJSON *root = cJSON_CreateObject();
    if (root == NULL)
    {
        printf("cJSON_CreateObject fail\r\n");
        return;
    }

    // 添加简单字段
    cJSON_AddStringToObject(root, "app_config_key", g_lfs_app_config.app_config_key);
    cJSON_AddStringToObject(root, "app_config_version", g_lfs_app_config.app_config_version);

    // 创建嵌套对象
    cJSON *wifi_config = cJSON_CreateObject();
    if (wifi_config == NULL)
    {
        cJSON_Delete(root);
        printf("Failed to create nested object\n");
        return;
    }
    cJSON_AddStringToObject(wifi_config, "ssid", g_lfs_app_config.app_wifi_config.ssid);
    cJSON_AddStringToObject(wifi_config, "pwd", g_lfs_app_config.app_wifi_config.pwd);
    cJSON_AddNumberToObject(wifi_config, "retry_connect_count", g_lfs_app_config.app_wifi_config.retry_connect_count);
    cJSON_AddItemToObject(root, "app_wifi_config", wifi_config);

    // 处理 role_openings 数组
    cJSON *role_openings = cJSON_CreateArray();
    if (role_openings == NULL)
    {
        cJSON_Delete(root);
        printf("Failed to create role_openings array\n");
        return;
    }
    for (int i = 0; i < MAX_ROLES && g_lfs_app_config.role_openings[i].opening_filename[0] != '\0'; i++)
    {
        cJSON *role_opening = cJSON_CreateObject();
        if (role_opening == NULL)
        {
            cJSON_Delete(root);
            cJSON_Delete(role_openings);
            printf("Failed to create role_opening object\n");
            return;
        }
        cJSON_AddStringToObject(role_opening, "opening_filename", g_lfs_app_config.role_openings[i].opening_filename);
        cJSON_AddNumberToObject(role_opening, "role_name", g_lfs_app_config.role_openings[i].role_name);
        cJSON_AddNumberToObject(role_opening, "role_flag", g_lfs_app_config.role_openings[i].role_flag);
        cJSON_AddItemToArray(role_openings, role_opening);
    }
    cJSON_AddItemToObject(root, "role_openings", role_openings);

    // 将 cJSON 对象转换为格式化的字符串
    char *json_string = cJSON_Print(root);
    if (json_string == NULL)
    {
        cJSON_Delete(root);
        printf("Failed to print JSON\n");
        return;
    }

    printf("json_string = %s\r\n", json_string);

    /* 2. 打开文件 */
    lfs_file_t file;
    int err = lfs_file_open(lfs, &file, APP_CONFIG_FILENAME, LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC);
    if (err)
    {
        printf("Failed to open/create file: %d\n", err);
        cJSON_free(json_string);
        cJSON_Delete(root);
        return;
    }

    /* 3. 写入数据 */
    size_t bytes_written = lfs_file_write(lfs, &file, json_string, strlen(json_string));
    if (bytes_written != strlen(json_string))
    {
        printf("lfs_file_write fail! err code : %zu\n", bytes_written);
        lfs_file_close(lfs, &file);
        cJSON_free(json_string);
        cJSON_Delete(root);
        return;
    }

    /* 4. 同步文件 */
    err = lfs_file_sync(lfs, &file);
    if (err)
    {
        printf("Failed to sync file: %d\n", err);
        lfs_file_close(lfs, &file);
        cJSON_free(json_string);
        cJSON_Delete(root);
        return;
    }

    /* 5. 关闭文件 */
    err = lfs_file_close(lfs, &file);
    if (err)
    {
        printf("Failed to close file: %d\n", err);
        cJSON_free(json_string);
        cJSON_Delete(root);
        return;
    }

    /* 6. 清理资源 */
    cJSON_free(json_string);
    cJSON_Delete(root);

    printf("Config file saved successfully.\n");
}

/* 修改ssid pwd */
void change_wifi_config(lfs_t *lfs, char *ssid, char *pwd)
{
    /* 1. 检查 lfs */
    if (lfs == NULL)
    {
        printf("lfs is NULL\r\n");
        return;
    }

    /* 2. 检查 ssid pwd */
    if ((ssid[0] == '\0' ) || (pwd[0] == '\0'))
    {
        printf("ssid or pwd is error !\r\n");
        return;
    }

    // /* 2. 读取当前配置数据 */
    // get_flash_app_raw_cfg_data(lfs, NULL);

    /* 3. 更新 WiFi 配置 */
    memset(g_lfs_app_config.app_wifi_config.ssid, 0, sizeof(g_lfs_app_config.app_wifi_config.ssid));
    memset(g_lfs_app_config.app_wifi_config.pwd, 0, sizeof(g_lfs_app_config.app_wifi_config.pwd));
    memcpy(g_lfs_app_config.app_wifi_config.ssid, ssid, strlen(ssid));
    memcpy(g_lfs_app_config.app_wifi_config.pwd, pwd, strlen(pwd));

    /* 4. 保存更新后的配置数据 */
    save_flash_app_cfg_data(lfs);
}

/* 修改重连次数 */
void change_wifi_retry_connect_count(lfs_t *lfs, int count)
{
    /* 1. 检查 lfs */
    if (lfs == NULL)
    {
        printf("lfs is NULL\r\n");
        return;
    }

    // /* 2. 读取当前配置数据 */
    // get_flash_app_raw_cfg_data(lfs, NULL);

    /* 3. 更新重连次数 */
    g_lfs_app_config.app_wifi_config.retry_connect_count = count;

    /* 4. 保存更新后的配置数据 */
    save_flash_app_cfg_data(lfs);
}

/* 
    更新或录入 角色 （角色名字、角色开场白文件名）
    当role_flag = 1 的时候证明这个位置被使用了
    录入的时候，占用位置之后将role_flag = 1 、默认role_flag = 0
*/
void update_role_opening(lfs_t *lfs, int role_name, char *opening_file_name)
{
    /* 1. 检查 lfs */
    if (lfs == NULL)
    {
        printf("lfs is NULL\r\n");
        return;
    }

    // /* 2. 读取当前配置数据 */
    // get_flash_app_raw_cfg_data(lfs, NULL);

    /* 3. 查找空闲位置或更新现有角色 */
    int found = 0;
    for (int i = 0; i < MAX_ROLES; i++)
    {
        if (g_lfs_app_config.role_openings[i].role_flag == 0 || g_lfs_app_config.role_openings[i].role_name == role_name)
        {
            /* 更新角色信息 */
            memset(g_lfs_app_config.role_openings[i].opening_filename, 0, sizeof(g_lfs_app_config.role_openings[i].opening_filename));
            memcpy(g_lfs_app_config.role_openings[i].opening_filename, opening_file_name, strlen(opening_file_name));
            g_lfs_app_config.role_openings[i].role_name = role_name;
            g_lfs_app_config.role_openings[i].role_flag = 1;
            found = 1;
            break;
        }
    }

    /* 4. 如果没有找到空闲位置，打印错误信息 */
    if (!found)
    {
        printf("No available role opening slot to update or add new role.\r\n");
        return;
    }

    /* 5. 保存更新后的配置数据 */
    save_flash_app_cfg_data(lfs);
}



// /*
//     根据角色名字 来写入对应角色开场白文件的文件内容
//     文件内容为 .mp3的音频数据，在100Kb以内
// */
// void write_role_opening_file(lfs_t *lfs, int role_name);

/* 初始化 存储角色开场白的文件 */
void store_role_opening_file_init(lfs_t *lfs, char *role_open_file_name)
{
    if (role_open_file_name == NULL)
    {
        printf("role_open_file_name is NULL\r\n");
        return;
    }
    // 打开或创建文件
    lfs_file_t file;
    int err = lfs_file_open(lfs, &file, role_open_file_name, LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC);
    if (err)
    {
        printf("Failed to open/create role_opening_54.mp3 file: %d\n", err);
        return;
    }
    printf("role_opening_54.mp3 file created successfully.\n");
}


/* 得到存储的wifi ssid pwd */
void get_wifi_config(char * ssid, char * pwd)
{
    memcpy(ssid, g_lfs_app_config.app_wifi_config.ssid, strlen(g_lfs_app_config.app_wifi_config.ssid));
    memcpy(pwd, g_lfs_app_config.app_wifi_config.pwd, strlen(g_lfs_app_config.app_wifi_config.pwd));
}