#include "ble_distance.h"
#include "string.h"
#include "system/os/os_api.h"
#include "system/os/os_error.h"
#include "init.h"

#include "btstack/le/le_common_define.h"
#include "ble_simple_client.h"
#include "ble_att_util.h"

#define APP_STATUS (1)

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


// 清空RSSI样本
static void clear_rssi_samples(lwl_eend_dev_t *p_eend_dev, void *priv)
{
    os_mutex_pend(&p_eend_dev->distance_info.mutex, 0);
    p_eend_dev->distance_info.rssi_sample_count = 0;
    memset(p_eend_dev->distance_info.rssi_samples, 0, sizeof(p_eend_dev->distance_info.rssi_samples));
    os_mutex_post(&p_eend_dev->distance_info.mutex);
}

// 计算平均RSSI并过滤掉低于平均值一半的数据点
static void process_rssi_samples(lwl_eend_dev_t *p_eend_dev, void *priv)
{
    log_d("[MEASDIST]===process_rssi======\r\n");
    os_mutex_pend(&p_eend_dev->distance_info.mutex, 0);
    if (p_eend_dev->distance_info.rssi_sample_count <= 0) {
        os_mutex_post(&p_eend_dev->distance_info.mutex);
        return;
    }
        
    char sn_string[SN_SIZE * 2 + 1] = "(NULL)";
    p_eend_dev->get_string(p_eend_dev, sn_string, sizeof(sn_string));


    log_d("[MEASDIST]===process_rssi   %s  sample_count : %d          ======\r\n", sn_string, p_eend_dev->distance_info.rssi_sample_count);
    // 计算原始平均RSSI
    int sum = 0;
    for (int i = 0; i < p_eend_dev->distance_info.rssi_sample_count; ++i) {
        sum += p_eend_dev->distance_info.rssi_samples[i];
    }
    int original_average = sum / p_eend_dev->distance_info.rssi_sample_count;
    log_d("[MEASDIST]original_average : %d\n",original_average);

    // 过滤数据点，创建临时数组存储符合条件的数据点
    s8 filtered_samples[MAX_SAMPLES];
    int filtered_count = 0;
    for (int i = 0; i < p_eend_dev->distance_info.rssi_sample_count; ++i) {
        if (p_eend_dev->distance_info.rssi_samples[i] >= original_average) {
            filtered_samples[filtered_count++] = p_eend_dev->distance_info.rssi_samples[i];
        }
    }

    // 如果没有数据点满足条件，则保留原始数据
    if (filtered_count == 0) {
        log_i("[MEASDIST]No valid samples found, using original average\n");
        p_eend_dev->distance_info.rssi = -128;
        os_mutex_post(&p_eend_dev->distance_info.mutex);
        return;
    }

    // 重新计算平均RSSI
    sum = 0;
    for (int i = 0; i < filtered_count; ++i) {
        sum += filtered_samples[i];
    }
    p_eend_dev->distance_info.rssi = sum ? (s8)(sum / filtered_count) : -128;

    log_d("[MEASDIST]%s  Average RSSI: %d\n", sn_string, p_eend_dev->distance_info.rssi);
    os_mutex_post(&p_eend_dev->distance_info.mutex);
}

// 处理整个数组
static void process_all_dev_rssi_samples()
{
    eend_dev_traverse(process_rssi_samples, NULL);
}

// 清除所有RSSI样本
static void clean_all_dev_rssi_samples() 
{
    eend_dev_traverse(clear_rssi_samples, NULL);
}

void monitor_init(void)
{
    int ret = 0;
    
    lwl_app_context_t *ctx = lwl_app_core_context_get_instance();

    // 清空所有设备的RSSI样本
    eend_dev_traverse(clear_rssi_samples, NULL);
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
    // 用系统的发消息的函数，发送一个消息代表我的任务停止
    ret = __os_taskq_post("distance_main", Q_USER, argc, argv);
    if (ret == OS_TASK_NOT_EXIST) {
        log_i("[MEASDIST]the task is not exist\n");
    } else if (ret == OS_ERR_POST_NULL_PTR) {
        log_i("[MEASDIST]the message queue is not exist\n");
    } else {
        log_i("[MEASDIST]post success\n");
    }
    log_i("[MEASDIST]monitor_start : start measure the blue distance\n");
}

