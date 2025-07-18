#include "http_ota.h"

#include "system/includes.h"
#include "os/os_api.h"
#include "http/http_cli.h"
#include "fs/fs.h"
#include <stdlib.h>
#include "storage_device.h"
#include "net_update.h"
#include "app_config.h"
#include "wifi/wifi_connect.h"
#include "util/cjson_util.h"
#include "protocol/cp/c_uart_p.h"

#define DEFAULT_RECV_BUF_SIZE   (4*1024)   //数据缓存BUF大小
#define MAX_RECONNECT_CNT 10               //最大重连次数
#define LOCAL_VERSION_CODE "v0.0.36" 

static void set_led_OTA_on(void)       { uart_send_set_led_color(COLOR_YELLOW); } /* 控制 led : 正在升级 黄色 */

struct download_hdl {
    char *url;                    //下载链接
    u32 file_size;                //需要下载的文件大小
    u8 *recv_buf;                 //数据缓存BUF
    u32 recv_buf_size;            //数据缓存BUF大小
    u32 download_len;             //已下载的数据长度
    u32 reconnect_cnt;             //重连次数
    httpcli_ctx ctx;              //http请求参数
    const struct net_download_ops *download_ops; //http操作集
};

struct download_parm {
    char *url;  //下载链接
};
int OTA_download_running = 0;
//http回调函数
static int http_user_cb(void *ctx, void *buf, unsigned int size, void *priv, httpin_status status)
{
    struct download_hdl *hdl = (struct download_hdl *)priv;

    if (status == HTTPIN_HEADER) {
        //第一次连接时，记录文件总长度
        if (hdl->ctx.lowRange == 0) {
            hdl->file_size = hdl->ctx.content_length;
        }

        log_e("http_need_download_file=%d KB", hdl->file_size / 1024);
    }
    return 0;
}

static void download_task(void *priv)
{
    struct download_hdl *hdl = priv;
    void *update_fd = NULL;
    int ret = 0;
    int err = 0;
    char sock_err = 0;

    update_fd = net_fopen(CONFIG_UPGRADE_OTA_FILE_NAME, "w");
    if (!update_fd) {
        log_e("open update_fd error\n");
        goto _out_;
    }

_reconnect_:
    //发起连接请求，建立socket连接
    ret = hdl->download_ops->init(&hdl->ctx);

    if (ret != HERROR_OK) {
        if (hdl->ctx.req_exit_flag == 0) {
            if (hdl->reconnect_cnt < MAX_RECONNECT_CNT) {
                hdl->reconnect_cnt++;
                goto _reconnect_;
            } else {
                log_e("download reconnect upto max count.\n");
                goto _out_;
            }
        }
    }

    while (hdl->ctx.req_exit_flag == 0) {
        ret = hdl->download_ops->read(&hdl->ctx, (char *)hdl->recv_buf, hdl->recv_buf_size);//最大接收为recv_buf_size
        if (ret <  0) {  //读取数据失败
            sock_err = 1;
            goto _out_;
        } else {
            hdl->download_len += ret;
            err = net_fwrite(update_fd, hdl->recv_buf, ret, 0);
            if (err != ret) {
                goto _out_;
            }
        }

        if (hdl->download_len >= hdl->file_size) {
            goto _finish_;
        }
    }

_finish_:
    printf("download success.\n");

    net_fclose(update_fd, sock_err);
    system_reset();

_out_:
    //关闭网络连接
    hdl->download_ops->close(&hdl->ctx);

    net_fclose(update_fd, sock_err);
    free(hdl->url);
    free(hdl->recv_buf);
    memset(hdl, 0, sizeof(struct download_hdl));
    free(hdl);
}

int http_create_download_task(char *url)
{
    struct download_parm parm = {0};
    struct download_hdl *hdl = NULL;

    parm.url = url;

    //申请堆内存
    hdl = (struct download_hdl *)calloc(1, sizeof(struct download_hdl));

    if (hdl == NULL) {
        return -ENOMEM;
    }

    hdl->download_ops = &http_ops;
    hdl->recv_buf_size = DEFAULT_RECV_BUF_SIZE;

    hdl->url = (char *)calloc(1, strlen(parm.url) + 1);

    if (hdl->url == NULL) {
        return -ENOMEM;
    }

    hdl->recv_buf = (u8 *)calloc(1, hdl->recv_buf_size);

    if (hdl->recv_buf == NULL) {
        return -ENOMEM;
    }

    strcpy(hdl->url, parm.url);

    //http请求参数赋值
    hdl->ctx.url = hdl->url;
    hdl->ctx.cb = http_user_cb;
    hdl->ctx.priv = hdl;
    hdl->ctx.connection = "close";

    //创建下载线程
    return thread_fork("download_task", 20,  2 * 1024, 0, NULL, download_task, hdl);
}

