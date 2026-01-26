#include "mode_list.h"

#include "../lwl_app_framework.h"
#include "log.h"
#include "app_cookie.h"
#include "../../bt_ble/server/ble_simple_server.h"
#include "../../bt_ble/client/ble_simple_client.h"
#include "../../include/app_config.h"
#include "../../protocol/cp/c_uart_p.h"
#include "../../protocol/odm/odm_secure.h"

#include "../../bt_ble/client/ble_distance.h"
#include <sys_time.h>
#include "key_event.h"
#include "cJSON_common/cJSON.h"
#include "bt_ble/app_wifi_manage.h"
#include "http/http_cli.h"
#include <stdlib.h> // For rand() and srand()

// 为模式8 SN/Key写入功能添加必要的头文件
#include "../../../common/example/third_party/littlefs/lfs.h"  // LittleFS文件系统

#include "../../LED/LED.h"

static void set_led_red(void)        { set_rgb_color(COLOR_RED); }
static void set_led_green(void)      { set_rgb_color(COLOR_GREEN); }
static void set_led_blue(void)       { set_rgb_color(COLOR_BLUE); }

// 外部声明全局LittleFS实例
extern lfs_t g_lfs;


// 模式8功能声明
static void mode8_reset_network_check(void);
static int mode8_write_sn_key_to_storage(const char* sn, const char* key); // 新增模式8写入SN/Key函数声明
static int mode8_verify_written_sn_key(const char* expected_sn, const char* expected_key); // 新增模式8验证SN/Key函数声明
static int mode8_write_and_verify_sn_key(const char* sn, const char* key); // 新增模式8写入并验证SN/Key综合函数声明
static void mode8_wrsn_audio_callback(void *priv);

// 模式8网络检查状态变量
static u8 g_mode8_network_check_done = 0;
static u8 g_mode8_sn_key_obtained = 0;     // SN和Key是否已获取成功
static u8 g_mode8_first_touch_handled = 0; // 第一次触摸按键是否已处理
// 模式8写入请求延迟到任务上下文执行的标志与运行中标志
static u8 g_mode8_write_pending = 0;
static u8 g_mode8_op_running = 0;

// 全局LittleFS访问读写锁（保护所有lfs_*并发访问）
#include "../../util/rwlock.h"
static rwlock_t g_lfs_rwlock;

// 模式8存储从服务器获取的SN和Key
#define MODE8_SN_SIZE 32
#define MODE8_KEY_SIZE 64
static char g_mode8_obtained_sn[MODE8_SN_SIZE] = {0};   // 从服务器获取的SN
static char g_mode8_obtained_key[MODE8_KEY_SIZE] = {0}; // 从服务器获取的Key

// 模式8重置函数
static void mode8_reset_network_check(void)
{
    g_mode8_network_check_done = 0;
    g_mode8_sn_key_obtained = 0;
    g_mode8_first_touch_handled = 0;
    g_mode8_write_pending = 0;
    g_mode8_op_running = 0;
    // 清空存储的SN和Key
    memset(g_mode8_obtained_sn, 0, MODE8_SN_SIZE);
    memset(g_mode8_obtained_key, 0, MODE8_KEY_SIZE);
    log_i("MODE 8: Network check state reset");
}

// 新增模式8处理函数
void process_mode_8(void)
{
    // 如果提示音还在播放中，则等待播放完成
    if (g_audio_playing) {
        // 等待音频播放完成，无需特殊处理
        return;
    }

    // 如果有挂起的写入请求，且当前未在执行写入，则在任务上下文执行
    if (g_mode8_write_pending && !g_mode8_op_running && g_mode8_sn_key_obtained) {
        g_mode8_op_running = 1;
        log_i("MODE 8: Pending SN/Key write detected, executing in task context");
        int result = mode8_write_and_verify_sn_key(g_mode8_obtained_sn, g_mode8_obtained_key);
        
        log_i("MODE 8: Write+Verify completed with result=%d", result);
        g_mode8_write_pending = 0;
        g_mode8_op_running = 0;
        g_mode8_sn_key_obtained = 0;

        if (result == 0) {
            log_i("MODE 8: SN/Key write and verification succeeded");
            // 播放test_sn_s.mp3
            lwl_app_context_t *context = lwl_app_core_context_get_instance();
            if (context && context->music_player) {
                g_audio_playing = 1;
                lwl_app_audio_play(CONFIG_CP_VOICE_FILE_PATH"test_sn_s.mp3", mode8_wrsn_audio_callback);
                log_i("MODE 8: test_sn_s.mp3 started successfully");
            }
        } else {
            log_e("MODE 8: SN/Key write and/or verification failed");
            lwl_app_context_t *context7_failed = lwl_app_core_context_get_instance();
            g_audio_playing = 1;  // 设置音频播放标志
            lwl_app_audio_play(CONFIG_CP_VOICE_FILE_PATH"test_sn_f.mp3", mode8_wrsn_audio_callback);
            log_i("MODE 8: test_sn_f.mp3 started successfully");
        }


    }

    // 提示音播放完成后，SN请求将在audio callback中执行
    // 其余无需额外处理
}


