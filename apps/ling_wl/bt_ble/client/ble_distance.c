#include "ble_distance.h"
#include "string.h"
#include "system/os/os_api.h"
#include "system/os/os_error.h"
#include "init.h"

#include "btstack/le/le_common_define.h"
#include "ble_simple_client.h"
#include "ble_att_util.h"
#include "log/log_mqtt.h"
#define APP_STATUS (1)
#define RSSI_EXPIRE_TIME_MS (5000) // RSSI样本过期时间：5秒

// 消息类型
enum
{
    MESSAGE_INIT = 0x01,
    MESSAGE_STOP = 0x02,
    MESSAGE_START = 0x03,
};

// distance task status
typedef enum
{
    DISTANCE_TASK_STOP = 0,
    DISTANCE_TASK_INIT,
    DISTANCE_TASK_START,
} distance_task_status_t;

static int argc = 2;
static int argv[30] = {0};
static int recv_argv[30] = {0};

static void scan_rssi_record_cb(adv_report_t *report_pt, u16 len, void *priv);
static int creat_scan_adv_process_task(void);

static const char *distance_type_text[] = {
    [DISTANCE_0] = "e < 2    ",
    [DISTANCE_1] = "2 < e < 5",
    [DISTANCE_2] = "5 < e    ",
    [DISTANCE_3] = "out range",
};

inline const char *distance_type_to_text(enum distance_type type)
{
    return distance_type_text[type];
}

// 清空RSSI样本
static void clear_rssi_samples(lwl_eend_dev_t *p_eend_dev, void *priv)
{
    // 增加NULL指针检查
    if (!p_eend_dev)
    {
        log_i("[CLEAR_RSSI] ERROR: NULL device pointer");
        return;
    }

    os_mutex_pend(&p_eend_dev->distance_info.mutex, 0);
    memset(p_eend_dev->distance_info.rssi_samples, 0, sizeof(p_eend_dev->distance_info.rssi_samples));
    memset(p_eend_dev->distance_info.rssi_timestamps, 0, sizeof(p_eend_dev->distance_info.rssi_timestamps));
    p_eend_dev->distance_info.rssi_sample_count = 0;
    os_mutex_post(&p_eend_dev->distance_info.mutex);
}

// 过期清理
static void simple_expire_cleanup(lwl_eend_dev_t *dev, void *priv)
{
    // 增加NULL指针检查
    if (!dev)
    {
        log_i("[EXPIRE_CLEANUP] ERROR: NULL device pointer");
        return;
    }

    os_mutex_pend(&dev->distance_info.mutex, 0);
    u32 now = timer_get_ms();
    int valid = 0;
    int sample_count = dev->distance_info.rssi_sample_count;

    // 边界检查
    if (sample_count < 0 || sample_count > MAX_SAMPLES)
    {
        log_i("[EXPIRE_CLEANUP] ERROR: invalid sample_count %d, resetting", sample_count);
        dev->distance_info.rssi_sample_count = 0;
        os_mutex_post(&dev->distance_info.mutex);
        return;
    }

    for (int i = 0; i < sample_count && i < MAX_SAMPLES; i++)
    {
        // 处理时间戳溢出情况，当now < timestamp时说明发生了溢出
        u32 time_diff;
        if (now >= dev->distance_info.rssi_timestamps[i])
        {
            time_diff = now - dev->distance_info.rssi_timestamps[i];
        }
        else
        {
            // 时间戳溢出，计算正确的时间差
            time_diff = (0xFFFFFFFF - dev->distance_info.rssi_timestamps[i]) + now + 1;
        }

        if (time_diff < RSSI_EXPIRE_TIME_MS)
        {
            if (valid != i && valid < MAX_SAMPLES)
            {
                dev->distance_info.rssi_samples[valid] = dev->distance_info.rssi_samples[i];
                dev->distance_info.rssi_timestamps[valid] = dev->distance_info.rssi_timestamps[i];
            }
            valid++;
        }
    }
    dev->distance_info.rssi_sample_count = valid;
    os_mutex_post(&dev->distance_info.mutex);
}

