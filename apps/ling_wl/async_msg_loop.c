#include "async_msg_loop.h"

#include "util/cqueue.h"
#include "net_music_play_my.h"
#include "os/os_api.h"
#include "cpu.h"
#include "bt_ble/app_wifi_manage.h"
#include "app_mode/lwl_eend_device.h"
#include "app_mode/lwl_app_core.h"
#include "bt_ble/client/ble_keep_alive.h"
#include "protocol/cp/c_uart_p.h"

enum
{
    MY_TASK_MESSAGE_SWITCH_PLAY,
    MY_TASK_MESSAGE_SWITCH_ONLY,
    MY_TASK_MESSAGE_PLAY_ONE,
    MY_TASK_MESSAGE_PLAY_ONE_LINGOS,
    MY_TASK_MESSAGE_PLAY_ONE_LFS,
    MY_TASK_MESSAGE_MAX = 0xFF,
};

#define AUDIO_URL_MAX_LEN 256
#define AUDIO_FNAME_MAX_LEN 256

typedef struct {
    char sn[SN_SIZE];
    pl_switch_cb_func_t cb;
    void *priv;
} MyMessageSwitchOnly;

typedef struct {
    char audio_url[AUDIO_URL_MAX_LEN];
    u8 audio_enable;
    pl_music_play_cb_func_t cb;
    void *priv;
} MyMessagePlayOne;

typedef struct {
    char audio_url[AUDIO_URL_MAX_LEN];
    u8 audio_enable;
    int audio_message_id;
    pl_music_play_cb_func_t cb;
    void *priv;
} MyMessagePlayOneLingOS;

typedef struct {
    char audio_fname[AUDIO_FNAME_MAX_LEN];
    u8 audio_enable;
    pl_music_play_cb_func_t cb;
    void *priv;
} MyMessagePlayOneLFS;

typedef struct
{
    int msg;
    union
    {
        MyMessageSwitchOnly switch_only;
        MyMessagePlayOne play_one;
        MyMessagePlayOneLingOS play_one_lingos;
        MyMessagePlayOneLFS play_one_lfs;
    };
} MyTaskMessageType;

static OS_SEM sem_net_music_dec_end;
static OS_SEM sem_eend_cmd_send_completed;

#define MY_TASK_MESSAGE_QUEUE_CAPACITY 10
static MyBlockingQueue g_message_queue; // MyBlockingQueue<MyTaskMessageType>

extern music_player g_music_player;

static void switch_only_sync(MyTaskMessageType *message);
static void play_one_sync(MyTaskMessageType *message);
static void play_one_lingos_sync(MyTaskMessageType *message);
static void play_one_lfs_sync(MyTaskMessageType *message);

static void my_task(void *priv)
{
    MyTaskMessageType message;

    os_sem_create(&sem_net_music_dec_end, 0);
    os_sem_create(&sem_eend_cmd_send_completed, 0);

    printf("Init message queue\n");
    my_blocking_queue_init(&g_message_queue, sizeof(MyTaskMessageType), MY_TASK_MESSAGE_QUEUE_CAPACITY);

    while (true)
    {
        /* 从消息队列取消息，若无消息则阻塞 */
        my_blocking_queue_dequeue(&g_message_queue, &message);

        printf("My task get message: %d\n", message.msg);

        switch (message.msg)
        {
        case MY_TASK_MESSAGE_SWITCH_ONLY:
            switch_only_sync(&message);
            break;
        case MY_TASK_MESSAGE_PLAY_ONE:
            play_one_sync(&message);
            break;
        case MY_TASK_MESSAGE_PLAY_ONE_LINGOS:
            play_one_lingos_sync(&message);
            break;
        case MY_TASK_MESSAGE_PLAY_ONE_LFS:
            play_one_lfs_sync(&message);
            break;
        }
    }
}

#include "bt_ble/client/ble_simple_client.h"

static void ble_start_scan_rssi(void *priv, void *cb)
{
    // TODO
    // struct ble_simple_client_operation_t *ops = ble_get_simple_client_operation_table();

    // ops->start_scan_rssi(priv, cb);

    printf("ble_start_scan_rssi\n");
    set_scan_adv_callback(cb, priv);
    ble_simple_client_module_enable(1);
}