static void process_mode_8_touch_key(u8 touch_key_event_type)
{
    // 特殊处理模式8的触摸按键检测
    if (g_mode8_sn_key_obtained && !g_mode8_first_touch_handled) {
        if (touch_key_event_type == 0) {
            // 第一次触摸按键按下事件，且SN/Key已获取成功
            log_i("MODE 8: First touch key detected after SN/Key obtained");
            
            //=============================================================================================================================================================================================
            // 添加刷写SN和Key的代码
            log_i("MODE 8: ========== STARTING SN/KEY WRITE PROCESS ==========");
            log_i("MODE 8: SN to write: %s", g_mode8_obtained_sn);
            log_i("MODE 8: Key to write: %s", g_mode8_obtained_key);
            log_i("MODE 8: ==================================================================");
            // 触发写入动作：改为设置挂起标志，避免在事件回调/模式切换上下文直接进行LFS操作
            g_mode8_write_pending = 1;
            
            // 标记第一次触摸已处理
            g_mode8_first_touch_handled = 1;
            log_i("MODE 8: First touch handling completed");
        }
    }
}

// 模式8执行SN请求函数
static void mode8_execute_sn_request(void)
{
    // 只执行一次网络检查
    if (g_mode8_network_check_done) {
        return; // 网络检查已完成，无需重复执行
    }
    
    g_mode8_network_check_done = 1;

    log_i("================================1Entering mode 8================================");
    log_i("================================2Entering mode 8================================");

    // Check WIFI connection status

    /* 网络连接成功处理 */
    WIFI_connect_STATUES wifi_status = get_wifi_connect_status(NULL);
    enum wifi_sta_connect_state state = wifi_get_sta_connect_state();
    log_i("==============================[MODE8] wifi_status: %d %d================================", state, wifi_status);
    if (wifi_status != WIFI_CONNECT_SUCC && wifi_status != WIFI_CONNECT_DHCP_SUCC &&
        state != WIFI_STA_NETWORK_STACK_DHCP_SUCC && state != WIFI_STA_CONNECT_SUCC) {
        log_i("===============:WIFI disconnected===============");
        log_i("[MODE8] WIFI not connected, cannot request SN, process terminated");
        return;
    }
    log_i("========================================== [MODE8] WIFI connected================================");

    // Build POST request to https://test.coolqq.com/api/sn
    //char *url是合并 "https://" 和 SERVER_DOMAIN 和"/api/sn"
    char url[100];
    snprintf(url, sizeof(url), "https://%s/api/sn", SERVER_DOMAIN);
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "C");
    char *post_data = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    log_i("==========================================[MODE8]POST:%s %s================================", url, post_data);

    http_body_obj http_body_buf;
    httpcli_ctx ctx;
    memset(&ctx, 0, sizeof(ctx));
    memset(&http_body_buf, 0, sizeof(http_body_buf));

    http_body_buf.recv_len = 0;
    http_body_buf.buf_len = 4096;
    http_body_buf.buf_count = 1;
    http_body_buf.p = (char *)malloc(http_body_buf.buf_len * http_body_buf.buf_count);
    if (!http_body_buf.p) {
        log_i("===========================================");
        log_i("[MODE8] Memory allocation failed, process terminated");
        free(post_data);
        return;
    }

    ctx.url = url;
    ctx.timeout_millsec = 5000;
    ctx.priv = &http_body_buf;
    ctx.connection = "close";
    ctx.data_format = "application/json"; // Set Content-Type
    ctx.post_data = post_data;
    ctx.data_len = strlen(post_data);

    log_i("========================Sending httpcli_post...=============");
    int ret = httpcli_post(&ctx);
    log_i("=======================[MODE8] httpcli_post return: %d, length: %d===========", ret, http_body_buf.recv_len);
    free(post_data);

    if (ret != HERROR_OK || http_body_buf.recv_len <= 0) {
        log_i("==========================================MODE 8: HTTP request failed================================");
        log_i("==========================================[MODE8] HTTP request failed, process terminated================================");
        if (http_body_buf.p) free(http_body_buf.p);
        return;
    }

    log_i("==========================================[MODE8] HTTP response: %s================================", http_body_buf.p);
    // Parse JSON response
    const cJSON *resp = cJSON_Parse(http_body_buf.p);
    if (!resp) {
        log_i("==========================================MODE 8: Response is not valid JSON================================");
        log_i("==========================================[MODE8] Response is not valid JSON, process terminated================================");
        free(http_body_buf.p);
        return;
    }
    const cJSON *data = cJSON_GetObjectItem(resp, "data");
    const cJSON *sn = data ? cJSON_GetObjectItem(data, "sn") : NULL;
    const cJSON *key = data ? cJSON_GetObjectItem(data, "key") : NULL;
    if (sn && key && cJSON_IsString(sn) && cJSON_IsString(key)) {
        log_i("===============================[MODE8] SN: %s=======================", sn->valuestring);
        log_i("===============================[MODE8] Key: %s=======================", key->valuestring);
        
        // 保存获取到的SN和Key到全局变量
        strncpy(g_mode8_obtained_sn, sn->valuestring, MODE8_SN_SIZE - 1);
        g_mode8_obtained_sn[MODE8_SN_SIZE - 1] = '\0';  // 确保字符串终止
        strncpy(g_mode8_obtained_key, key->valuestring, MODE8_KEY_SIZE - 1);
        g_mode8_obtained_key[MODE8_KEY_SIZE - 1] = '\0';  // 确保字符串终止
        
        log_i("===============================[MODE8] SN and Key saved to global variables=======================");
        
        set_led_red();
        log_i("========================[MODE8] SN/Key obtained successfully, red LED is ON=================");
        
        // 设置SN和Key获取成功标志
        g_mode8_sn_key_obtained = 1;
        
        g_audio_playing = 0;
        g_mode8_op_running = 0;
        g_mode8_write_pending = 1;//TODO using touch key to trigger write
        
        // 播放获取SN成功的音频
        // lwl_app_context_t *context7_success = lwl_app_core_context_get_instance();
        // g_audio_playing = 1;  // 设置音频播放标志
        // lwl_app_audio_play(CONFIG_CP_VOICE_FILE_PATH"test_getsn.mp3", mode8_success_audio_callback);
        // g_mode8_op_running = 1;
        // log_i("MODE 8: test_getsn.mp3 started successfully");
    } else {
        log_i("===============================[MODE8] Failed to get SN/Key, process terminated==========");
        log_i("=============================MODE 8: SN/Key fetch failed================================");
    }
    cJSON_Delete(resp);
    free(http_body_buf.p);
}
// 模式8音频播放回调函数
void mode8_audio_callback(void *priv)
{
    // 检查当前模式，如果不是模式8，则忽略回调
    if (get_current_mode() != WORK_MODE_8) {
        log_i("MODE 8: Audio callback ignored - not in mode 8 (current mode: %d)", get_current_mode());
        return;
    }
    
    log_i("MODE 8: test_snkey.mp3 playback completed");
    g_audio_playing = 0;
    
    // 模式8音频播放完成后，开始SN号获取流程
    log_i("MODE 8: Audio playback completed, starting SN key retrieval process");

    char sn[SN_SIZE+1];
    if (!get_SN(sn)) {
        mode8_execute_sn_request();
    } else {
        log_i("MODE 8: device has SN: %s, skipping SN request", sn);
        lwl_app_context_t *context7_failed = lwl_app_core_context_get_instance();
        g_audio_playing = 1;  // 设置音频播放标志
        lwl_app_audio_play(CONFIG_CP_VOICE_FILE_PATH"test_sn_f.mp3", mode8_wrsn_audio_callback);
        log_i("MODE 8: test_sn_f.mp3 started successfully");
    }
}

