#include "lwl_app_framework.h"

#include "cpu.h"
#include "log.h"
#include "app_mode/lwl_app_core.h"

#include "power_on/power_on.h"
#include "app_core.h"
#include "os/os_api.h"
#include "gpio.h"
#include "app_cookie.h"

static u8 app_prev_mode = 0;
static u8 app_curr_mode = 0;
static u8 app_next_mode = 0;
static u8 mode_exitting = 0;
static OS_MUTEX g_mode_mutex;

static bool play_voice_prompt = 0;

static void lwl_app_mode_play_cb(void *priv)
{
    os_mutex_pend(&g_mode_mutex, 0);
    play_voice_prompt = 0;
#ifndef C_SPEAKLER
    gpio_direction_output(IO_PORTA_00, 0);  //Disable C speaker
#endif
    os_mutex_post(&g_mode_mutex);
}

void lwl_app_mode_play(const char *fname) 
{
    os_mutex_pend(&g_mode_mutex, 0);
    log_i("play: %s", fname);
    gpio_direction_output(IO_PORTA_00, 1);  //Enable C speaker
    lwl_app_context_t *context = lwl_app_core_context_get_instance();
    int err = app_music_play_local_music_with_callback(context->music_player, fname, lwl_app_mode_play_cb, NULL);
    if (!err) {
        play_voice_prompt = 1;
    }
    os_mutex_post(&g_mode_mutex);
}

//*----------------------------------------------------------------------------*/
/**@brief    模式退出检查
   @param    curr_task:当前模式
   @return   TRUE可以退出， FALSE不可以退出
   @note
*/
/*----------------------------------------------------------------------------*/
static int lwl_app_mode_switch_exit_check(u8 curr_task)
{
    int ret = false;
    switch (curr_task) {
    case APP_DEVICE_CHECK_MODE:
        ret = false;
        break;
    default:
        ret = TRUE;
        break;

    }
    return ret;
}
//*----------------------------------------------------------------------------*/
/**@brief    模式进入检查
   @param    app_mode:目标模式
   @return   TRUE可以进入， FALSE不可以进入
   @note	 例如一些需要设备在线的任务(music/record/linein等)，
   			 如果设备在线可以进入， 没有设备在线不进入可以在这里处理
*/
/*----------------------------------------------------------------------------*/
static int lwl_app_mode_switch_check(u8 app_mode)
{
    int ret = false;
    switch (app_mode) {
    case APP_DEVICE_CHECK_MODE:
        ret = false;
        break;
    default:
        ret = TRUE;
        break;

    }
    return ret;
}
//*----------------------------------------------------------------------------*/
/**@brief    切换到指定模式
   @param    app_mode:指定模式
   @return
   @note
*/
/*----------------------------------------------------------------------------*/
int lwl_app_mode_switch_to(u8 app_mode)
{
    os_mutex_pend(&g_mode_mutex, 0);

    if (app_curr_mode == app_mode) {
        os_mutex_post(&g_mode_mutex);
        return false;
    }

    if (!lwl_app_mode_switch_check(app_mode)) {
        os_mutex_post(&g_mode_mutex);
        return false;
    }

    if (!lwl_app_mode_switch_exit_check(app_curr_mode)) {
        os_mutex_post(&g_mode_mutex);
        return false;
    }

    printf("cur --- %x \n", app_curr_mode);
    printf("new +++ %x \n", app_mode);

    app_prev_mode = app_curr_mode;
    app_next_mode = app_mode;

    os_mutex_post(&g_mode_mutex);
    return TRUE;
}

//*----------------------------------------------------------------------------*/
/**@brief    跳回到原来的模式
   @param
   @return
   @note
*/
/*----------------------------------------------------------------------------*/
int lwl_app_mode_switch_back()
{
    if (app_prev_mode == 0) {
        return -EINVAL;
    }
    return lwl_app_mode_switch_to(app_prev_mode);
}

//*----------------------------------------------------------------------------*/
/**@brief    模式切换退出检测
   @param
   @return   1:响应退出模式， 0:不响应
   @note
*/
/*----------------------------------------------------------------------------*/
u8 lwl_app_mode_exitting()//
{
    if (app_next_mode && !mode_exitting) {
        /* app_curr_mode = app_next_mode; */
        /* app_next_mode = 0; */
        while (play_voice_prompt) {
            msleep(10);
        }
        mode_exitting = 1;
        return 1;
    }
    return 0;
}

/* --------------------------------------------------------------------------*/
/**
 * @brief 模式退出完成
 */
/* ----------------------------------------------------------------------------*/
void lwl_app_mode_exit_ok()
{
    if (app_next_mode) {
        app_curr_mode = app_next_mode;
        app_next_mode = 0;
        mode_exitting = 0;
    }
}

void lwl_app_init()
{
    os_mutex_create(&g_mode_mutex);
    lwl_app_context_t *context = lwl_app_core_context_get_instance();
    char sn[25];
    get_SN(sn);
    if (sn[0] == 0) {
        log_i("no sn, go to device check mode");
        app_curr_mode = APP_DEVICE_CHECK_MODE;
    } else {
        log_i("have sn, go to power on mode");
        app_curr_mode = APP_POWER_ON_MODE;
    }
    play_voice_prompt = 0;
}

void lwl_app_mode_loop()
{
    switch(app_curr_mode) {
    case APP_POWER_ON_MODE:
        log_i("APP_POWER_ON_MODE\n");
        app_power_on_mode();
        lwl_app_mode_exit_ok();
        break;
    case APP_DEFAULT_MODE:
        log_i("APP_DEFAULT_MODE\n");
        app_default_mode();
        lwl_app_mode_exit_ok();
        break;
    case APP_NET_CONNECTING_MODE:
        log_i("APP_NET_CONNECTING_MODE\n");
        app_net_connecting_mode();
        lwl_app_mode_exit_ok();
        break;
    case APP_NET_CONNECTED_MODE:
        log_i("APP_NET_CONNECTED_MODE\n");
        app_net_connected_mode();
        lwl_app_mode_exit_ok();
        break;
    case APP_UPDATE_MODE:
        log_i("APP_UPDATE_MODE\n");
        app_update_mode();
        lwl_app_mode_exit_ok();
    case APP_WSS_CONNECTED_MODE:
        log_i("APP_WSS_CONNECTED_MODE\n");
        app_wss_connected_mode();
        lwl_app_mode_exit_ok();
        break;
    case APP_DEVICE_CHECK_MODE:
        log_i("APP_DEVICE_CHECK_MODE\n");
        app_devices_check_mode();
        lwl_app_mode_exit_ok();
        break;
    }
}

// 在释放时删除互斥锁
void lwl_app_uninit()
{
    os_mutex_del(&g_mode_mutex, 1);
}