static const u8 cmd_turn_off[] = {0xA0, 0x00};
static const u8 cmd_turn_on[] = {0xA0, 0x01};

static void let_eend_turn_off_cb(void *priv)
{
    u8 *cmd = (u8 *)priv;
    log_i("cmd send completed %02X %02X", cmd[0], cmd[1]);
    os_sem_post(&sem_eend_cmd_send_completed);
}
static void let_eend_turn_on_cb(void *priv)
{
    u8 *cmd = (u8 *)priv;
    log_i("cmd send completed %02X %02X", cmd[0], cmd[1]);
    os_sem_post(&sem_eend_cmd_send_completed);
}

static bool switch_only(char *sn, void (*timeout_cb)(int, void *), void *priv)
{
}

static void switch_only_sync(MyTaskMessageType *message)
{
    ASSERT(message->msg == MY_TASK_MESSAGE_SWITCH_ONLY);
    MyMessageSwitchOnly *param = &message->switch_only;

    /* 切换设备 */
    /* 设置C2广播 */
    uart_send_set_sn(param->sn);

    // /* 通知A关机 */
    // lwl_eend_dev_t *pprev = find_eend_dev_active();
    // if (pprev != NULL) {
    //     int r = ble_keep_alive_device_push_cmd(pprev, cmd_turn_off, sizeof(cmd_turn_off), let_eend_turn_off_cb, cmd_turn_off);
    //     /* 等待命令发送完毕 */
    //     if (os_sem_pend(&sem_eend_cmd_send_completed, 1000) == OS_TIMEOUT) {
    //         /* 通知A关机失败 */
    //         if (param->cb != NULL) {
    //             param->cb(SWITCH_ONLY_CODE_PREV_STOP_TIMEOUT, param->priv);
    //         }
    //         return;
    //     }
    // }
    pl_switch_info_t switch_info;
    switch_info.sn = param->sn;

    /* 通知B开机 */
    lwl_eend_dev_t *pnext = find_eend_dev_by_sn(param->sn);
    if (pnext != NULL) {
        int r = ble_keep_alive_device_push_cmd(pnext, cmd_turn_on, sizeof(cmd_turn_on), let_eend_turn_on_cb, cmd_turn_on);
        /* 等待命令发送完毕 */
        if (os_sem_pend(&sem_eend_cmd_send_completed, 200) == OS_TIMEOUT) {
            /* 通知B开机失败 */
            if (param->cb != NULL) {
                param->cb(SWITCH_ONLY_CODE_PREV_STOP_TIMEOUT, &switch_info, param->priv);
            }
            return;
        }
        /* 设置上下文 */
        set_eend_dev_active(pnext);
    } else { // pnext == NULL
        /* 设置上下文 */
        set_eend_dev_active(NULL);
    }

    /* 执行任务完成回调 */
    if (param->cb != NULL) {
        param->cb(SWITCH_ONLY_CODE_SUCCESS, &switch_info, param->priv);
    }
}

static void play_end_callback(void *priv)
{
    log_i("play end\n");
    os_sem_post(&sem_net_music_dec_end);
}


static void play_one_sync(MyTaskMessageType *message)
{
    ASSERT(message->msg == MY_TASK_MESSAGE_PLAY_ONE);
    MyMessagePlayOne *param = &message->play_one;

    pl_music_info_t music_info;
    music_info.name = param->audio_url;

    if (param->audio_enable) {
        /* 执行回调 */
        if (param->cb != NULL) param->cb(PLAY_AUDIO_STATE_BEFORE_DEC_START, &music_info, param->priv);

        /* 播放音频 */
        app_music_play_net_music_with_callback(&g_music_player, param->audio_url, play_end_callback, NULL);

        /* 等待播放结束 */
        os_sem_pend(&sem_net_music_dec_end, 0);
        
        /* 执行回调 */
        if (param->cb != NULL) param->cb(PLAY_AUDIO_STATE_AFTER_DEC_END, &music_info, param->priv);
    } else {
        /* 执行回调 */
        if (param->cb != NULL) param->cb(PLAY_AUDIO_STATE_CANCEL, &music_info, param->priv);
    }
}