// RSSI处理
// 计算平均RSSI并过滤掉低于平均值一半的数据点
static void process_rssi_samples(lwl_eend_dev_t *p_eend_dev, void *priv)
{
    // 增加NULL指针检查
    if (!p_eend_dev)
    {
        log_i("[PROCESS_RSSI] ERROR: NULL device pointer");
        return;
    }

    log_d("[MEASDIST]===process_rssi======\r\n");
    os_mutex_pend(&p_eend_dev->distance_info.mutex, 0);
    if (p_eend_dev->distance_info.rssi_sample_count <= 0)
    {
        os_mutex_post(&p_eend_dev->distance_info.mutex);
        return;
    }

    char sn_string[SN_SIZE * 2 + 1] = "(NULL)";
    // 增加get_string回调函数的NULL检查
    if (p_eend_dev->get_string)
    {
        p_eend_dev->get_string(p_eend_dev, sn_string, sizeof(sn_string));
    }

    log_d("[MEASDIST]===process_rssi   %s  sample_count : %d          ======\r\n", sn_string, p_eend_dev->distance_info.rssi_sample_count);

    // 计算原始平均RSSI（加强边界检查）
    int sum = 0;
    int sample_count = p_eend_dev->distance_info.rssi_sample_count;

    // 严格的边界检查，防止数组越界
    if (sample_count > MAX_SAMPLES)
    {
        log_i("[PROCESS_RSSI] %s: ERROR - sample_count %d exceeds MAX_SAMPLES %d, resetting",
              sn_string, sample_count, MAX_SAMPLES);
        sample_count = 0;
        p_eend_dev->distance_info.rssi_sample_count = 0;
        os_mutex_post(&p_eend_dev->distance_info.mutex);
        return;
    }

    for (int i = 0; i < sample_count && i < MAX_SAMPLES; ++i)
    {
        sum += p_eend_dev->distance_info.rssi_samples[i];
    }
    int original_average = sum / sample_count;
    log_d("[MEASDIST]original_average : %d\n", original_average);

    // 构建原始RSSI样本数组字符串（加强边界检查）
    char raw_samples_str[512] = {0};
    int offset = 0;
    offset += snprintf(raw_samples_str + offset, sizeof(raw_samples_str) - offset,
                       "[RAW_RSSI] %s: count=%d, samples=[", sn_string, sample_count);

    for (int i = 0; i < sample_count && i < MAX_SAMPLES && offset < (sizeof(raw_samples_str) - 50); ++i)
    {
        if (i > 0)
        {
            offset += snprintf(raw_samples_str + offset, sizeof(raw_samples_str) - offset, ",");
        }
        offset += snprintf(raw_samples_str + offset, sizeof(raw_samples_str) - offset, "%d",
                           p_eend_dev->distance_info.rssi_samples[i]);
    }
    offset += snprintf(raw_samples_str + offset, sizeof(raw_samples_str) - offset, "], original_average=%d", original_average);

    // 打印完整的原始RSSI信息
    log_mqtt("%s", raw_samples_str);

    // 使用简单均值：直接使用所有样本的平均值
    p_eend_dev->distance_info.rssi = (s8)original_average;

    // 打印最终RSSI信息
    // log_i("[FINAL_RSSI] %s: simple_average=%d (using all %d samples)",
    //       sn_string, p_eend_dev->distance_info.rssi, p_eend_dev->distance_info.rssi_sample_count);

    // log_d("[MEASDIST]%s  Average RSSI: %d\n", sn_string, p_eend_dev->distance_info.rssi);
    os_mutex_post(&p_eend_dev->distance_info.mutex);
}

// 处理整个数组
static void process_all_dev_rssi_samples()
{
    eend_dev_traverse(process_rssi_samples, NULL);
}

// 清除所有RSSI样本（初始化用）
static void clean_all_dev_rssi_samples()
{
    eend_dev_traverse(clear_rssi_samples, NULL);
}

// 清理所有设备的过期RSSI样本
static void remove_all_dev_expired_rssi_samples()
{
    eend_dev_traverse(simple_expire_cleanup, NULL);
}