int request_ota_info(char **pota_info)
{
    int ret = 0;
    http_body_obj http_body_buf;
    httpcli_ctx *ctx = (httpcli_ctx *)calloc(1, sizeof(httpcli_ctx));
    if (!ctx) {
        return OTAERROR_Malloc_Failed;
    }

    memset(&http_body_buf, 0x0, sizeof(http_body_obj));

    http_body_buf.recv_len = 0;
    http_body_buf.buf_len = 4 * 1024;
    http_body_buf.buf_count = 1;
    http_body_buf.p = (char *) malloc(http_body_buf.buf_len * http_body_buf.buf_count);
    if (!http_body_buf.p) {
        free(ctx);
        return OTAERROR_Malloc_Failed;
    }
    ctx->url = "https://kqq.tos-cn-beijing.volces.com/kqq_ota.json";
    ctx->timeout_millsec = 5000;
    ctx->priv = &http_body_buf;
    ctx->connection = "close";


    log_i("http get ota info start\n\n");
    ret = httpcli_get(ctx);

    if (ret != HERROR_OK) {
        log_e("http get ota info faile %d\n", ret);
        ret = OTAERROR_Net_Error;
    } else {
        if (http_body_buf.recv_len > 0) {
            log_i("\nOTA Receive %d Bytes\n", http_body_buf.recv_len);
            ret = http_body_buf.recv_len;
            *pota_info = (char *)malloc(http_body_buf.recv_len + 1);
            if (*pota_info == NULL) {
                ret = OTAERROR_Malloc_Failed;
            } else {
                memcpy(*pota_info, http_body_buf.p, http_body_buf.recv_len);
                *((*pota_info) + http_body_buf.recv_len) = '\0';
                ret = http_body_buf.recv_len;
            }
        } else {
            ret = OTAERROR_Unexpected_Error;
        }
    }

    //关闭连接
    httpcli_close(ctx);

    if (http_body_buf.p) {
        free(http_body_buf.p);
        http_body_buf.p = NULL;
    }

    if (ctx) {
        free(ctx);
        ctx = NULL;
    }
    return ret;
}

int parse_ota_info(const cJSON *nroot, ota_info_t *pota_info)
{
    if (nroot == NULL || pota_info == NULL) {
        goto error_ret;
    }
    memset(pota_info, 0, sizeof(ota_info_t));

   /* if (cjson_get_integer_value(nroot, "errno", &pota_info->_errno))
        goto error_ret;

    pota_info->errmsg = cjson_get_string_value(nroot, "errmsg");
    if (pota_info->errmsg == NULL)
        goto error_ret;

    pota_info->debug  = cjson_get_string_value(nroot, "debug");
    if (pota_info->debug == NULL)
        goto error_ret;

    cJSON *ndatas = cjson_get_array(nroot, "data");
    if (ndatas == NULL || cJSON_GetArraySize(ndatas) == 0) 
        goto error_ret;

    cJSON *ndata = cJSON_GetArrayItem(ndatas, 0);
    if (ndata == NULL)
        goto error_ret;

    pota_info->file_md5 = cjson_get_string_value(ndata, "file_md5");
    if (pota_info->file_md5 == NULL)
        goto error_ret;

    if (cjson_get_integer_value(ndata, "file_size", &pota_info->file_size))
        goto error_ret;*/

    pota_info->file_url = cjson_get_string_value(nroot, "file");
    if (pota_info->file_url == NULL)
        goto error_ret;
    
    //if (cjson_get_integer_value(nroot, "is_force", &pota_info->is_force))
    //    goto error_ret;

    pota_info->package_name = cjson_get_string_value(nroot, "file_md5");
    if (pota_info->package_name == NULL)
        goto error_ret;

    /*pota_info->release = cjson_get_string_value(ndata, "release");
    // if (pota_info->release == NULL)
    //     goto error_ret;

    pota_info->release_date = cjson_get_string_value(ndata, "release_date");
    if (pota_info->release_date == NULL)
        goto error_ret; 

    if (cjson_get_integer_value(nroot, "version_code", &pota_info->version_code))
        goto error_ret;*/

    pota_info->version_name = cjson_get_string_value(nroot, "version");
    if (pota_info->version_name == NULL)
        goto error_ret; 

    //log_i("errno       : %d", pota_info->_errno);
    //log_i("errmsg      : %s", pota_info->errmsg);
    //log_i("debug       : %s", pota_info->debug);

    log_i("file_md5    : %s", pota_info->file_md5);
    log_i("file_size   : %d", pota_info->file_size);
    log_i("file_url    : %s", pota_info->file_url);
    //log_i("is_force    : %d", pota_info->is_force);
    log_i("package_name: %s", pota_info->package_name);
    if (pota_info->release != NULL) {
        log_i("release     : %s", pota_info->release);
    } else {
        log_i("release     : null");
    }
    //log_i("release_date: %s", pota_info->release_date);
    pota_info->version_code = pota_info->version_name[strlen(pota_info->version_name) - 1]  - '0' ;
    log_i("version_code: %d", pota_info->version_code);
    log_i("version_name: %s", pota_info->version_name);

    return OTAERROR_Success;

error_ret:
    return OTAERROR_Parse_Error;
}

