#ifndef __MODE_LIST_H__
#define __MODE_LIST_H__
/**
 * @author luotianyu
 * @brief 
 *   +----> IDLE
 *   |
 * POWER_ON -------> INIT -------> MODE_1 -------> MODE_2 -------> MODE_3 -------> MODE_4 -------> MODE_5 -------> MODE_6 -------> MODE_7 -------> MODE_8(串行状态)
 *   ^                                                                                                                                               |
 *   |-----------------------------------------------------------------------------------------------------------------------------------------------+
 * 
 * 模式说明：
 * INIT         初始模式（绿灯常亮）
 * MODE_1       触摸按键检测
 * MODE_2       所有颜色循环闪烁
 * MODE_3       循环播放音频（C端播放）
 * MODE_4       麦克风VAD检测
 * MODE_5       加速度传感器测试
 * MODE_6       串行状态机（E端设备音频循环播放，含多子状态）
 * MODE_7       联网测试
 * MODE_8       获取SN号，进入时播放test_snkey.mp3
 * 
 * 按键短按依次切换模式，MODE_8后回到INIT，循环。
 */

#include "app_core.h"

#include "../../bt_ble/client/ble_distance.h"
#include "../lwl_app_framework.h"
#include "../../protocol/odm/odm_secure.h"
#include "../../include/app_config.h"

// 定义工作模式枚举
typedef enum {
    WORK_MODE_INIT = 0,     // 初始模式（绿灯常亮）
    WORK_MODE_1,            // 模式1（触摸按键检测）
    WORK_MODE_2,            // 模式2（所有颜色循环闪烁）
    WORK_MODE_3,            // 模式3（循环播放音频 - C端播放）
    WORK_MODE_4,            // 模式4（麦克风VAD检测）
    WORK_MODE_5,            // 模式5（加速度传感器测试）
    WORK_MODE_6,            // 模式6（循环播放音频 - E端播放）
    WORK_MODE_7,            // 模式7（联网测试）
    WORK_MODE_8,            // 模式8（获取SN号，进入时播放test_snkey.mp3）
    WORK_MODE_MAX           // 模式总数
} work_mode_e;


#define MAX_PDEV_LIST 16

// 全局变量
extern u8 g_audio_playing;
extern u8 g_mode_switching;      // 模式切换锁定标志，防止重复切换导致问题
extern lwl_eend_dev_t* pdev_list[MAX_PDEV_LIST];
extern int pdev_list_count;


work_mode_e get_current_mode();

// 模式切换函数声明
void mode1_init();
void mode2_init();
void mode3_init();
void mode4_init();
void mode5_init();
void mode6_init();
void mode7_init();
void mode8_init();

void mode1_uninit();
void mode2_uninit();
void mode3_uninit();
void mode4_uninit();
void mode5_uninit();
void mode6_uninit();
void mode7_uninit();
void mode8_uninit();

void process_mode_init(void);
void process_mode_1(void);
void process_mode_2(void);
void process_mode_3(void);
void process_mode_4(void);
void process_mode_5(void);
void process_mode_6(void);
void process_mode_7(void);
void process_mode_8(void);

void check_key_status(void);

void process_mode_1_touch_key(u8 touch_key_event_type);
void process_mode_8_touch_key(u8 touch_key_event_type);
bool mode7_is_wifi_connected();

// 模式处理函数数组
static void (*mode_process_functions[])(void) = {
    process_mode_init,
    process_mode_1,
    process_mode_2,
    process_mode_3,
    process_mode_4,
    process_mode_5,
    process_mode_6,
    process_mode_7,
    process_mode_8,
};

// 模式名称数组（用于日志）
static const char *mode_names[] = {
    "INIT",
    "MODE_1_TOUCH_KEY",
    "MODE_2_ALL_COLORS",
    "MODE_3_AUDIO_LOOP_C",
    "MODE_4_MIC_VAD",
    "MODE_5_GSENSOR_TEST",
    "MODE_6_AUDIO_LOOP_E",
    "MODE_7_WIFI_CONNECT",
    "MODE_8_SNKEY",
};

#endif