// 模式8成功音频播放回调函数
static void mode8_success_audio_callback(void *priv)
{
    // 检查当前模式，如果不是模式8，则忽略回调
    if (get_current_mode() != WORK_MODE_8) {
        log_i("MODE 8: Success audio callback ignored - not in mode 8 (current mode: %d)", get_current_mode());
        return;
    }
    
    log_i("MODE 8: test_getsn.mp3 playback completed");
    g_audio_playing = 0;
    g_mode8_op_running = 0;
    g_mode8_write_pending = 1;//TODO using touch key to trigger write
    
    // SN获取成功音频播放完成后的处理
    log_i("MODE 8: SN retrieval success audio completed");
}

// 模式8 test_getsn.mp3 音频播放回调函数
static void mode8_wrsn_audio_callback(void *priv)
{
    // 检查当前模式，如果不是模式8，则忽略回调
    if (get_current_mode() != WORK_MODE_8) {
        log_i("MODE 8: WRSN audio callback ignored - not in mode 8 (current mode: %d)", get_current_mode());
        return;
    }
    
    log_i("MODE 8: test_sn_s.mp3 playback completed");
    g_audio_playing = 0;
    lwl_app_mode_switch_to(APP_POWER_OFF_MODE);
    
    log_i("MODE 8: WRSN audio playback finished");
}

