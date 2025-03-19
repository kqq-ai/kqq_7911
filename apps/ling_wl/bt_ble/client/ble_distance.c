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

int current_ble_num = 0;
int argc = 2;
int argv[30] = {0};
int recv_argv[30] = {0};

OS_MUTEX g_mutex;

disatance_info_t moniter_ble_numbers[MAX_BLE_NUM];

// 声明 os_task_create 函数
int os_task_create(void (*func)(void *parm),
                   void *parm,
                   u8 prio,
                   u32 stk_size,
                   int q_size,
                   const char *name);

// extern int os_taskq_post(const char *name, int type, int argc, int *argv);
extern int __os_taskq_pend(int *argv, int argc, int tick);
extern int __os_taskq_post(const char *name, int type, int argc, int *argv);
extern u32 timer_get_ms(void);
extern int os_taskq_pend(const char *fmt, int *argv, int argc);

static void scan_rssi_record_cb(adv_report_t *report_pt, u16 len, void *priv);

// uint32_t distance_ble_count = sizeof(moniter_ble_numbers) / sizeof(moniter_ble_numbers[0]) -1;
void distance_info_init(disatance_info_t *info)
{
    info->p_eend_dev = NULL;
    info->average_rssi = -128;
    info->functions_rssi = -128;
    memset(info->rssi_samples, 0, sizeof(info->rssi_samples));
    info->sample_count = 0;
}

// 计算平均RSSI并过滤掉低于平均值一半的数据点
static void process_rssi(disatance_info_t *info)
{

    log_d("[MEASDIST]===process_rssi======\r\n");
    if (info->sample_count <= 0)
        return;
        
    char sn_string[SN_SIZE * 2 + 1] = "(NULL)";
    info->p_eend_dev->get_string(info->p_eend_dev, sn_string, sizeof(sn_string));
    log_d("[MEASDIST]===process_rssi   %s  sample_count : %d          ======\r\n",sn_string,info->sample_count);
    // 计算原始平均RSSI
    int sum = 0;
    for (int i = 0; i < info->sample_count; ++i)
    {
        sum += info->rssi_samples[i];
    }
    int original_average = sum / info->sample_count;
    log_d("[MEASDIST]original_average : %d\n",original_average);

    // 过滤数据点，创建临时数组存储符合条件的数据点
    s8 filtered_samples[MAX_SAMPLES];
    int filtered_count = 0;
    for (int i = 0; i < info->sample_count; ++i)
    {
        if (info->rssi_samples[i] >= original_average)
        {
            filtered_samples[filtered_count++] = info->rssi_samples[i];
        }
    }

    // 如果没有数据点满足条件，则保留原始数据
    if (filtered_count == 0)
    {
        log_i("[MEASDIST]No valid samples found, using original average\n");
        info->average_rssi = -128;
        info->functions_rssi = -128; // 或者使用其他逻辑来决定最大RSSI
        return;
    }

    // 重新计算平均RSSI
    sum = 0;
    for (int i = 0; i < filtered_count; ++i)
    {
        sum += filtered_samples[i];
    }
    info->average_rssi = sum ? (s8)(sum / filtered_count) : -128;
    info->functions_rssi = info->average_rssi;

    // 找到最大RSSI
    // info->functions_rssi = filtered_samples[0];
    // for (int i = 1; i < filtered_count; ++i)
    // {
    //     if (filtered_samples[i] > info->functions_rssi)
    //     {
    //         info->functions_rssi = filtered_samples[i];
    //     }
    // }
    log_d("[MEASDIST]%s  Average RSSI: %d, Function RSSI: %d\n", sn_string, info->average_rssi, info->functions_rssi);
}

static void clear_rssi_samples(disatance_info_t *info, size_t current_ble_num)
{
    for (size_t i = 0; i < current_ble_num; i++)
    {
        memset(info[i].rssi_samples, 0, sizeof(info[i].rssi_samples));
        info[i].sample_count = 0;
        info[i].average_rssi = -128;
        info[i].functions_rssi = -128;
    }
}

// 处理整个数组
static void process_distance_info_array(disatance_info_t *array, size_t current_ble_num)
{
    int ret = 0;
    ret = os_mutex_pend(&g_mutex, 0);
    if (0 != ret)
    {
        log_i("[MEASDIST]scan_rssi_record_cb : os_mutex_pend failed\n");
    }
    log_d("[MEASDIST]===process_distance_info_array======\r\n");
    for (size_t i = 0; i < current_ble_num; ++i)
    {
        log_d("[MEASDIST]===process_distance_info_array    %d======\r\n", i);
        process_rssi(&array[i]);
    }
    ret = os_mutex_post(&g_mutex);
    if (0 != ret)
    {
        log_i("[MEASDIST]scan_rssi_record_cb : os_mutex_post failed\n");
    }
}

void monitor_init(void)
{
    int ret = 0;
    for (int i = 0; i < MAX_BLE_NUM; i++)
    {
        distance_info_init(&moniter_ble_numbers[i]);
    }

    ble_adv_report_event_user_handler_add(scan_rssi_record_cb, NULL);
    // 创建互斥锁
    ret = os_mutex_create(&g_mutex);
    if (0 != ret)
    {
        log_i("[MEASDIST]create mutex failed\n");
    }

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
    log_i("[MEASDIST]monitor_stop : stop measure the blue distance\n");
}

void monitor_register_device(lwl_eend_dev_t *p_eend_dev)
{
    if (current_ble_num <= MAX_BLE_NUM)
    {
        if (p_eend_dev != NULL)
        {
            moniter_ble_numbers[current_ble_num++].p_eend_dev = p_eend_dev;
        }
        else
            log_i("[MEASDIST]monitor_register_device : The dev is NULL! \n");
    }
    else
        log_i("[MEASDIST]monitor_register_device : The ble number has reached its limit! \n");
}


