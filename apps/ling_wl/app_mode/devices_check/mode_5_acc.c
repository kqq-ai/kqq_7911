#include "mode_list.h"

#include "../../LED/LED.h"

static void set_led_red(void)        { set_rgb_color(COLOR_RED); }
static void set_led_green(void)      { set_rgb_color(COLOR_GREEN); }
static void set_led_blue(void)       { set_rgb_color(COLOR_BLUE); }

// 模式5 加速度传感器相关变量
extern void *g_iic_hdl;                            // 外部IIC句柄声明（在app_sc7a20_task.c中定义）
static void *g_sc7a20_iic_hdl = NULL;              // SC7A20 IIC设备句柄
static u8 g_mode5_gsensor_enabled = 0;             // 模式5 G-Sensor是否已启用
static s16 g_accel_x = 0, g_accel_y = 0, g_accel_z = 0;  // 三轴加速度值
static s16 g_accel_x_prev = 0, g_accel_y_prev = 0, g_accel_z_prev = 0;  // 前一次三轴加速度值
static u32 g_motion_count = 0;                 // 运动检测计数
static u8 g_motion_detected = 0;               // 运动检测状态 (0=静止, 1=小幅晃动, 2=大幅晃动)
static u32 g_small_motion_count = 0;           // 小幅晃动计数
static u32 g_large_motion_count = 0;           // 大幅晃动计数

// 模式5 加速度传感器功能声明
static void mode5_gsensor_init(void);
static void mode5_gsensor_uninit(void);
static int mode5_gsensor_read_data(void);
static void mode5_gsensor_status_update(void);
static void mode5_motion_detection(void);
// 模式5处理函数：加速度传感器测试模式
void process_mode_5(void)
{
    // 如果G-Sensor功能已启用，则监控传感器状态
    if (g_mode5_gsensor_enabled) {
        mode5_gsensor_status_update();
        mode5_motion_detection();
    }
    
    // 如果提示音还在播放中，则等待播放完成
    if (g_audio_playing) {
        // 等待音频播放完成，无需特殊处理
        return;
    }
}

// 模式5音频播放回调函数
void mode5_audio_callback(void *priv)
{
    // 检查当前模式，如果不是模式5，则忽略回调
    if (get_current_mode() != WORK_MODE_5) {
        log_i("MODE 5: Audio callback ignored - not in mode 5 (current mode: %d)", get_current_mode());
        return;
    }
    
    log_i("MODE 5: Prompt audio callback - finished, starting G-Sensor monitoring");
    g_audio_playing = 0;
}

//=================================================================================================
// 模式5 G-Sensor功能相关函数
//=================================================================================================
/**
 * @brief 启动G-Sensor功能
 * @return 0成功, -1失败
 */
static void mode5_gsensor_init(void)
{
    
    
    set_led_green();  // 初始绿灯表示G-Sensor模式（静止状态）
    g_audio_playing = 1;  // 设置为1，表示正在播放SDcardMusic
    g_mode5_gsensor_enabled = 0;  // G-Sensor尚未启用
    g_motion_detected = 0;  // 重置运动检测状态
    g_motion_count = 0;     // 重置运动计数
    g_small_motion_count = 0; // 重置小幅晃动计数
    g_large_motion_count = 0; // 重置大幅晃动计数
    log_i("MODE 5: G-Sensor accelerometer test mode started");

    // 获取外部已初始化的SC7A20 IIC句柄
    if (!g_iic_hdl) {
        log_e("[MODE 5 GSENSOR] SC7A20 IIC handle not initialized");
        return;
    }

    // 播放进入模式5的音频，使用回调
    lwl_app_mode_play(CONFIG_CP_VOICE_FILE_PATH"test_gyro.mp3", mode5_audio_callback);
    log_i("Playing test_gyro.mp3 for MODE 5 with callback");

    
    
    g_sc7a20_iic_hdl = g_iic_hdl;
    
    // 初始化变量
    g_accel_x = g_accel_y = g_accel_z = 0;
    g_accel_x_prev = g_accel_y_prev = g_accel_z_prev = 0;

    g_mode5_gsensor_enabled = 1;
    
    log_i("[MODE 5 GSENSOR] SC7A20 G-Sensor functionality initialized and started");
    return;
}

/**
 * @brief 停止G-Sensor功能
 */
static void mode5_gsensor_uninit(void)
{
    if (g_sc7a20_iic_hdl) {
        g_sc7a20_iic_hdl = NULL;
        log_i("[MODE 5 GSENSOR] SC7A20 G-Sensor functionality stopped");
    }
    
    g_mode5_gsensor_enabled = 0;
    g_motion_detected = 0;
    g_motion_count = 0;
    g_small_motion_count = 0;
    g_large_motion_count = 0;
}

/**
 * @brief 读取G-Sensor数据
 * @return 0成功, -1失败
 */
static int mode5_gsensor_read_data(void)
{
    if (!g_sc7a20_iic_hdl) {
        return -1;
    }
    
    // 保存前一次的值
    g_accel_x_prev = g_accel_x;
    g_accel_y_prev = g_accel_y;
    g_accel_z_prev = g_accel_z;
    
    // 使用SC7A20接口读取真实的加速度数据
    if (read_accel_data(g_sc7a20_iic_hdl, &g_accel_x, &g_accel_y, &g_accel_z) != 0) {
        // 如果读取失败，返回错误
        return -1;
    }
    
    return 0;
}

/**
 * @brief 检测运动状态
 */