void monitor_init(void)
{
    int ret = 0;

    // 配置验证日志
    log_i("[MEASDIST] monitor_init: MAX_SAMPLES=%d, RSSI_EXPIRE_TIME_MS=%dms",
          MAX_SAMPLES, RSSI_EXPIRE_TIME_MS);

    lwl_app_context_t *ctx = lwl_app_core_context_get_instance();
    // 创建扫描和广告数据处理任务
    creat_scan_adv_process_task();

    // 清空所有设备的RSSI样本
    clean_all_dev_rssi_samples();
    // 注册回调函数
    ble_adv_report_event_user_handler_add(scan_rssi_record_cb, NULL);

    argv[0] = MESSAGE_INIT;
    argv[1] = 10;
    // 用系统的发消息的函数，发送一个消息代表开始接收RSSI
    ret = __os_taskq_post("distance_main", Q_USER, argc, argv);
    if (ret == OS_TASK_NOT_EXIST)
    {
        log_i("[MEASDIST]the task is not exist\n");
    }
    else if (ret == OS_ERR_POST_NULL_PTR)
    {
        log_i("[MEASDIST]the message queue is not exist\n");
    }
    else
    {
        log_i("[MEASDIST]post success\n");
    }
    log_i("[MEASDIST]monitor_init\n");
}

void monitor_start(void)
{
    int ret = 0;

    argv[0] = MESSAGE_START;
    argv[1] = 11;
    // 用系统的发消息的函数，发送一个消息代表我的任务开始
    ret = __os_taskq_post("distance_main", Q_USER, argc, argv);
    if (ret == OS_TASK_NOT_EXIST)
    {
        log_i("[MEASDIST]the task is not exist\n");
    }
    else if (ret == OS_ERR_POST_NULL_PTR)
    {
        log_i("[MEASDIST]the message queue is not exist\n");
    }
    else
    {
        log_i("[MEASDIST]post success\n");
    }
    log_i("[MEASDIST]monitor_start : start measure the blue distance\n");
}

void monitor_stop(void)
{
    int ret = 0;
    argv[0] = MESSAGE_STOP;
    argv[1] = 12;
    // 用系统的发消息的函数，发送一个消息代表我的任务停止
    ret = __os_taskq_post("distance_main", Q_USER, argc, argv);
    if (ret == OS_TASK_NOT_EXIST)
    {
        log_i("[MEASDIST]the task is not exist\n");
    }
    else if (ret == OS_ERR_POST_NULL_PTR)
    {
        log_i("[MEASDIST]the message queue is not exist\n");
    }
    else
    {
        log_i("[MEASDIST]post success\n");
    }
    log_i("[MEASDIST]monitor_stop : clear_all_eend_dev\n");
    clear_all_eend_dev(); // 清空所有E端设备信息
    log_i("[MEASDIST]monitor_stop : clear_all_eend_dev end\n");
}

extern bool g_handshake_is_ongoing;