static void scan_rssi_record_cb(adv_report_t *report_pt, u16 len, void *priv)
{
    int ret = 0;
    ret = os_mutex_pend(&g_mutex, 0);
    if (0 != ret)
    {
        log_i("[MEASDIST]scan_rssi_record_cb : os_mutex_pend failed\n");
    }

    if (!adv_pre_match_impl(report_pt->data, report_pt->length)) {
        ret = os_mutex_post(&g_mutex);
        return;
    }

    s8 rssi = report_pt->rssi;

    // log_d("[MEASDIST]scan_rssi_record_cb : %s, %d, %d\n", dev_name, dev_name_len, rssi);
    for (uint32_t i = 0; i < current_ble_num; ++i)
    {
        lwl_eend_dev_t *p_eend_dev = moniter_ble_numbers[i].p_eend_dev;
        if (p_eend_dev->adv_match(p_eend_dev, report_pt->data, report_pt->length))
        {
            // log_d("[MEASDIST]RSSI = %d, len = %d, name: %s", rssi, dev_name_len, dev_name);

            // Store the RSSI value
            moniter_ble_numbers[i].rssi_samples[moniter_ble_numbers[i].sample_count % MAX_SAMPLES] = rssi;
            if (moniter_ble_numbers[i].sample_count == MAX_SAMPLES)
            {
                moniter_ble_numbers[i].sample_count = 0;
            }
            moniter_ble_numbers[i].sample_count++;


            //rocess_rssi(&moniter_ble_numbers[i]);

        }
    }
    ret = os_mutex_post(&g_mutex);
    if (0 != ret)
    {
        log_i("[MEASDIST]scan_rssi_record_cb : os_mutex_post failed\n");
    }
}
static void scan_rssi_cb(void *priv)
{
    log_i("[MEASDIST]start scan RSSI call over\n");
}

__attribute__((weak)) void app_function(disatance_info_t *arg)
{
    log_i("[MEASDIST]notify_function\n");
}

__attribute__((weak)) void self_function(disatance_info_t *arg, int distance_type)
{
    log_i("[MEASDIST]self_function\n");
}

static void blue_distance_notify_app()
{
    int max_rssi = -128;
    int max_rssi_index = 0;
    // 判断哪一个蓝牙的functions_rssi值最大，然后通知该蓝牙do something
    for (int i = 0; i < current_ble_num; i++)
    {
        max_rssi = max_rssi > moniter_ble_numbers[i].functions_rssi ? max_rssi : moniter_ble_numbers[i].functions_rssi;
        if (moniter_ble_numbers[i].functions_rssi == max_rssi)
        {
            max_rssi_index = i;
        }
    }
    if (max_rssi == -128) {
        return;
    }
    for (int j = 0; j < current_ble_num; j++)
    {
        if (j == max_rssi_index)
        {
            app_function(&moniter_ble_numbers[max_rssi_index]);

            char sn_string[SN_SIZE * 2 + 1] = "(NULL)";
            lwl_eend_dev_t *p_eend_dev = moniter_ble_numbers[max_rssi_index].p_eend_dev;
            p_eend_dev->get_string(p_eend_dev, sn_string, sizeof(sn_string));
            log_i("[MEASDIST]ble index : %d --- ble name  : %s closer ! %d=========================\n", max_rssi_index, sn_string, moniter_ble_numbers[max_rssi_index].functions_rssi);
        }
    }
}

static void blue_distance_notify_self()
{
    for (int i = 0; i < current_ble_num; i++)
    {
        char sn_string[SN_SIZE * 2 + 1] = "(NULL)";
        lwl_eend_dev_t *p_eend_dev = moniter_ble_numbers[i].p_eend_dev;
        p_eend_dev->get_string(p_eend_dev, sn_string, sizeof(sn_string));
        
        log_i("[MEASDIST] %s: %d\r\n", sn_string, moniter_ble_numbers[i].average_rssi);

        if (moniter_ble_numbers[i].average_rssi >= BLUE_DISTANCE_TWO_METER && moniter_ble_numbers[i].average_rssi != -128)
        {
            self_function(&moniter_ble_numbers[i], DISTANCE_0);
            log_d("[MEASDIST] distance < 2m : do things1 here --- %s\r\n", sn_string);
        }
        else if (moniter_ble_numbers[i].average_rssi < BLUE_DISTANCE_TWO_METER &&
                 moniter_ble_numbers[i].average_rssi >= BLUE_DISTANCE_FIVE_METER &&
                 moniter_ble_numbers[i].average_rssi != -128)
        {
            self_function(&moniter_ble_numbers[i], DISTANCE_1);
            log_d("[MEASDIST] distance > 2m  < 5m do things2 here ---%s\r\n", sn_string);
        }
        else if (moniter_ble_numbers[i].average_rssi < BLUE_DISTANCE_FIVE_METER && moniter_ble_numbers[i].average_rssi != -128)
        {
            self_function(&moniter_ble_numbers[i], DISTANCE_2);
            log_d("[MEASDIST] distance > 5m do things3 here ---%s\r\n", sn_string);
        } 
        else {
            self_function(&moniter_ble_numbers[i], DISTANCE_3);
            log_d("[MEASDIST] not found ---%s\r\n", sn_string);
        }
    }
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
                process_distance_info_array(moniter_ble_numbers, current_ble_num);
                log_d("[MEASDIST]----------------the funtions_rssi is measured !-----------------\r\n");
                blue_distance_notify_app();
                blue_distance_notify_self();
                clear_rssi_samples(moniter_ble_numbers, current_ble_num);
                log_d("[MEASDIST]-------------------end measure ble rssi distance---------------\n");
                notify_app_time = current_time;
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
