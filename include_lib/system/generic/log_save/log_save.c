
#include "log_save.h"

// variables used by the filesystem
// 获取文件系统的全局句柄
extern lfs_t g_lfs;
lfs_file_t log_file_write;


#define APP_BIG_LOG_FILENAME_A "app_big_log_A.txt"
#define APP_BIG_LOG_FILENAME_B "app_big_log_B.txt"
extern uint32_t timer_get_ms(void);


#define LOG_BUFFER_SIZE 10240  // 10KB缓冲区
#define DELETE_THRESHOLD 102400    // 100KB
#define RESET_THRESHOLD 409600     // 400KB

static char log_buffer[LOG_BUFFER_SIZE];
static size_t log_buffer_offset = 0;  // 当前写入位置
static bool log_flush_required = false;  // 是否需要刷新
static size_t total_log_size = 0;    // 记录日志总大小

// 刷新缓冲区到文件
void log_flush_buffer(void) {
    if (log_buffer_offset == 0) return;  // 缓冲区为空
    
    // 检查文件大小阈值
    if (total_log_size >= RESET_THRESHOLD) {
        // 超过400KB，重置文件A
        if ( lfs_mlist_isopen(g_lfs.mlist, (struct lfs_mlist *)(&log_file_write) )) {
            lfs_file_close(&g_lfs, &log_file_write);
        }
        lfs_file_open(&g_lfs, &log_file_write, APP_BIG_LOG_FILENAME_A, 
                      LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC);
        lfs_file_close(&g_lfs, &log_file_write);
        total_log_size = 0;
        ///printf("Log file reset (size > 400KB)\n");
    } else if (total_log_size >= DELETE_THRESHOLD) {
        // 超过100KB，删除文件B
        struct lfs_info info;
        if (lfs_stat(&g_lfs, APP_BIG_LOG_FILENAME_B, &info) == LFS_ERR_OK) {
            // 如果文件B存在，删除它
            lfs_remove(&g_lfs, APP_BIG_LOG_FILENAME_B);
        }
        ///printf("Deleted fileB.txt (log size > 100KB)\n");
    }
    puts("Before write write write to LFS ~~~~~~ \r\n");
    if ( lfs_mlist_isopen(g_lfs.mlist, (struct lfs_mlist *)(&log_file_write) )) {
        lfs_file_close(&g_lfs, &log_file_write);
    }
    lfs_file_open(&g_lfs, &log_file_write, APP_BIG_LOG_FILENAME_A, 
                      LFS_O_WRONLY | LFS_O_APPEND);
    // if (! lfs_mlist_isopen(g_lfs.mlist, (struct lfs_mlist *)(&log_file_write) )) {
    //     puts(" write write write LFS_O_TRUNC LFS ~!!! \r\n");
    //     lfs_file_open(&g_lfs, &log_file_write, APP_BIG_LOG_FILENAME_A, 
    //                   LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC);
    // }
    int err = lfs_file_write(&g_lfs, &log_file_write, log_buffer, log_buffer_offset);
    if (err < 0) {
        ///printf("lfs_file_write fail! err code : %d\n", err);
        // 错误处理...
    } else {
        total_log_size += log_buffer_offset;  // 更新总大小
        puts("Batch write to LFS ok~ \r\n");
    }
    lfs_file_close(&g_lfs, &log_file_write);
    
    // 重置缓冲区
    log_buffer_offset = 0;
    log_flush_required = false;
}
// 带缓冲区的写入函数
void log_write_with_buffer(const char* data, size_t len) {
    // 检查缓冲区空间是否足够
    if (log_buffer_offset + len > LOG_BUFFER_SIZE) {
        // 缓冲区\快满，先刷新到文件
        log_flush_buffer();
    }
    
    // 复制数据到缓冲区
    memcpy(log_buffer + log_buffer_offset, data, len);
    log_buffer_offset += len;
    
    // 标记需要刷新（可选：根据条件触发）
    log_flush_required = true;
}