//=================================================================================================
// 模式8 SN/Key写入功能实现
//=================================================================================================

#define KEY_SIZE 16


/**
 * @brief 将SN和Key写入存储
 * @param sn 序列号字符串
 * @param key 密钥字符串 
 * @return 0成功, -1失败
 */
static int mode8_write_sn_key_to_otp(const char* sn, const char* key)
{
    if (!sn || !key) {
        log_e("MODE 8: Invalid SN or Key pointer");
        return -1;
    }
    
    if (strlen(sn) == 0 || strlen(key) == 0) {
        log_e("MODE 8: Empty SN or Key string");
        return -2;
    }
    
    log_i("MODE 8: Writing SN/Key to OTP...");
    log_i("MODE 8: SN: %s (length: %d)", sn, strlen(sn));
    log_i("MODE 8: Key: %s (length: %d)", key, strlen(key));

    
    uint8_t keyInByte[KEY_SIZE];
    int key_len = hex2bytes(key, keyInByte);
    if (key_len != KEY_SIZE) {
        log_e("MODE 8: Failed to exchange Key data, len = %d, expected: %d", key_len, KEY_SIZE);
        return -7;
    }
    
    norflash_eraser_otp();  //强制擦除otp，包括SN和Wifi
    
    //这里仅写入操作，先组合sn和key，一次性写入，跳过前8字节
    u8 otp_buf[8 + SN_SIZE + KEY_SIZE + 1 + 1] = {0};
    memcpy(otp_buf + 8, sn, SN_SIZE + 1);
    memcpy(otp_buf + 8 + SN_SIZE + 1, keyInByte, KEY_SIZE);
    otp_buf[8 + SN_SIZE + 1 + KEY_SIZE] = '\0'; // Key结束符
    int ret_otp = norflash_write_otp(otp_buf, 8 + SN_SIZE + KEY_SIZE + 1 + 1, 8 + SN_SIZE + KEY_SIZE + 1 + 1);//写，读写最大字节：768
    if (ret_otp != 0) {
        log_e("MODE 8: Failed to write SN/Key to OTP, error: %d", ret_otp);
        return -8;
    }
    
    log_i("MODE 8: ========== SN/KEY WRITE SUCCESS ==========");
    log_i("MODE 8: OTP Written SN:  %s (%d bytes)", sn, strlen(sn));
    log_i("MODE 8: ==========================================");

    log_i("MODE 8: SN/Key verification passed");
    return 0;  // 成功
}

/**
 * @brief 验证写入的SN和Key是否正确
 * @param expected_sn 期望的SN
 * @param expected_key 期望的Key
 * @return 0成功, -1失败
 */