static void play_one_lingos_sync(MyTaskMessageType *message)
{
    ASSERT(message->msg == MY_TASK_MESSAGE_PLAY_ONE_LINGOS);
    MyMessagePlayOneLingOS *param = &message->play_one_lingos;

    pl_music_info_t music_info;
    music_info.name = param->audio_url;

    if (param->audio_enable) {
        /* 执行回调 */
        if (param->cb != NULL) param->cb(PLAY_AUDIO_STATE_BEFORE_DEC_START, &music_info, param->priv);

        /* 播放音频 */
        app_music_play_net_music_with_callback(&g_music_player, param->audio_url, play_end_callback, NULL);

        /* 等待播放结束 */
        os_sem_pend(&sem_net_music_dec_end, 0);
        
        /* 执行回调 */
        if (param->cb != NULL) param->cb(PLAY_AUDIO_STATE_AFTER_DEC_END, &music_info, param->priv);
    } else {
        /* 执行回调 */
        if (param->cb != NULL) param->cb(PLAY_AUDIO_STATE_CANCEL, &music_info, param->priv);
    }
}

static void play_one_lfs_sync(MyTaskMessageType *message)
{
    ASSERT(message->msg == MY_TASK_MESSAGE_PLAY_ONE_LFS);
    MyMessagePlayOneLFS *param = &message->play_one;

    pl_music_info_t music_info;
    music_info.name = param->audio_fname;
    
    if (param->audio_enable) {
        /* 执行回调 */
        if (param->cb != NULL) param->cb(PLAY_AUDIO_STATE_BEFORE_DEC_START, &music_info, param->priv);

        /* 播放音频 */
        app_music_play_lfs_music_with_callback(&g_music_player, param->audio_fname, play_end_callback, NULL);

        /* 等待播放结束 */
        os_sem_pend(&sem_net_music_dec_end, 0);
        
        /* 执行回调 */
        if (param->cb != NULL) param->cb(PLAY_AUDIO_STATE_AFTER_DEC_END, &music_info, param->priv);
    } else {
        /* 执行回调 */
        if (param->cb != NULL) param->cb(PLAY_AUDIO_STATE_CANCEL, &music_info, param->priv);
    }
}

/**
 * 打印字符串，蓝牙广播名可能包含\0字符（...）
 */
void printf_dev_name(char *name, u16 len)
{
    for (int i = 0; i < len; ++i) {
        printf("%c", name[i]);
    }
}

/**
 * @name my_task_switch_only
 * @brief This function is used to create a specific task message and enqueue it into a message queue.
 *
 * @param name    Pointer to a character string indicating the E-end sequence number.
 * @param cb      A pointer to a callback function, see SWITCH_ONLY_CODE_.
 * @param priv    A pointer to user-defined private data.
 */
void my_task_switch_only(char *sn, pl_switch_cb_func_t *cb, void *priv)
{
    /* 创建消息 */
    MyTaskMessageType message;
    message.msg = MY_TASK_MESSAGE_SWITCH_ONLY;
    memcpy(message.switch_only.sn, sn, SN_SIZE);
    message.switch_only.priv = priv;
    message.switch_only.cb = cb;
    printf("Put message: %d\n", message.msg);
    printf("sn         : ");
    printf_dev_name(message.switch_only.sn, SN_SIZE);
    printf("\n");
    /* 消息入队 */
    my_blocking_queue_enqueue(&g_message_queue, &message);
}

/**
 * @name my_task_play_one
 * @brief This function is used to create a specific task message and enqueue it into a message queue.
 *
 * @param audio_url    Pointer to a character string indicating the audio source URL.
 * @param cb      A pointer to a callback function.
 * @param priv    A pointer to user-defined private data.
 */
void my_task_play_one(char* audio_url, pl_music_play_cb_func_t cb, void *priv)
{
    /* 创建消息 */
    MyTaskMessageType message;
    message.msg = MY_TASK_MESSAGE_PLAY_ONE;
    strcpy(message.play_one.audio_url, audio_url);
    message.play_one.audio_enable = true;
    message.play_one.priv = priv;
    message.play_one.cb = cb;
    printf("Put message: %d\n", message.msg);
    printf("audio_url  : %s\n", message.play_one.audio_url);
    /* 消息入队 */
    my_blocking_queue_enqueue(&g_message_queue, &message);
}

