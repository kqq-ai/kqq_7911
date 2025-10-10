#include "ble_distance.h"
#include "string.h"
#include "system/os/os_api.h"
#include "system/os/os_error.h"
#include "init.h"

#include "btstack/le/le_common_define.h"
#include "ble_simple_client.h"
#include "ble_att_util.h"
#include "bt_ble/client/ble_dev_task_scheduler.h"
#include "../../protocol/cp/c_uart_p.h"
#define swap(a,b) {a =a ^ b; b = a ^ b; a = a ^ b;}

#include "math.h"
#define APP_STATUS (1)
#define RSSI_EXPIRE_TIME_MS (2200) // RSSI样本过期时间：5秒
#define step_max_change (0.3) // m/s


// 使用系统预定义的颜色常量
static void set_led_red(void)        { uart_send_set_led_color(COLOR_RED); }
static void set_led_orange(void)     { uart_send_set_led_color(COLOR_ORANGE); }
static void set_led_green(void)      { uart_send_set_led_color(COLOR_GREEN); }
static void set_led_blue(void)       { uart_send_set_led_color(COLOR_BLUE); }
static void set_led_purple(void)     { uart_send_set_led_color(COLOR_PURPLE); }
// 消息类型······························
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
// 逻辑回归系数 : [[0.59431181 4.63600231 0.88283592 - 0.16861603]]
// 逻辑回归截距 : [1.2385184]
// 特征缩放均值 : [-70.87293993 - 76.96748184 - 71.51834131 16.8227893]
// 特征缩放标准差 : [8.11150689 7.21031345 7.92898719 6.48619069]
void distance_predict(const double x[5], double *prob)
{




// scale: [8.61450156 9.16385475 7.90461619 8.0518822  0.60653504]
    // double mean[5] = {-70.16632919, -78.29563205, -74.61802141, -70.76800694, 2.65483616};
    // double mean[5] = {-68.51584699 , -76.78142077, -72.85846995, -69.6284153 , 1.94710464};
    double mean[5] = {-67.80083565 ,-76.8551532 ,-72.53203343 ,-68.89415042 ,2.24017005};

    // double scale[5] = {8.03458968, 8.73837695, 7.82651692, 8.22573379, 0.64757132};
    // double scale[5] = {8.16111801, 8.61998467, 7.47205518, 7.59577544, 0.76510294};
    double scale[5] = {8.61450156,9.16385475, 7.90461619, 8.0518822, 0.60653504};

    // 回归系数
    // double coef[5] = {-0.04444551, -0.12459655, -0.2989419, -0.02093879, 0.05144372};
    // double coef[5] = {-0.08298197, -0.04560951, -0.32894203, -0.11677333, 0.09981421};
    double coef[5] = {-0.09285038, -0.02243895, -0.4047161, -0.06951638, 0.0952348};
    
    // double intercept = 1.086524432284897;
    // double intercept = 0.8484964330457704;
    double intercept = 0.8503851306122057;
    // intercept: 0.8484964330457704
    // 输入特征
    // double x[5] = {sample_max, sample_min, median, max_2, sum};
    double x_scaled[5];

    // 1. 标准化
    for (int i = 0; i < 5; i++)
    {
        x_scaled[i] = (x[i] - mean[i]) / scale[i];
    }

    // 2. 线性组合 (log(y_pred))
    double log_y = intercept;
    for (int i = 0; i < 5; i++)
    {
        log_y += coef[i] * x_scaled[i];
    }

    // 3. exp 还原
    double y_pred = exp(log_y);
    if (prob != NULL)
    {
        *prob = y_pred;
    }
    // return y_pred;
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
    if (p_eend_dev->distance_info.rssi_sample_count <= 0) {
        // p_eend_dev->distance_info.rssi = -128;
        p_eend_dev->distance_info.rssi = p_eend_dev->distance_info.rssi >= -118? p_eend_dev->distance_info.rssi - 10 : -128;
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
    // static int samples[MAX_SAMPLES];
    // int max_sample;
    // int max_2_sample;
    // int min_sample;
    // double possible = 0.0;
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

    //预测处理
    // for (int i = 0; i < sample_count && i < MAX_SAMPLES; ++i)
    // {
    //     sum += p_eend_dev->distance_info.rssi_samples[i];
    //     samples[i] = p_eend_dev->distance_info.rssi_samples[i];
    // }

    // for (int i = 0; i < sample_count - 1; i++)
    // {
    //     for (int j = 0; j < sample_count - i - 1; j++)
    //     {
    //         if (samples[j] > samples[j + 1])
    //         {
    //             int temp = samples[j];
    //             samples[j] = samples[j + 1];
    //             samples[j + 1] = temp;
    //         }
    //     }
    // }

    // max_sample = samples[sample_count - 1];
    // max_2_sample = samples[sample_count - 2];
    // min_sample = samples[0];
    // int median = 0;
    // if (sample_count > 0)
    // {
    //     if (sample_count % 2 == 0)
    //     {
    //         median = (samples[sample_count / 2 - 1] + samples[sample_count / 2]) / 2;
    //     }
    //     else
    //     {
    //         median = samples[sample_count / 2];
    //     }
    // }
    // if (sample_count >= 10)
    // {
    // distance_predict((double[5]){max_sample, min_sample, median, max_2_sample, log(sample_count)}, &possible);
    // log_i("[Predict]: sn: %s max_sample : %d, max_2_sample : %d, median : %d, sample_count : %d, possible : %f\n", sn_string, max_sample, max_2_sample, median, sample_count, possible);
    // }

    // int original_average = sum / sample_count;
    // int original_average = median;

    log_d("[MEASDIST]original_average : %d\n", original_average);
#if PRINT_RSSI_LOG
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
    // offset += snprintf(raw_samples_str + offset, sizeof(raw_samples_str) - offset, ", possible=%f", possible);
    // 打印完整的原始RSSI信息
    // log_mqtt("%s", raw_samples_str);
    // log_i("[MQTT message] %s", raw_samples_str);
    log_i("[MQTT message] %s, possible=%f", raw_samples_str, possible);
#endif
    // 使用简单均值：直接使用所有样本的平均值
    // p_eend_dev->distance_info.rssi = (s8)original_average;
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

static void active_op_callback(uint8_t code, void *priv)
{
    lwl_eend_dev_t *dev = (lwl_eend_dev_t *)priv;

    if (code == BDOCode_Success)
    {
        dev->active_time = timer_get_ms();
        log_i("[BDO]Active success!");
    }
}

static void scan_rssi_record_cb(adv_report_t *report_pt, u16 len, void *priv)
{
    if (!adv_pre_match_impl(report_pt->data, report_pt->length))
    {
        return;
    }

    lwl_app_context_t *context = lwl_app_core_context_get_instance();

    lwl_cqueue_adv_data_t adv_data;

    memset(&adv_data, 0, sizeof(adv_data));
    adv_data.rssi = report_pt->rssi;
    memcpy(adv_data.address, report_pt->address, 6);
    adv_data.address_type = report_pt->address_type;
    adv_data.adv_data_length = ble_adv_report_get_data(report_pt->data, report_pt->length, HCI_EIR_DATATYPE_MANUFACTURER_SPECIFIC_DATA, adv_data.adv_data);

    if (MYERROR_Is_Full == my_blocking_queue_try_enqueue(&context->adv_queue, &adv_data))
    {
        log_e("adv_queue full\n");
    }
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
        // if (p_eend_dev->distance_info.distance_type != DISTANCE_0)
        // {
        //      log_mqtt("BLE DISTANCE_0: %s, RSSI=%d lower than 60db", p_eend_dev->sn, rssi);
        // }
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
    
    // if(p_eend_dev->distance_info.distance<2.0f&& p_eend_dev->distance_info.distance!=0.0f){
    //     p_eend_dev->distance_info.distance_type = DISTANCE_0;
    // }else if(p_eend_dev->distance_info.distance>=2.0f && p_eend_dev->distance_info.distance<5.0f){
    //     p_eend_dev->distance_info.distance_type = DISTANCE_1;
    // }else if(p_eend_dev->distance_info.distance>=5.0f){
    //     p_eend_dev->distance_info.distance_type = DISTANCE_2;

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

                clean_expired_eend();
                // clean_all_dev_rssi_samples();distance_change
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
    lwl_cqueue_adv_data_t adv_data;

    while (1)
    {
        my_blocking_queue_dequeue(&temp_ctx->adv_queue, &adv_data);

        s8 rssi = adv_data.rssi;

        /*
        待激活广播格式：2(固定前缀) + 1(电量 + OFF) + 16(序列号)
        已激活广播格式：2(固定前缀) + 1(电量 + OFF) + 16(序列号) + 4(截止时间)
        开机/关机中广播格式：2(固定前缀) + 1(电量 + CONNECTING)
        已开机广播格式：2(固定前缀) + 1(电量 + CONNECTED)
        */
        uint8_t *buffer = adv_data.adv_data;
        int r = adv_data.adv_data_length;
        if (r < 19 || r > 23)
            continue; // 长度异常，忽略

#if 1
        char sn[16] = "";
        memcpy(sn, adv_data.adv_data + 3, 15);
        if (sn[0] != 'K' || sn[1] != 'P')
            continue; // 过滤非E端设备
#else
        char *sn = (char *)buffer + 3;
#endif
        sn[15] = '\0'; // 确保字符串以 null 结尾
        uint8_t bat_level = (buffer[2] >> 4) & 0x0F;
        uint8_t adv_state = (buffer[2] >> 0) & 0x0F;
        uint8_t has_deadline = (r == 23) ? 1 : 0;
        if (adv_state > 2) continue; // 过滤状态异常的广播

        lwl_eend_dev_t *dev = find_eend_dev_by_sn(sn);
        if (dev == NULL)
        {
            log_i("[MEASDIST]add new device: %s\n", sn);
            dev = add_new_eend_dev(sn);

            lwl_event_t e;
            e.event_type = LWL_EVENT_TYPE_EEND_DEV_NEW_IN;
            e.new_in.pdev = dev;
            lwl_event_notify(&e);
        }
        uint8_t adv_state_latest = dev->adv_state;
        /// log_i("[MEASDIST]RSSI = %d bat = %d", rssi, bat_level);

        dev->bat_level = bat_level;
        dev->adv_state = adv_state;
        dev->has_deadline = has_deadline;
        dev->update_time = timer_get_ms();
        dev->address_type = adv_data.address_type;    // 其他任务会读，可能存在安全问题，也可能没问题
        memcpy(dev->address, adv_data.address, 6);    // 其他任务会读，可能存在安全问题，也可能没问题

        if (dev->has_deadline)
        {
            dev->deadline = buffer[19] | (buffer[20] << 8) | (buffer[21] << 16) | (buffer[22] << 24);
            log_i("[MEASDIST]Has deadline %d", dev->deadline);
        }
        else
        {
            log_i("[MEASDIST]Has not deadline");
        }

        if (adv_state_latest != BDPAS_CONNECTED && dev->adv_state == BDPAS_CONNECTED)
        {
            extern void notify_e_conncted();
            notify_e_conncted();
        }
        if (adv_state_latest != BDPAS_OFF && dev->adv_state == BDPAS_OFF)
        {
            extern void notify_e_disconncted();
            notify_e_disconncted();
        }
        os_mutex_pend(&dev->distance_info.mutex, 0);
        // Store the RSSI value and timestamp with safety checks
        // 严格的边界检查，防止数组越界
        if (dev->distance_info.rssi_sample_count >= 0 && dev->distance_info.rssi_sample_count < MAX_SAMPLES)
        {
            int idx = dev->distance_info.rssi_sample_count;
            dev->distance_info.rssi_samples[idx] = rssi;
            dev->distance_info.rssi_timestamps[idx] = timer_get_ms();
            dev->distance_info.rssi_sample_count++;
            log_d("[RSSI_STORE] %s: Stored RSSI=%d", sn, rssi);

            char raw_samples_str[512] = {0};
            int offset = 0;
            offset += snprintf(raw_samples_str + offset, sizeof(raw_samples_str) - offset,
                            "[RAW_RSSI] %s: count=%d, samples=[", sn, dev->distance_info.rssi_sample_count);

            for (int i = 0; i < dev->distance_info.rssi_sample_count && i < MAX_SAMPLES && offset < (sizeof(raw_samples_str) - 50); ++i)
            {
                if (i > 0)
                {
                    offset += snprintf(raw_samples_str + offset, sizeof(raw_samples_str) - offset, ",");
                }
                offset += snprintf(raw_samples_str + offset, sizeof(raw_samples_str) - offset, "%d",
                                dev->distance_info.rssi_samples[i]);
            }
            offset += snprintf(raw_samples_str + offset, sizeof(raw_samples_str) - offset, "], timestamps=[");
            for (int i = 0; i < dev->distance_info.rssi_sample_count && i < MAX_SAMPLES && offset < (sizeof(raw_samples_str) - 50); ++i)
            {
                if (i > 0)
                {
                    offset += snprintf(raw_samples_str + offset, sizeof(raw_samples_str) - offset, ",");
                }
                offset += snprintf(raw_samples_str + offset, sizeof(raw_samples_str) - offset, "%u",
                                dev->distance_info.rssi_timestamps[idx]-dev->distance_info.rssi_timestamps[i]);
            }

            // offset += snprintf(raw_samples_str + offset, sizeof(raw_samples_str) - offset, "], original_average=%d", original_average);
            offset += snprintf(raw_samples_str + offset, sizeof(raw_samples_str) - offset, "]");
            log_i("%s,distance=%.2f", raw_samples_str,dev->distance_info.distance);
            if(dev->distance_info.rssi_sample_count>3){
                static int samples[MAX_SAMPLES];
                int samples_count=0;
                int max_sample;
                int max_2_sample;
                int min_sample;
                double possible = 0.0;
                int median = 0;
                for (int i = 0; i < dev->distance_info.rssi_sample_count && i < MAX_SAMPLES; i++)
                {
                    if(dev->distance_info.rssi_timestamps[dev->distance_info.rssi_sample_count-1]-dev->distance_info.rssi_timestamps[i]<1000){
                        samples[samples_count] = dev->distance_info.rssi_samples[i];
                        samples_count++;
                    }

                }

                for (int i = 0; i < samples_count - 1; i++)
                {
                    for (int j = 0; j < samples_count - i - 1; j++)
                    {
                        if (samples[j] > samples[j + 1])
                        {
                            int temp = samples[j];
                            samples[j] = samples[j + 1];
                            samples[j + 1] = temp;
                        }
                    }
                }
                if (samples_count> 0)
                {
                    if (samples_count % 2 == 0)
                    {
                        median = (samples[samples_count / 2 - 1] + samples[samples_count / 2]) / 2;
                    }
                    else
                    {
                        median = samples[samples_count / 2];
                    }
                }

                max_sample = samples[samples_count- 1];
                max_2_sample = samples[samples_count- 2];
                min_sample = samples[0];
                double delta_time = (dev->distance_info.rssi_timestamps[dev->distance_info.rssi_sample_count-1]-dev->distance_info.rssi_timestamps[dev->distance_info.rssi_sample_count-2])/1000.0f;
                // log_i("[Distance_Filter]: sn: %s delta_time : %.2f\n", sn, delta_time);
                distance_predict((double[5]){max_sample, min_sample, median, max_2_sample, log(samples_count)}, &possible);
                log_i("[Predict]: sn: %s max_sample : %d, max_2_sample : %d, median : %d, sample_count : %d, possible : %f\n", sn, max_sample, max_2_sample, median, samples_count, possible);
                // double step_max_change=0.5f;
                double change_rate=0.5f; // 每秒最大变化率，单位米/秒
                double step_change=delta_time>step_max_change?step_max_change:delta_time;
                step_change=step_change<0.1f?0.1f:step_change;
                double last_impace=0.35f*dev->distance_info.distance_change*delta_time>0.1f?0.1f:0.35f*dev->distance_info.distance_change*delta_time;
                double distance_max=dev->distance_info.distance+step_change+last_impace;
                double distance_min=dev->distance_info.distance-step_change+last_impace;
                if(distance_min<0.2f) distance_min=0.2f;
                distance_max=distance_max<distance_min?distance_min+0.3f:distance_max;
                log_d("[Distance_Filter]: sn: %s distance_max : %f, distance_min : %f, distance:%f\n", sn, distance_max, distance_min,possible);
                dev->distance_info.last_distance=dev->distance_info.distance;
                dev->distance_info.distance=fmax(fmin(possible, distance_max), distance_min);
                dev->distance_info.distance_change=(dev->distance_info.distance-dev->distance_info.last_distance)/delta_time;
                // dev->distance_info.speed=speed_limited;
                log_d("[Distance_Filter]: sn: %s distance : %f\n", sn, dev->distance_info.distance);
                if (strcmp(sn, "KPCNC5F10AEEFE2") == 0)
                {
                    
                    log_i("dect id:%s",sn);
                    if(dev->distance_info.distance<2.0f){
                        if(dev->distance_info.last_distance<2.0f){
                            dev->distance_info.test_count++;
                            if(dev->distance_info.test_count>3){
                                // set_led_red();
                                dev->distance_info.rssi=-40;
                            }
                        }else{
                            dev->distance_info.test_count=0;
                            // set_led_orange();
                        }
                    }else if(dev->distance_info.distance>=2.0f && dev->distance_info.distance<3.5f){
                        // set_led_green();
                        dev->distance_info.rssi=-70;
                    }else{
                        if(dev->distance_info.last_distance>3.5f){
                            dev->distance_info.test_count++;
                            dev->distance_info.rssi=-90;
                            // set_led_blue();
                        }else{
                            dev->distance_info.test_count=0;
                            // set_led_purple();
                        }
                        
                    }
                }

        }

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

        // os_mutex_pend(&dev->distance_info.mutex, 0);
        // // Store the RSSI value
        // dev->distance_info.rssi_samples[dev->distance_info.rssi_sample_count++] = rssi;
        // if (dev->distance_info.rssi_sample_count == MAX_SAMPLES) {
        //     dev->distance_info.rssi_sample_count = 0;
        // }
        // os_mutex_post(&dev->distance_info.mutex);

        u32 timestamp_now = lwl_rtc_timer_get_sec();

        // 激活的前提条件：1. 广播状态为 OFF, 2. 当前没有正在进行的激活/开机/关机任务, 3. 距离最近一次激活时间超过3秒(防止短时间内重复激活)
        if (dev->adv_state == BDPAS_OFF && dev->op_state == BDO_STATE_IDLE && ((timer_get_ms() - dev->active_time) > 3000)) {
            if (!dev->has_deadline) {
                // 如果广播是未激活的广播，则尝试进行激活
                log_i("[MEASDIST]Active");
                bdo_active(dev->sn, 9000, active_op_callback, dev);
            } else if (dev->has_deadline && dev->deadline < (timestamp_now)) {
                // 如果过期时间是一个过去的时间，则尝试进行激活
                log_i("[MEASDIST]Active");
                bdo_active(dev->sn, 9000, active_op_callback, dev);
            }else if (dev->has_deadline && (dev->deadline - 10) < (timestamp_now)) {
                // 如果距离过期时间仅剩 10s，则尝试进行激活
                log_i("[MEASDIST]Active");
                bdo_active(dev->sn, 9000, active_op_callback, dev);
            }
        }

        log_d("[MEASDIST]over");
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