static int mode8_verify_written_sn_key_from_otp(const char* expected_sn, const char* expected_key)
{
    if (!expected_sn || !expected_key) {
        log_e("MODE 8: Invalid expected SN or Key pointer");
        return -1;
    }
    
    log_i("MODE 8: Verifying written SN/Key...");
    //仅读取OTP，进行对比
    u8 otp_buf[8 + SN_SIZE + KEY_SIZE + 1 + 1] = {0};
    norflash_read_otp(otp_buf, 8 + SN_SIZE + KEY_SIZE + 1 + 1, 8 + SN_SIZE + KEY_SIZE + 1 + 1);
    if (strncmp((char *)(otp_buf + 8), expected_sn, SN_SIZE) != 0) {
        log_e("MODE 8: OTP SN data mismatch");
        log_e("MODE 8: Expected: %s", expected_sn);
        log_e("MODE 8: Read:     %s", otp_buf + 8);
        return -12;
    }else {
        log_i("MODE 8: OTP SN data match~~\n");
    }

    char keyInOTP[MODE8_KEY_SIZE] = {0};
    int read_key_len = KEY_SIZE;

    read_key_len = bytes2hex(otp_buf + 8 + SN_SIZE + 1, read_key_len, keyInOTP, sizeof(keyInOTP));
    
    if (strncasecmp(keyInOTP, expected_key, read_key_len) != 0) {
        log_e("MODE 8: Key data mismatch");
        log_e("MODE 8: Expected: %s", expected_key);
        log_e("MODE 8: Read:     %.*s", read_key_len, keyInOTP);
        return -13;
    }

    //log otp_buf完整内容
    log_i("MODE 8:VERIFICATION OTP content:");
    for (int i = 0; i < 8 + SN_SIZE + KEY_SIZE + 1 + 1; i++) {
        log_i("0x%02X ", otp_buf[i]);
    }
    log_i("\n");

    log_i("MODE 8: ========== VERIFICATION SUCCESS ==========");
    log_i("MODE 8: SN verification:  PASSED (%s)", expected_sn);
    log_i("MODE 8: Key verification: PASSED (%s)", expected_key);
    log_i("MODE 8: ========================================");
    
    return 0;  // 验证成功
}

/**
 * @brief 将SN和Key写入存储
 * @param sn 序列号字符串
 * @param key 密钥字符串 
 * @return 0成功, -1失败
 */
static int mode8_write_sn_key_to_lfs(const char* sn, const char* key)
{
    if (!sn || !key) {
        log_e("MODE 8: Invalid SN or Key pointer");
        return -1;
    }
    
    if (strlen(sn) == 0 || strlen(key) == 0) {
        log_e("MODE 8: Empty SN or Key string");
        return -2;
    }
    
    log_i("MODE 8: Writing SN/Key to storage...");
    log_i("MODE 8: SN: %s (length: %d)", sn, strlen(sn));
    log_i("MODE 8: Key: %s (length: %d)", key, strlen(key));
    
    // 真正的LittleFS写入实现
    struct lfs_info info;
    lfs_file_t file;
    int ret;

    // 写操作加写锁
    rwlock_acquire_write(&g_lfs_rwlock);
    
    // 先删除旧文件（如果存在）
    ret = lfs_stat(&g_lfs, "SN_KEY.bin", &info);
    if (ret == LFS_ERR_OK) {
        lfs_remove(&g_lfs, "SN_KEY.bin");
        log_i("MODE 8: Removed existing SN/Key file");
    }
    
    // 创建新文件
    ret = lfs_file_open(&g_lfs, &file, "SN_KEY.bin", 
                        LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC);
    if (ret < 0) {
        log_e("MODE 8: Failed to create SN/Key file, error: %d", ret);
        rwlock_release_write(&g_lfs_rwlock);
        return -3;
    }
    
    // 写入数据格式：
    // [4 bytes] SN长度
    // [SN data] SN字符串
    // [4 bytes] Key长度 
    // [Key data] Key字符串
    
    uint32_t sn_len = strlen(sn);
    uint32_t key_len = strlen(key);

    lfs_ssize_t bytes_written = 0;
    
    // 写入SN长度
    // lfs_ssize_t bytes_written = lfs_file_write(&g_lfs, &file, &sn_len, sizeof(sn_len));
    // if (bytes_written != sizeof(sn_len)) {
    //     log_e("MODE 8: Failed to write SN length, written: %d", bytes_written);
    //     lfs_file_close(&g_lfs, &file);
    //     rwlock_release_write(&g_lfs_rwlock);
    //     return -4;
    // }
    
    // 写入SN数据
    bytes_written = lfs_file_write(&g_lfs, &file, sn, sn_len);
    if (bytes_written != sn_len) {
        log_e("MODE 8: Failed to write SN data, written: %d, expected: %d", bytes_written, sn_len);
        lfs_file_close(&g_lfs, &file);
        rwlock_release_write(&g_lfs_rwlock);
        return -5;
    }
    
    // 写入Key长度
    // bytes_written = lfs_file_write(&g_lfs, &file, &key_len, sizeof(key_len));
    // if (bytes_written != sizeof(key_len)) {
    //     log_e("MODE 8: Failed to write Key length, written: %d", bytes_written);
    //     lfs_file_close(&g_lfs, &file);
    //     rwlock_release_write(&g_lfs_rwlock);
    //     return -6;
    // }
    uint8_t keyInVm[KEY_SIZE];
    key_len = hex2bytes(key, keyInVm);
    if (key_len != KEY_SIZE) {
        log_e("MODE 8: Failed to exchange Key data, len = %d, expected: %d", key_len, KEY_SIZE);
        lfs_file_close(&g_lfs, &file);
        rwlock_release_write(&g_lfs_rwlock);
        return -7;
    }
    
    // 写入Key数据
    bytes_written = lfs_file_write(&g_lfs, &file, keyInVm, key_len);
    if (bytes_written != key_len) {
        log_e("MODE 8: Failed to write Key data, written: %d, expected: %d", bytes_written, key_len);
        lfs_file_close(&g_lfs, &file);
        rwlock_release_write(&g_lfs_rwlock);
        return -7;
    }
    
    // 关闭文件
    lfs_file_close(&g_lfs, &file);
    // 释放写锁（验证函数内部再加读锁）
    rwlock_release_write(&g_lfs_rwlock);
    
    log_i("MODE 8: ========== SN/KEY WRITE SUCCESS ==========");
    log_i("MODE 8: Written SN:  %s (%d bytes)", sn, sn_len);
    log_i("MODE 8: Written Key: %s (%d bytes)", key, key_len);
    log_i("MODE 8: File: SN_KEY.bin");
    log_i("MODE 8: ==========================================");

    log_i("MODE 8: SN/Key verification passed");
    return 0;  // 成功
}