void log_save(int level, const char *tag, const char *format, ...) {

    // 1. 调用原有log_print输出日志
    ///log_print(level, tag, format, args); // 保留原有输出功能

    // 2. 准备保存到文件的内容
    ///va_list args_copy;
    ///va_copy(args_copy, args);

    static int kq = 0;
    kq++;
    if(kq == 6){
        puts("in LFS for LOG save~ kq == 3 \r\n");

        /* 改名lfs文件，如果B存在，就删除。把A改成B，新log存储到A */
            // 1. 检查文件B是否存在
        struct lfs_info info;
        if (lfs_stat(&g_lfs, APP_BIG_LOG_FILENAME_B, &info) == LFS_ERR_OK) {
            // 如果文件B存在，删除它
            lfs_remove(&g_lfs, APP_BIG_LOG_FILENAME_B);
        }
        
        // 2. 检查文件A是否存在
        if (lfs_stat(&g_lfs, APP_BIG_LOG_FILENAME_A, &info) == LFS_ERR_OK) {
            // 如果文件A存在，重命名为B
            lfs_rename(&g_lfs, APP_BIG_LOG_FILENAME_A, APP_BIG_LOG_FILENAME_B);
            puts("in lfs_rename APP_BIG_LOG_FILENAME_B~  \r\n");
        }
        
        // 3. 创建新的文件A
        int err = lfs_file_open(&g_lfs, &log_file_write, APP_BIG_LOG_FILENAME_A, 
                    LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC);
        
        if (err) {
            ///printf("lfs_file_open fail! err code : %d\n", err);
            kq = 0; /// ! 失败，重新进入，检查初始化，创建文件
            return;
        }
        //open 无错误
        lfs_file_close(&g_lfs, &log_file_write);
        puts("in LFS creat FILE A open ok~ \r\n");

    }
    if(kq > 8) {
        kq = 9;
        ///puts("in LFS 9 save~ 9 \r\n");
        // 1. 获取当前时间戳
        char timestamp[222] = {0};
        ///time_t t = time(NULL);
        uint32_t total_ms = timer_get_ms();
    
        uint32_t seconds = total_ms / 1000;
        uint32_t milliseconds = total_ms % 1000;
        uint32_t minutes = seconds / 60;
        seconds %= 60;
        uint32_t hours = minutes / 60;
        minutes %= 60;
        ///printf("%02u:%02u:%02u.%03u\n", hours, minutes, seconds, milliseconds);

        va_list args;
        va_start(args, format);
        // 2. 格式化用户日志
        ///char user_msg[1280];
        ///vsnprintf(user_msg, sizeof(user_msg), format, args);
    
        ///char sn_c_id[25] = "KCCNC5F12BEEEJ5";
        ///get_SN(&sn_c_id);
        ///snprintf(timestamp, sizeof(timestamp), "[%02u:%02u:%02u.%03u] %s %s\n", hours, minutes, seconds, milliseconds, "KCCNC5F12QEEGE3", format);///, user_msg
        int len_sprintf = sprintf(timestamp, "[%02u:%02u:%02u.%03u] %s ", hours, minutes, seconds, milliseconds, "KCCNC5F12YEEFH2" );///, user_msg
        size_t format_len = strlen(format); // 获取format长度
        

        // 确保总长度不超过220字节（留出两个位置给\n和\0）
        size_t max_append = 220 - len_sprintf;
        if (max_append < 0) max_append = 0;  // 防止负值

        // 如果format_len超过可用空间，截断它
        if (format_len > max_append) {
            format_len = max_append;
        }

        // 找到format中倒数第二个字符位置
        size_t last_pos = format_len - 1;

        // 复制format到timestamp末尾
        memcpy(timestamp + len_sprintf, format, format_len);

        // 更新总长度
        len_sprintf += format_len;

        // 确保倒数第二个字符是\n，最后一个是\0
        if (len_sprintf >= 2) {
            timestamp[len_sprintf - 2] = '\n';
            timestamp[len_sprintf - 1] = '\0';
        } else if (len_sprintf == 1) {
            // 如果只有一个字符，添加\n和\0
            timestamp[0] = '\n';
            timestamp[1] = '\0';
            len_sprintf = 2;
        } else {
            // 如果为空，添加\n和\0
            timestamp[0] = '\n';
            timestamp[1] = '\0';
            len_sprintf = 2;
        }
        /* 3. 写入数据 */
        log_write_with_buffer(timestamp, strlen(timestamp));

        // size_t bytes_written = lfs_file_write(&g_lfs, &log_file_write, timestamp, strlen(timestamp));
        // if (bytes_written != strlen(timestamp)){
        //     ///printf("lfs_file_write fail! err code : %zu\n", bytes_written);
        //     lfs_file_close(&g_lfs, &log_file_write);
        //     return;
        // }
        va_end(args);
        ///puts("in LFS 9 s\n");///ave~ 99999999999999999999999 
        // never close ？关机

    }

    ///printf("in LFS for LOG save~");
    ///va_end(args);
}