/**
 * @name my_task_play_one_lingos
 * @brief This function is used to create a specific task message and enqueue it into a message queue.
 *
 * @param audio_messageId Audio messsage id (LingOS).
 * @param audio_url    Pointer to a character string indicating the audio source URL.
 * @param cb      A pointer to a callback function.
 * @param priv    A pointer to user-defined private data.
 */
void my_task_play_one_lingos(int audio_message_id, char *audio_url, pl_music_play_cb_func_t cb, void *priv)
{
    /* 创建消息 */
    MyTaskMessageType message;
    message.msg = MY_TASK_MESSAGE_PLAY_ONE_LINGOS;
    strcpy(message.play_one_lingos.audio_url, audio_url);
    message.play_one_lingos.audio_enable = true;
    message.play_one_lingos.audio_message_id = audio_message_id;
    message.play_one_lingos.priv = priv;
    message.play_one_lingos.cb = cb;
    printf("Put message: %d\n", message.msg);
    printf("audio_url  : %s\n", message.play_one.audio_url);
    /* 消息入队 */
    my_blocking_queue_enqueue(&g_message_queue, &message);
}

/**
 * @name my_task_play_one_lfs
 * @brief This function is used to create a specific task message and enqueue it into a message queue.
 *
 * @param fname    Pointer to a character string indicating the audio file name.
 * @param priv    A pointer to user-defined private data.
 * @param cb      A pointer to a callback function.
 */
void my_task_play_one_lfs(char *fname, pl_music_play_cb_func_t cb, void *priv)
{
    /* 创建消息 */
    MyTaskMessageType message;
    message.msg = MY_TASK_MESSAGE_PLAY_ONE_LFS;
    strcpy(message.play_one_lfs.audio_fname, fname);
    message.play_one_lfs.audio_enable = true;
    message.play_one_lfs.priv = priv;
    message.play_one_lfs.cb = cb;
    printf("Put message: %d\n", message.msg);
    printf("audio_fname: %s\n", message.play_one_lfs.audio_fname);
    /* 消息入队 */
    my_blocking_queue_enqueue(&g_message_queue, &message);
}

static void audio_message_cancellation_cb(void *data, void *priv)
{
    /* 取消特定audio_only消息的播放 */
    int audio_message_id = (int)priv;
    MyTaskMessageType *pMessage = (MyTaskMessageType *)data;
    if (pMessage->msg == MY_TASK_MESSAGE_PLAY_ONE_LINGOS) {
        if (pMessage->play_one_lingos.audio_message_id == audio_message_id) {
            pMessage->play_one_lingos.audio_enable = false;
        }
    }
}

/**
 * @name disable_specific_audio_message
 * @brief Cancel the audio message with a specific audio message ID.
 *
 * @param audio_messageId Audio messsage id (LingOS).
 */
void disable_specific_audio_message(int audio_message_id)
{
    my_blocking_queue_traverse(&g_message_queue, audio_message_cancellation_cb, (void *)audio_message_id);
}

static void audio_message_cancellation_all_cb(void *data, void *priv)
{
    /* 取消特定audio_only消息的播放 */
    MyTaskMessageType *pMessage = (MyTaskMessageType *)data;
    switch (pMessage->msg) {
    case MY_TASK_MESSAGE_PLAY_ONE:
        pMessage->play_one.audio_enable = false;
        break;
    case MY_TASK_MESSAGE_PLAY_ONE_LINGOS:
        pMessage->play_one_lingos.audio_enable = false;
        break;
    case MY_TASK_MESSAGE_PLAY_ONE_LFS:
        pMessage->play_one_lfs.audio_enable = false;
        break;
    }
}

/**
 * @name disable_all_audio_message
 * @brief Cancel all of the audio message.
 */
void disable_all_audio_message()
{
    my_blocking_queue_traverse(&g_message_queue, audio_message_cancellation_all_cb, NULL);
}

void start_async_msg_loop()
{
    printf("create my_task\n");
    if (thread_fork("my_task", 10, 1024, 0, NULL, my_task, NULL) != OS_NO_ERR)
    {
        printf("thread fork fail\n");
    }
}