static void scan_rssi_record_cb(adv_report_t *report_pt, u16 len, void *priv)
{
#define FORTEST 0
#if FORTEST
    uint8_t buffer[32];
    memset(buffer, 0, sizeof(buffer));

    int r = ble_adv_report_get_data(report_pt->data, report_pt->length, HCI_EIR_DATATYPE_COMPLETE_LOCAL_NAME, buffer);
    if (r == -1)
    {
        return;
    }
    char *name = buffer;
    buffer[r] = '\0';

    log_i("%s %d", name, r);

    if (r != 7 || strcmp(name, "AIFANTA") != 0)
    {
        return;
    }
    char *sn = "WCCN24H28FEEEF7";
    s8 rssi = report_pt->rssi;
#else
    if (!adv_pre_match_impl(report_pt->data, report_pt->length))
    {
        return;
    }

    lwl_app_context_t *ctx = lwl_app_core_context_get_instance();
    lwl_cqueue_adv_data_t adv_queue_data;
    memset(&adv_queue_data, 0, sizeof(adv_queue_data));
    adv_queue_data.rssi = report_pt->rssi;
    memcpy(adv_queue_data.address, report_pt->address, 6);
    adv_queue_data.address_type = report_pt->address_type;

    // s8 rssi = report_pt->rssi;

    /*
    关机广播格式：2(固定前缀) + 1(电量) + 16(序列号)
    开机广播格式：2(固定前缀) + 1(电量) + 16(序列号) + 6(绑定的C端MAC)
    */
    // uint8_t buffer[32];
    int ret = ble_adv_report_get_data(report_pt->data, report_pt->length, HCI_EIR_DATATYPE_MANUFACTURER_SPECIFIC_DATA, adv_queue_data.adv_data);
    adv_queue_data.adv_data_length = ret;
    // log_i("ble_adv_report_get_data ret:%d",ret);
    // log_i("adv cb adv_data_length:%d\r\n", report_pt->length);
    ret = my_blocking_queue_try_enqueue(&ctx->adv_queue, &adv_queue_data);
    if (MYERROR_Is_Full == ret)
    {
        log_i("adv_queue full\n");
    }
// #if 1
//     char sn[16] = "";
//     memcpy(sn, buffer + 3, 15);
// #else
//     char *sn = (char *)buffer + 3;
// #endif
//     sn[15] = '\0'; // 确保字符串以 null 结尾
//     uint8_t bat_level = buffer[2];
//     uint8_t eend_power_state = (r == 25) ? 1 : 0;

//     if (strcmp(sn, "WCCN24H28FEEEFA") == 0) {
//         strcpy(sn, "WECNC4H06CPLJE7");
//     }
#endif
#if 0 // 测试用
    lwl_eend_dev_t *dev = find_eend_dev_by_sn(sn);
    if (dev == NULL) {
        log_i("[MEASDIST]add new device: %s\n", sn);
        dev = add_new_eend_dev(sn);

        lwl_event_t e;
        e.event_type = LWL_EVENT_TYPE_EEND_DEV_NEW_IN;
        e.new_in.pdev = dev;
        lwl_event_notify(&e);
    }
    log_d("[MEASDIST]RSSI = %d bat = %d", rssi, bat_level);
    uint8_t power_on_state_latest = dev->power_on_state;
    dev->bat_level = bat_level;
    dev->power_on_state = eend_power_state;
    
    if (dev->power_on_state) {
        memcpy(dev->bound_mac, buffer + 19, sizeof(dev->bound_mac));

        /*
        1. 上一次搜索的广播数据中，无C端MAC
        2. 这一次搜索的广播数据中，有C端MAC
        3. 广播数据中的C端MAC和自身匹配
        */
        if (power_on_state_latest != dev->power_on_state && 
            memcmp(dev->bound_mac, lwl_app_core_context_get_instance()->mac, sizeof(dev->bound_mac)) == 0) {
            extern void bka_notify_turn_on_success(void);
            bka_notify_turn_on_success();
        }
    } else {
        memset(dev->bound_mac, 0, sizeof(dev->bound_mac));
    }

    os_mutex_pend(&dev->distance_info.mutex, 0);
    // Store the RSSI value
    dev->distance_info.rssi_samples[dev->distance_info.rssi_sample_count++] = rssi;
    if (dev->distance_info.rssi_sample_count == MAX_SAMPLES) {
        dev->distance_info.rssi_sample_count = 0;
    }
    os_mutex_post(&dev->distance_info.mutex);
    

    // 握手通信相关处理
    if (dev->handshake_info.need_handshake) {
        os_mutex_pend(&dev->handshake_info.mutex, 0);

        if (dev->handshake_info.handshake_flag == 1) { /* 已经握手过了 */
            os_mutex_post(&dev->handshake_info.mutex);
            return;
        }
        
        /* 更新状态 */
        dev->handshake_info.alive = 1;
        dev->handshake_info.alive_hs_fail_cnt = 0;
        dev->handshake_info.address_type = report_pt->address_type;
        memcpy(dev->handshake_info.address, report_pt->address, 6);

        if (!g_handshake_is_ongoing) {
            /* 如果没有正在进行的握手任务，就启动一次握手流程 */
            extern void start_handshake(lwl_eend_dev_t *pdev);
            start_handshake(dev);
        }
        os_mutex_post(&dev->handshake_info.mutex);
    }

    log_d("[MEASDIST]over");
#endif
}

static void get_closest_devive(lwl_eend_dev_t *p_eend_dev, void *priv)
{
    char sn_string[SN_SIZE * 2 + 1] = "(NULL)";
    p_eend_dev->get_string(p_eend_dev, sn_string, sizeof(sn_string));
    log_i("[MEASDIST] %s: %d\r\n", sn_string, p_eend_dev->distance_info.rssi);
}