static void mode5_motion_detection(void)
{
    // 定义运动检测阈值
    #define SMALL_MOTION_THRESHOLD 350   // 小幅晃动阈值（降低以便检测）
    #define LARGE_MOTION_THRESHOLD 6000   // 大幅晃动阈值（保持较高）
    
    // 计算三轴加速度变化
    s16 delta_x = g_accel_x - g_accel_x_prev;
    s16 delta_y = g_accel_y - g_accel_y_prev;
    s16 delta_z = g_accel_z - g_accel_z_prev;
    
    // 计算总变化量
    u32 total_delta = (u32)(abs(delta_x) + abs(delta_y) + abs(delta_z));
    
    u8 motion_now = 0;  // 0=静止, 1=小幅晃动, 2=大幅晃动
    
    if (total_delta >= LARGE_MOTION_THRESHOLD) {
        motion_now = 2;  // 大幅晃动
    } else if (total_delta >= SMALL_MOTION_THRESHOLD) {
        motion_now = 1;  // 小幅晃动
    } else {
        motion_now = 0;  // 静止
    }
    
    // 检测运动状态变化
    if (motion_now != g_motion_detected) {
        if (motion_now == 2) {
            // 检测到大幅晃动
            g_large_motion_count++;
            g_motion_count++;
            set_led_red();  // 大幅晃动时红灯
            log_i("[MODE 5 GSENSOR] === LARGE MOTION DETECTED === (#%d total, #%d large)", 
                  g_motion_count, g_large_motion_count);
            log_i("[MODE 5 GSENSOR] Delta: X=%d, Y=%d, Z=%d, Total=%d (LARGE)", 
                  delta_x, delta_y, delta_z, total_delta);
            
            // 打印大幅运动开始的分隔符
            for (int i = 0; i < 5; i++) {
                printf(">>> LARGE MOTION START <<<\r\n");
            }
        } else if (motion_now == 1) {
            // 检测到小幅晃动
            g_small_motion_count++;
            g_motion_count++;
            set_led_blue();  // 小幅晃动时蓝灯
            log_i("[MODE 5 GSENSOR] === SMALL MOTION DETECTED === (#%d total, #%d small)", 
                  g_motion_count, g_small_motion_count);
            log_i("[MODE 5 GSENSOR] Delta: X=%d, Y=%d, Z=%d, Total=%d (SMALL)", 
                  delta_x, delta_y, delta_z, total_delta);
            
            // 打印小幅运动开始的分隔符
            for (int i = 0; i < 3; i++) {
                printf(">>> SMALL MOTION START <<<\r\n");
            }
        } else {
            // 运动停止
            set_led_green();  // 静止时绿灯
            if (g_motion_detected == 2) {
                log_i("[MODE 5 GSENSOR] === LARGE MOTION STOPPED ===");
                // 打印大幅运动结束的分隔符
                for (int i = 0; i < 5; i++) {
                    printf(">>> LARGE MOTION STOP <<<\r\n");
                }
            } else if (g_motion_detected == 1) {
                log_i("[MODE 5 GSENSOR] === SMALL MOTION STOPPED ===");
                // 打印小幅运动结束的分隔符
                for (int i = 0; i < 3; i++) {
                    printf(">>> SMALL MOTION STOP <<<\r\n");
                }
            }
        }
        g_motion_detected = motion_now;
    }
}

/**
 * @brief 更新G-Sensor状态信息并打印
 */
static void mode5_gsensor_status_update(void)
{
    static u32 last_print_time = 0;
    static u32 last_data_read_time = 0;
    static u32 read_error_count = 0;
    u32 current_time = timer_get_ms();
    
    // 每50ms读取一次传感器数据
    if (current_time - last_data_read_time >= 50) {
        if (mode5_gsensor_read_data() != 0) {
            read_error_count++;
            // 如果连续读取失败次数过多，打印错误信息
            if (read_error_count % 20 == 1) {  // 每20次失败打印一次错误
                log_e("[MODE 5 GSENSOR] Failed to read sensor data (error count: %d)", read_error_count);
            }
        } else {
            // 读取成功，重置错误计数
            read_error_count = 0;
        }
        last_data_read_time = current_time;
    }
    
    // 每0.5秒(500ms)打印一次陀螺仪状态信息
    if (current_time - last_print_time >= 500) {
        const char *motion_status;
        if (g_motion_detected == 2) {
            motion_status = "LARGE_MOTION";
        } else if (g_motion_detected == 1) {
            motion_status = "SMALL_MOTION";
        } else {
            motion_status = "STATIC";
        }
        
        log_i("[MODE 5 GSENSOR] Gyroscope Status: X=%d, Y=%d, Z=%d", g_accel_x, g_accel_y, g_accel_z);
        log_i("[MODE 5 GSENSOR] Motion: %s, Total: %d, Small: %d, Large: %d, Errors: %d", 
              motion_status, g_motion_count, g_small_motion_count, g_large_motion_count, read_error_count);
        printf("[GYROSCOPE] X:%d Y:%d Z:%d Motion:%s Total:%d Small:%d Large:%d Errors:%d\r\n", 
               g_accel_x, g_accel_y, g_accel_z, motion_status, 
               g_motion_count, g_small_motion_count, g_large_motion_count, read_error_count);
        last_print_time = current_time;
    }
}


void mode5_init()
{
    mode5_gsensor_init();
}

void mode5_uninit()
{
    mode5_gsensor_uninit();
}