void monitor_stop(void)
{
    int ret = 0;
    argv[0] = MESSAGE_STOP;
    argv[1] = 12;
    // 用系统的发消息的函数，发送一个消息代表我的任务开始
    ret = __os_taskq_post("distance_main", Q_USER, argc, argv);
    if (ret == OS_TASK_NOT_EXIST) {
        log_i("[MEASDIST]the task is not exist\n");
    } else if (ret == OS_ERR_POST_NULL_PTR) {
        log_i("[MEASDIST]the message queue is not exist\n");
    } else {
        log_i("[MEASDIST]post success\n");
    }
    log_i("[MEASDIST]monitor_stop : stop measure the blue distance\n");
}

extern bool g_handshake_is_ongoing;

static void scan_rssi_record_cb(adv_report_t *report_pt, u16 len, void *priv)
{
#define FORTEST 0
#if FORTEST
    uint8_t buffer[32];
    memset(buffer, 0, sizeof(buffer));

    int r = ble_adv_report_get_data(report_pt->data, report_pt->length, HCI_EIR_DATATYPE_COMPLETE_LOCAL_NAME, buffer);
    if (r == -1) {
        return;
    }
    char *name = buffer;
    buffer[r] = '\0';

    log_i("%s %d", name, r);

    if (r != 7 || strcmp(name, "AIFANTA") != 0) {
        return;
    }
    char *sn = "WCCN24H28FEEEF7";
    s8 rssi = report_pt->rssi;
#else
    if (!adv_pre_match_impl(report_pt->data, report_pt->length)) {
        return;
    }

    s8 rssi = report_pt->rssi;
    
    /*
    关机广播格式：2(固定前缀) + 1(电量) + 16(序列号)
    开机广播格式：2(固定前缀) + 1(电量) + 16(序列号) + 6(绑定的C端MAC)
    */
    uint8_t buffer[32];
    int r = ble_adv_report_get_data(report_pt->data, report_pt->length, HCI_EIR_DATATYPE_MANUFACTURER_SPECIFIC_DATA, buffer);
    char *sn = (char *)buffer + 3;
    sn[15] = '\0'; // 确保字符串以 null 结尾
    uint8_t bat_level = buffer[2];
    uint8_t eend_power_state = (r == 25) ? 1 : 0;
#endif
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
    dev->bat_level = bat_level;
    dev->power_on_state = eend_power_state;
    if (dev->power_on_state) {
        memcpy(dev->bound_mac, buffer + 19, sizeof(dev->bound_mac));
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
    if (p_eend_dev == NULL) {
        return;
    }

    lwl_event_t e;
    e.event_type = LWL_EVENT_TYPE_EEND_DEV_CLOSEST_CHANGE;
    e.distance_change.pdev = p_eend_dev;
    lwl_event_notify(&e);
}

static void blue_distance_notify_self_process(lwl_eend_dev_t *p_eend_dev, void *priv)
{
    s8 rssi = p_eend_dev->distance_info.rssi;

    p_eend_dev->distance_info.distance_type_last = p_eend_dev->distance_info.distance_type;

    if (rssi >= BLUE_DISTANCE_TWO_METER && rssi != -128) {
        p_eend_dev->distance_info.distance_type = DISTANCE_0;
    } else if (rssi < BLUE_DISTANCE_TWO_METER && rssi >= BLUE_DISTANCE_FIVE_METER && rssi != -128) {
        p_eend_dev->distance_info.distance_type = DISTANCE_1;
    } else if (rssi < BLUE_DISTANCE_FIVE_METER && rssi != -128) {
        p_eend_dev->distance_info.distance_type = DISTANCE_2;
    } else {
        p_eend_dev->distance_info.distance_type = DISTANCE_3;
    }

    if (p_eend_dev->distance_info.distance_type != p_eend_dev->distance_info.distance_type_last) {
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

                process_all_dev_rssi_samples();
                blue_distance_notify_app();
                blue_distance_notify_self();
                clean_all_dev_rssi_samples();
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

late_initcall(creat_task_distance);