static void blue_distance_notify_app()
{
    lwl_eend_dev_t *p_eend_dev = find_eend_dev_closest();
    if (p_eend_dev == NULL)
    {
        return;
    }

    lwl_event_t e;
    e.event_type = LWL_EVENT_TYPE_EEND_DEV_CLOSEST_CHANGE;
    e.distance_change.pdev = p_eend_dev;
    lwl_event_notify(&e);
}

static void blue_distance_notify_self_process(lwl_eend_dev_t *p_eend_dev, void *priv)
{
    // 增加NULL指针检查
    if (!p_eend_dev)
    {
        log_i("[DISTANCE_NOTIFY] ERROR: NULL device pointer");
        return;
    }

    s8 rssi = p_eend_dev->distance_info.rssi;

    p_eend_dev->distance_info.distance_type_last = p_eend_dev->distance_info.distance_type;

    if (rssi >= BLUE_DISTANCE_TWO_METER && rssi != -128)
    {
        p_eend_dev->distance_info.distance_type = DISTANCE_0;
    }
    else if (rssi < BLUE_DISTANCE_TWO_METER && rssi >= BLUE_DISTANCE_FIVE_METER && rssi != -128)
    {
        p_eend_dev->distance_info.distance_type = DISTANCE_1;
    }
    else if (rssi < BLUE_DISTANCE_FIVE_METER && rssi != -128)
    {
        p_eend_dev->distance_info.distance_type = DISTANCE_2;
    }
    else
    {
        p_eend_dev->distance_info.distance_type = DISTANCE_3;
    }

    if (p_eend_dev->distance_info.distance_type != p_eend_dev->distance_info.distance_type_last)
    {
        lwl_event_t e;
        e.event_type = LWL_EVENT_TYPE_EEND_DEV_DISTANCE_CHANGE;
        e.distance_change.pdev = p_eend_dev;
        lwl_event_notify(&e);
    }
}

static void blue_distance_notify_self()
{
    eend_dev_traverse(blue_distance_notify_self_process, NULL);
}

// 把这个单独用一个任务来完成
static void distance_main(void *priv)
{
    log_i("[MEASDIST] =======================================distance_main task ================================\r\n");
    int ret = 0;
    int current_time = 0;
    int notify_app_time = 0;
    int notify_self_time = 0;
    distance_task_status_t current_task_state = DISTANCE_TASK_STOP;

    while (APP_STATUS)
    {
        if (current_task_state == DISTANCE_TASK_STOP)
        {
            ret = os_taskq_pend("taskq", recv_argv, ARRAY_SIZE(recv_argv));
            if (ret == OS_TASKQ || recv_argv[0] == Q_USER)
            {
                if (recv_argv[1] == MESSAGE_INIT)
                {
                    current_task_state = DISTANCE_TASK_INIT;
                }
                if (recv_argv[1] == MESSAGE_START)
                {
                    current_task_state = DISTANCE_TASK_START;
                }
            }
            else
                continue;
        }
        else if (current_task_state == DISTANCE_TASK_INIT)
        {
            ret = os_taskq_pend("taskq", recv_argv, ARRAY_SIZE(recv_argv));
            if (ret == OS_TASKQ || recv_argv[0] == Q_USER)
            {
                if (recv_argv[1] == MESSAGE_START)
                {
                    current_task_state = DISTANCE_TASK_START;
                }
            }
            else
                continue;
        }
        else if (current_task_state == DISTANCE_TASK_START)
        {
            current_time = timer_get_ms();
            // log_i("[MEASDIST]------------------start measure ble rssi distance---------------\n");
            if (current_time - notify_app_time > NOTIFY_APP_TIME)
            {
                notify_app_time = current_time;

                // 首先清理过期数据
                remove_all_dev_expired_rssi_samples();

                // 然后处理有效样本
                process_all_dev_rssi_samples();

                // 通知距离变化
                blue_distance_notify_self();
            }

            if (current_time - notify_self_time > NOTIFY_SELF_TIME)
            {
                // process_distance_info_array(moniter_ble_numbers,current_ble_num);
                //  blue_distance_notify_self(self_function);
                notify_self_time = current_time;
            }
            ret = __os_taskq_pend(recv_argv, ARRAY_SIZE(recv_argv), 100);
            if (ret == OS_TASKQ || recv_argv[0] == Q_USER)
            {
                if (recv_argv[1] == MESSAGE_STOP)
                {
                    current_task_state = DISTANCE_TASK_STOP;
                }
            }
        }
        mdelay(100);
    }
}
static int creat_task_distance(void)
{
    int ret = 0;
    // 创建任务
    ret = os_task_create(distance_main, NULL, 10, 1024, 256, "distance_main");
    if (ret != 0)
    {
        log_i("[MEASDIST]distance_main task create failed\n");
        return -1;
    }
    return 0;
}