/**
 * @brief 验证写入的SN和Key是否正确
 * @param expected_sn 期望的SN
 * @param expected_key 期望的Key
 * @return 0成功, -1失败
 */
static int mode8_verify_written_sn_key_from_lfs(const char* expected_sn, const char* expected_key)
{
    if (!expected_sn || !expected_key) {
        log_e("MODE 8: Invalid expected SN or Key pointer");
        return -1;
    }
    
    log_i("MODE 8: Verifying written SN/Key...");
    
    struct lfs_info info;
    lfs_file_t file;
    int ret;

    // 读操作加读锁
    rwlock_acquire_read(&g_lfs_rwlock);
    
    // 检查文件是否存在
    ret = lfs_stat(&g_lfs, "SN_KEY.bin", &info);
    if (ret != LFS_ERR_OK) {
        log_e("MODE 8: SN/Key file does not exist, error: %d", ret);
        rwlock_release_read(&g_lfs_rwlock);
        return -2;
    }
    
    // 打开文件进行读取
    ret = lfs_file_open(&g_lfs, &file, "SN_KEY.bin", LFS_O_RDONLY);
    if (ret < 0) {
        log_e("MODE 8: Failed to open SN/Key file for verification, error: %d", ret);
        rwlock_release_read(&g_lfs_rwlock);
        return -3;
    }
    
    // 读取并验证SN
    uint32_t read_sn_len = SN_SIZE;
    
    char read_sn[MODE8_SN_SIZE] = {0};
    lfs_ssize_t bytes_read = lfs_file_read(&g_lfs, &file, read_sn, read_sn_len);
    if (bytes_read != read_sn_len) {
        log_e("MODE 8: Failed to read SN data, read: %d, expected: %d", bytes_read, read_sn_len);
        lfs_file_close(&g_lfs, &file);
        rwlock_release_read(&g_lfs_rwlock);
        return -6;
    }
    
    if (strncmp(read_sn, expected_sn, read_sn_len) != 0) {
        log_e("MODE 8: SN data mismatch");
        log_e("MODE 8: Expected: %s", expected_sn);
        log_e("MODE 8: Read:     %.*s", read_sn_len, read_sn);
        lfs_file_close(&g_lfs, &file);
        rwlock_release_read(&g_lfs_rwlock);
        return -7;
    }
    
    // 读取并验证Key
    uint32_t read_key_len = KEY_SIZE;
    
    char read_key[MODE8_KEY_SIZE] = {0};
    bytes_read = lfs_file_read(&g_lfs, &file, read_key, read_key_len);
    if (bytes_read != read_key_len) {
        log_e("MODE 8: Failed to read Key data, read: %d, expected: %d", bytes_read, read_key_len);
        lfs_file_close(&g_lfs, &file);
        rwlock_release_read(&g_lfs_rwlock);
        return -10;
    }

    read_key_len = bytes2hex(read_key, read_key_len, read_key, sizeof(read_key));
    
    if (strncasecmp(read_key, expected_key, read_key_len) != 0) {
        log_e("MODE 8: Key data mismatch");
        log_e("MODE 8: Expected: %s", expected_key);
        log_e("MODE 8: Read:     %.*s", read_key_len, read_key);
        lfs_file_close(&g_lfs, &file);
        rwlock_release_read(&g_lfs_rwlock);
        return -11;
    }
    
    // 关闭文件
    lfs_file_close(&g_lfs, &file);
    // 释放读锁
    rwlock_release_read(&g_lfs_rwlock);

    log_i("MODE 8: ========== VERIFICATION SUCCESS ==========");
    log_i("MODE 8: SN verification:  PASSED (%s)", expected_sn);
    log_i("MODE 8: Key verification: PASSED (%s)", expected_key);
    log_i("MODE 8: File size: %d bytes", info.size);
    log_i("MODE 8: ========================================");
    
    return 0;  // 验证成功
}