// 解析版本号字符串为三个整数部分
int parse_version(const char* version_str, int* major, int* minor, int* patch) {
    if (version_str == NULL || *version_str != 'v') {
        return 0; // 无效格式
    }
    
    // 跳过'v'字符
    const char* p = version_str + 1;
    
    // 解析主版本号
    *major = strtol(p, (char**)&p, 10);
    if (*p != '.') return 0;
    p++;
    
    // 解析次版本号
    *minor = strtol(p, (char**)&p, 10);
    if (*p != '.') return 0;
    p++;
    
    // 解析修订号
    *patch = strtol(p, NULL, 10);
    
    return 1; // 解析成功
}

// 比较两个版本号
int compare_versions(const char* ver1, const char* ver2) {
    int major1, minor1, patch1;
    int major2, minor2, patch2;
    
    if (!parse_version(ver1, &major1, &minor1, &patch1) ||
        !parse_version(ver2, &major2, &minor2, &patch2)) {
        return -2; // 解析失败
    }
    
    // 逐级比较版本号
    if (major1 > major2) return 1;
    if (major1 < major2) return -1;
    
    if (minor1 > minor2) return 1;
    if (minor1 < minor2) return -1;
    
    if (patch1 == (patch2 +1)) return 1; //如果前面两位相等，这个大于1，才升级
    if (patch1 < patch2) return -1;
    
    return 0; // 版本相同
}

int do_check_and_ota()
{
    char *ota_info_json = NULL;
    int ret = request_ota_info(&ota_info_json);
    if (ret < 0 || ota_info_json == NULL) {
        log_e("[UPGRADE]failed to request OTA info: %d", ret);
        return -1;
    }
    log_i("ota_info  22222: %s", ota_info_json);

    const cJSON *nroot = cjson_parse(ota_info_json);
    if (nroot == NULL) {
        goto error_ret;
    }

    ota_info_t ota_info;
    if (parse_ota_info(nroot, &ota_info) != OTAERROR_Success) {
        goto error_ret;
    }

    int net_ver_big = compare_versions(ota_info.version_name, LOCAL_VERSION_CODE);
    if ( 0 == net_ver_big) { log_i("net:%s ===== LOCAL_VERSION_CODE:%s \r\n", ota_info.version_name, LOCAL_VERSION_CODE);}
    if ( 1 == net_ver_big) { //< ota_info.version_code) {
        log_i("%s < %s, need OTA.", LOCAL_VERSION_CODE, ota_info.version_name);
        log_i("download %s", ota_info.file_url);
        set_led_OTA_on();  // 控制 led : 正在升级 黄色

        size_t url_len = strlen(ota_info.file_url);
        char *ufw_url = malloc(url_len + 1);
        strncpy(ufw_url, ota_info.file_url, url_len + 1);
        OTA_download_running = 1;   /// reset after OTA

        http_create_download_task(ufw_url);
        ret = 1;
    } else {
        ret = 0;
    }
    
    if (nroot != NULL) {
        cJSON_Delete(nroot);
        nroot = NULL;
    }
    if (ota_info_json != NULL) {
        free(ota_info_json);
        ota_info_json = NULL;
    }
    return ret;

error_ret:
    if (nroot != NULL) {
        cJSON_Delete(nroot);
        nroot = NULL;
    }
    if (ota_info_json != NULL) {
        free(ota_info_json);
        ota_info_json = NULL;
    }
    return -1;
}