static void scan_adv_process_task(void *priv)
{
    lwl_app_context_t *temp_ctx = lwl_app_core_context_get_instance();
    // 增加NULL检查
    if (!temp_ctx)
    {
        log_i("[SCAN_ADV] ERROR: NULL context pointer");
        return;
    }

    lwl_cqueue_adv_data_t temp_adv_data;
    memset(&temp_adv_data, 0, sizeof(temp_adv_data));
    int ret = 0;
    while (1)
    {
        ret = my_blocking_queue_dequeue(&temp_ctx->adv_queue, &temp_adv_data);
        if (MYERROR_Success == ret)
        {
            // printf("my_blocking_queue_dequeue one time !\r\n");
            // s8 rssi = temp_adv_data->rssi;

            /*
            关机广播格式：2(固定前缀) + 1(电量) + 16(序列号)
            开机广播格式：2(固定前缀) + 1(电量) + 16(序列号) + 6(绑定的C端MAC)
            */
            // uint8_t buffer[32];
            // int r = ble_adv_report_get_data(temp_adv_data->data, temp_adv_data->length, HCI_EIR_DATATYPE_MANUFACTURER_SPECIFIC_DATA, buffer);
            int r = temp_adv_data.adv_data_length;
#if 1
            // 增加数据长度检查，防止内存访问越界
            if (r < 19)
            {
                log_i("[SCAN_ADV] ERROR: adv_data too short: %d bytes", r);
                continue;
            }

            char sn[16] = "";
            // 安全的内存拷贝，确保不会越界
            int sn_len = (r >= 18) ? 15 : (r - 3);
            if (sn_len > 0 && sn_len <= 15)
            {
                memcpy(sn, temp_adv_data.adv_data + 3, sn_len);
            }
#else
            char *sn = (char *)buffer + 3;
#endif
            sn[sn_len] = '\0'; // 确保字符串以 null 结尾
            uint8_t bat_level = (r >= 3) ? temp_adv_data.adv_data[2] : 0;
            uint8_t eend_power_state = (r == 25) ? 1 : 0;

            lwl_eend_dev_t *dev = find_eend_dev_by_sn(sn);
            if (dev == NULL)
            {
                log_i("[MEASDIST]add new device: %s\n", sn);
                dev = add_new_eend_dev(sn);
                // 增加NULL检查，防止设备创建失败
                if (dev == NULL)
                {
                    log_i("[MEASDIST] ERROR: failed to create new device for %s", sn);
                    continue;
                }

                lwl_event_t e;
                e.event_type = LWL_EVENT_TYPE_EEND_DEV_NEW_IN;
                e.new_in.pdev = dev;
                lwl_event_notify(&e);
            }
            // log_d("[MEASDIST]RSSI = %d bat = %d", temp_adv_data.rssi, bat_level);
            uint8_t power_on_state_latest = dev->power_on_state;
            /// log_i("power_on_state_latest: %d\r\n",power_on_state_latest);
            dev->bat_level = bat_level;
            dev->power_on_state = eend_power_state;
            /// log_i("dev->power_on_state: %d\r\n",dev->power_on_state);
            if (dev->power_on_state)
            {
                // 增加边界检查，防止访问越界
                if (r >= 25)
                {
                    memcpy(dev->bound_mac, temp_adv_data.adv_data + 19, sizeof(dev->bound_mac));
                }
                else
                {
                    log_i("[MEASDIST] WARNING: adv_data too short for MAC addr, r=%d", r);
                    memset(dev->bound_mac, 0, sizeof(dev->bound_mac));
                }
                /// log_i("dev->bound_mac: %02x:%02x:%02x:%02x:%02x:%02x\r\n",dev->bound_mac[0],dev->bound_mac[1],dev->bound_mac[2],dev->bound_mac[3],dev->bound_mac[4],dev->bound_mac[5]);
                /*
                1. 上一次搜索的广播数据中，无C端MAC
                2. 这一次搜索的广播数据中，有C端MAC
                3. 广播数据中的C端MAC和自身匹配
                */
                if (power_on_state_latest != dev->power_on_state &&
                    memcmp(dev->bound_mac, lwl_app_core_context_get_instance()->mac, sizeof(dev->bound_mac)) == 0)
                {
                    extern void bka_notify_turn_on_success(void);
                    bka_notify_turn_on_success();
                    log_i("MAC notify ok !\r\n");
                }
            }
            else
            {
                memset(dev->bound_mac, 0, sizeof(dev->bound_mac));
            }
            /// log_i("os_mutex_pend dev->distance_info.mutex time :%d\n",timer_get_ms());
            os_mutex_pend(&dev->distance_info.mutex, 0);
            // Store the RSSI value and timestamp with safety checks
            // 严格的边界检查，防止数组越界
            if (dev->distance_info.rssi_sample_count >= 0 && dev->distance_info.rssi_sample_count < MAX_SAMPLES)
            {
                int idx = dev->distance_info.rssi_sample_count;
                dev->distance_info.rssi_samples[idx] = temp_adv_data.rssi;
                dev->distance_info.rssi_timestamps[idx] = timer_get_ms();
                dev->distance_info.rssi_sample_count++;
            }
            else
            {
                // 数组已满或索引异常，记录错误
                log_i("[RSSI_STORE] %s: ERROR - invalid sample_count=%d (MAX=%d)",
                      sn, dev->distance_info.rssi_sample_count, MAX_SAMPLES);
                // 重置为安全状态
                dev->distance_info.rssi_sample_count = 0;
            }
            os_mutex_post(&dev->distance_info.mutex);

            // 添加RSSI存储日志
            log_d("[RSSI_STORE] %s: new_rssi=%d, sample_count=%d, timestamp=%u",
                  sn, temp_adv_data.rssi, dev->distance_info.rssi_sample_count, timer_get_ms());

            /// log_i("os_mutex_post dev->distance_info.mutex time :%d\n",timer_get_ms());

            // 握手通信相关处理
            if (dev->handshake_info.need_handshake)
            {
                /// log_i("os_mutex_pend dev->handshake_info.mutex time :%d\n",timer_get_ms());
                os_mutex_pend(&dev->handshake_info.mutex, 0);
                /// log_i("os_mutex_post dev->handshake_info.mutex time next :%d\n",timer_get_ms());
                if (dev->handshake_info.handshake_flag == 1)
                { /* 已经握手过了 */
                    os_mutex_post(&dev->handshake_info.mutex);
                    /// log_i("os_mutex_post dev->handshake_info.mutex time :%d\n",timer_get_ms());
                    continue;
                }

                /* 更新状态 */
                dev->handshake_info.alive = 1;
                dev->handshake_info.alive_hs_fail_cnt = 0;
                dev->handshake_info.address_type = temp_adv_data.address_type;
                memcpy(dev->handshake_info.address, temp_adv_data.address, 6);

                if (!g_handshake_is_ongoing)
                {
                    /* 如果没有正在进行的握手任务，就启动一次握手流程 */
                    extern void start_handshake(lwl_eend_dev_t * pdev);
                    start_handshake(dev);
                }
                os_mutex_post(&dev->handshake_info.mutex);
                /// log_i("os_mutex_post dev->handshake_info.mutex time :%d\n",timer_get_ms());
            }

            log_d("[MEASDIST]over");
        }
        else if (MYERROR_Is_Empty == ret)
        {
            log_d("[MEASDIST]empty adv_queue");
        }
    }
}

static int creat_scan_adv_process_task(void)
{
    int ret = 0;
    // 创建任务
    ret = os_task_create(scan_adv_process_task, NULL, 14, 1024, 256, "scan_adv_process");
    if (ret != 0)
    {
        log_i("[MEASDIST]scan_adv_process task create failed\n");
        return -1;
    }
    return 0;
}

late_initcall(creat_task_distance);