/**
 * @brief 写入并验证SN和Key的综合函数
 * @param sn 序列号字符串
 * @param key 密钥字符串
 * @return 0成功, -1失败
 */
static int mode8_write_and_verify_sn_key(const char* sn, const char* key)
{
    if (!sn || !key) {
        log_e("MODE 8: Invalid SN or Key pointer");
        set_led_red();  // 参数错误，显示红灯
        return -1;
    }
    
    if (strlen(sn) == 0 || strlen(key) == 0) {
        log_e("MODE 8: Empty SN or Key string");
        set_led_red();  // 参数错误，显示红灯
        return -2;
    }
    
    log_i("MODE 8: Starting combined write and verify operation...");
    
    // 第一步：执行写入操作
#ifdef TEST_SERVER
    int write_result = mode8_write_sn_key_to_lfs(sn, key);
#else
    int write_result = mode8_write_sn_key_to_otp(sn, key);
#endif
    if (write_result != 0) {
        log_e("MODE 8: ========== SN/KEY WRITE FAILED ==========");
        log_e("MODE 8: Failed to write SN/Key to storage, error code: %d", write_result);
        set_led_red();    // 写入失败，显示红灯
        return write_result;
    }
    
    log_i("MODE 8: ========== SN/KEY WRITE SUCCESS ==========");
    log_i("MODE 8: SN/Key written to storage successfully!");
    
    // 验证写入的数据
#ifdef TEST_SERVER
    int read_result = mode8_verify_written_sn_key_from_lfs(sn, key);
#else
    int read_result = mode8_verify_written_sn_key_from_otp(sn, key);
#endif
    if (read_result != 0) {
        log_w("MODE 8: Read back verification failed, but write was successful");
        log_w("MODE 8: Read error code: %d", read_result);
        set_led_red();  // 写入成功但验证失败，显示红灯
        return -1;
    }
    
    // 第三步：完全成功
    log_i("MODE 8: ========== COMPLETE SUCCESS ==========");
    log_i("MODE 8: Read back verification successful");
    log_i("MODE 8: All operations completed successfully!");
    set_led_green();  // 完全成功，显示绿灯
    
    return 0;  // 完全成功
}


void mode8_init()
{

    // 初始化全局LFS读写锁
    rwlock_init(&g_lfs_rwlock);

    // 重置模式8状态（无论从哪个模式切换，都重置模式8的SN号获取状态）
    mode8_reset_network_check();
    wifi_and_network_on();
    set_led_blue(); // 进入模式8时点亮蓝灯
    g_audio_playing = 1;  // 设置为1，表示正在播放test_snkey.mp3
    log_i("MODE 8: enter mode8 test_snkey.mp3");
    {
        log_i("MODE 8: Starting SN key retrieval mode");
        lwl_app_context_t *context7 = lwl_app_core_context_get_instance();
        lwl_app_audio_play(CONFIG_CP_VOICE_FILE_PATH"test_snkey.mp3", mode8_audio_callback);
        log_i("MODE 8: test_snkey.mp3 started successfully");
        log_i("MODE 8: SN key retrieval mode initialized");
    }
}

void mode8_uninit()
{
    rwlock_destroy(&g_lfs_rwlock);
    lwl_app_mode_switch_to(APP_POWER_OFF_MODE);
}