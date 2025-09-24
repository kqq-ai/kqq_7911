#include "odm_secure.h"
#include "app_lingwl.h"

#include "http/http_cli.h"
#include "cJSON_common/cJSON.h"
#include "mbedtls/gcm.h"
#include "time.h"

#include "util/cjson_util.h"
#include "syscfg_id.h"
#include "lfs.h"
#include "app_mode/lwl_app_framework.h"

extern lfs_t g_lfs;

#define BUF_SIZE 128
#define SN_SIZE 15
#define KEY_SIZE 16

char snInVm[SN_SIZE+SN_SIZE]="\0";

char keyInVm[KEY_SIZE];

size_t hex2bytes(const char *hex_str, uint8_t *bytes)
{
    size_t len = strlen(hex_str);
    if (len % 2 != 0 || len > 2*KEY_SIZE) return 0;          // 长度必须是偶数

    size_t byte_count = len / 2;
    for (size_t i = 0; i < byte_count; ++i) {
        char hi = hex_str[i * 2];
        char lo = hex_str[i * 2 + 1];

        // 检查字符合法性
        if (!isxdigit((unsigned char)hi) || !isxdigit((unsigned char)lo))
            return 0;

        // 转换
        uint8_t b = 0;
        if (isdigit((unsigned char)hi)) b = (hi - '0') << 4;
        else                            b = (toupper(hi) - 'A' + 10) << 4;

        if (isdigit((unsigned char)lo)) b |= (lo - '0');
        else                            b |= (toupper(lo) - 'A' + 10);

        bytes[i] = b;
    }
    return byte_count;
}

size_t bytes2hex(const uint8_t *bytes, size_t byte_count, char *hex_str, size_t hex_str_size)
{
    if (!bytes || !hex_str || hex_str_size < (byte_count * 2 + 1)) return 0;

    // 检查指针是否重叠
    const uint8_t *src = bytes;
    uint8_t *tmp_buf = NULL;
    if ((hex_str <= (char*)bytes && hex_str + hex_str_size > (char*)bytes) ||
        ((char*)bytes <= hex_str && (char*)bytes + byte_count > hex_str)) {
        // 有重叠，做临时拷贝
        tmp_buf = malloc(byte_count);
        if (!tmp_buf) return 0;
        memcpy(tmp_buf, bytes, byte_count);
        src = tmp_buf;
    }

    for (size_t i = 0; i < byte_count; ++i) {
        snprintf(hex_str + i * 2, 3, "%02X", src[i]);
    }
    hex_str[byte_count * 2] = '\0';

    if (tmp_buf) free(tmp_buf);
    return byte_count * 2;
}

static void gen_SN()
{
    struct lfs_info info;
    lfs_file_t file;

#if 0
    get_Key_from_Char();
    // 新设备，写入pt 和 keyInVm
    int err = lfs_file_open(&g_lfs, &file, "SN_KEY.bin", 
                LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC);
    /* 3. 写入数据 */
    size_t bytes_written = lfs_file_write(&g_lfs, &file, snInVm, SN_SIZE );
    assert(bytes_written == SN_SIZE);
    bytes_written        = lfs_file_write(&g_lfs, &file, keyInVm, KEY_SIZE);
    assert(bytes_written == KEY_SIZE);
    log_i(" write LittleFS snInVm = %s ,  key len = %d \n", snInVm , bytes_written );
    lfs_file_close(&g_lfs, &file);

#else
    if (lfs_stat(&g_lfs, "SN_KEY.bin", &info) != LFS_ERR_OK) {
        return;
    }
    // assert(lfs_stat(&g_lfs, "SN_KEY.bin", &info) == LFS_ERR_OK);
    // 如果文件存在，get SN from LittleFS
    int ret_lfs = lfs_file_open(&g_lfs, &file, "SN_KEY.bin", LFS_O_RDONLY);
    if (ret_lfs < 0) {
        lfs_remove(&g_lfs, "SN_KEY.bin");// 如果open失败，删除
        log_e("Failed to open log file B: %d", ret_lfs);
    }else{
        lfs_ssize_t bytes_read = lfs_file_read(&g_lfs, &file, &snInVm, SN_SIZE);
        snInVm[SN_SIZE] = '\0';
        bytes_read = lfs_file_read(&g_lfs, &file, &keyInVm, KEY_SIZE);
        log_i(" read LittleFS snInVm = %s ,  key len = %d \n", snInVm , bytes_read ); //读取成功，第二次启动
    }
    lfs_file_close(&g_lfs, &file);
#endif
}

bool get_SN(char* SN){
    if (snInVm[0] == 0) gen_SN();
    if (snInVm[0] == 0 || strlen(snInVm) != SN_SIZE || WRITE_SN_KEY) {
        log_i("get_SN NULL\n");
        snInVm[0] = 0;
        SN[0] = 0;
        return false;
    }
    snprintf(SN, KEY_SIZE, "%s", snInVm);
    log_i("get_SN %s\n", snInVm);
    return true;
}

char *get_mqtt_token() {
    return NULL;
}

// 获取带Token的会话url
char *get_wss_connection_url()
{
    return NULL;
}