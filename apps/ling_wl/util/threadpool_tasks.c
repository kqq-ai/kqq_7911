#include "threadpool_tasks.h"
#include "app_lfs.h"
#include "network_download/net_download.h"
#include "typedef.h"
#include "log.h"
#include <stdio.h>
#include <string.h>

// 定义下载完成后的回调函数
void download_complete_callback(void *priv)
{
    log_i("Download complete callback called.\n");
    // 在这里可以处理下载完成后的逻辑，例如通知应用程序或进行其他操作
}

// 初始化下载参数
void init_download_params(struct net_download_parm *parm, const char *url)
{
    memset(parm, 0, sizeof(struct net_download_parm));
    parm->url = url;
    parm->cbuf_size = 100 * 1024;  // 设置缓存大小为200KB
    parm->timeout_millsec = 10000; // 设置超时时间为10秒
    parm->seek_low_range = 0;      // 从文件开头开始下载
    parm->seek_high_range = 0;     // 下载整个文件
    // parm->prio = 0; // 设置优先级
    // parm->max_reconnect_cnt = 3; // 最大重连次数
    // parm->save_file = 1; // 保存下载的文件
    // parm->no_wait_close = 0; // 等待下载完成后再关闭
    // parm->file_dir = ""; // 设置文件保存目录
    // parm->dir_len = strlen(parm->file_dir); // 目录长度
    // parm->cpu_index = 0; // 使用默认CPU
    // parm->net_buf = NULL; // 由库分配网络缓冲区
    // parm->start_play_threshold = 1024 * 4; // 下载缓存达到4KB时开始播放
    // parm->seek_threshold = 1024 * 200; // 跳转范围超过200KB时重建链接
}

// 下载URL内容
int download_url_content(const char *url)
{
    void *net_file = NULL;
    struct net_download_parm parm;
    int err;

    // 初始化下载参数
    init_download_params(&parm, url);

    // 打开网络下载
    err = net_download_open(&net_file, &parm);
    if (err)
    {
        log_e("net_download_open failed with error: %d\n", err);
        return err;
    }

    // 检查下载是否准备好
    while (!net_download_check_ready(net_file))
    {
        // 等待下载准备好
        log_i("Waiting for download to be ready...\n");
        // 这里可以添加一些延迟或等待逻辑
    }

    // 读取下载内容
    char buffer[1024];
    int bytes_read;
    while ((bytes_read = net_download_read(net_file, buffer, sizeof(buffer))) > 0)
    {
        // 处理下载的内容，例如写入文件或进行其他操作
        log_i("Read %d bytes from download.\n", bytes_read);
    }

    // 检查下载状态
    int status = net_download_get_status(net_file);
    if (status == NET_DOWNLOAD_COMPLETE)
    {
        log_i("Download completed successfully.\n");
    }
    else
    {
        log_e("Download failed with status: %d\n", status);
    }

    // 关闭下载
    net_download_close(net_file);

    // 调用下载完成回调
    download_complete_callback(NULL);

    return 0;
}

// 主函数，测试下载功能
void test_url_task()
{
    printf("================================== test_url_task ==========================\r\n");
    const char *url = "http://192.168.3.45:8888/M500000a1c8y2kutQE.mp3";
    int ret = download_url_content(url);
    if (ret == 0)
    {
        log_i("Download completed successfully.\n");
    }
    else
    {
        log_e("Download failed with error: %d\n", ret);
    }
    return;
}

/* 根据 role名字 来保存 opening文件 */
void save_role_opening_file_task(void *args) {
    /* 0. 初始化准备 */
    void *net_file = NULL;
    struct net_download_parm net_parm;
    int err;

    /* 1. 检查args */
    if (args == NULL) {
        log_e("args is NULL\r\n");
        return;
    }

    /* 2. 解析args */
    save_role_opening_task_param_t *task_param = (save_role_opening_task_param_t *)args;

    /* 3. 初始化下载参数 */
    init_download_params(&net_parm, task_param->url);

    log_i("Downloading from URL: %s", task_param->url);
    log_i("Saving to file: %s", task_param->opening_filename);

    /* 4. 打开网络下载 */
    err = net_download_open(&net_file, &net_parm);
    if (err) {
        log_e("net_download_open failed with error: %d\n", err);
        goto cleanup;
    }

    /* 5. 打开文件 */
    lfs_file_t lfs_file;
    err = lfs_file_open(task_param->lfs, &lfs_file, task_param->opening_filename, LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC);
    if (err) {
        log_e("lfs_file_open fail! err code : %d\n", err);
        goto cleanup;
    }

    /* 6. 检查下载是否准备好 */
    while (!net_download_check_ready(net_file)) {
        log_i("Waiting for download to be ready...\n");
        // 这里可以添加一些延迟或等待逻辑
    }

    /* 7. 读取下载内容并写入文件 */
    char *buffer = (char *)malloc(1024);
    if (buffer == NULL) {
        log_e("malloc fail!\n");
        goto cleanup;
    }
    int bytes_read;
    while ((bytes_read = net_download_read(net_file, buffer, 1024)) > 0) {
        err = lfs_file_write(task_param->lfs, &lfs_file, buffer, bytes_read);
        if (err < 0) {
            log_e("lfs_file_write fail! err code : %d\n", err);
            free(buffer);
            goto cleanup;
        }
        log_i("lfs_file_write success! write %d bytes\n", err);
    }
    free(buffer);

    /* 8. 关闭文件 */
    lfs_file_close(task_param->lfs, &lfs_file);

    /* 9. 检查下载状态 */
    int status = net_download_get_status(net_file);
    if (status == NET_DOWNLOAD_COMPLETE) {
        log_i("Download completed successfully.\n");
    } else {
        log_e("Download failed with status: %d\n", status);
    }

    /* 10. 关闭下载 */
    net_download_close(net_file);

cleanup:
    /* 11. 释放资源 */
    if (task_param) {
        if (task_param->url) {
            free(task_param->url);
        }
        free(task_param);
    }

    log_i("save_role_opening_file_task end!\n");
}