#include "async_msg_loop.h"

#include "util/cqueue.h"
#include "net_music_play_my.h"
#include "os/os_api.h"
#include "cpu.h"

enum {
    MY_TASK_MESSAGE_SWITCH_PLAY,
    MY_TASK_MESSAGE_SWITCH_ONLY,
    MY_TASK_MESSAGE_PLAY_ONE,
    MY_TASK_MESSAGE_MONITER_RSSI,

    MY_TASK_MESSAGE_MAX = 0xFF,
};

#define SN_MAX_SIZE 16
#define AUDIO_URL_MAX_LEN 256

typedef struct {
    char sn[SN_MAX_SIZE];
    char audio_url[AUDIO_URL_MAX_LEN];
    void *priv;
    void *cb;
} MyMessageSwitchPlay;

typedef struct {
    char sn[SN_MAX_SIZE];
    void *priv;
    void *cb;
} MyMessageSwitchOnly;

typedef struct {
    char audio_url[AUDIO_URL_MAX_LEN];
    u8   audio_enable;
    int  audio_message_id;
    void *priv;
    void *cb;
} MyMessagePlayOne;

typedef struct {
    void *rssi_cb_priv;
    void (*rssi_cb)(void *priv, char *dev_name, u16 dev_name_len, s8 rssi);
    void *priv;
    void *cb;
} MyMessageMoniterRSSI;

typedef struct {
    int msg;
    union {
        MyMessageSwitchPlay switch_play;
        MyMessageSwitchOnly switch_only;
        MyMessagePlayOne play_one;
        MyMessageMoniterRSSI moniter_rssi;
    };
} MyTaskMessageType;


int g_playing_audio_message_id = 0;

static OS_SEM sem_net_music_dec_end;

#define MY_TASK_MESSAGE_QUEUE_CAPACITY 10
static MyBlockingQueue g_message_queue; // MyBlockingQueue<MyTaskMessageType>

static uint8_t uart_tx_buf[1 + SN_MAX_SIZE];

extern void *uart0_hdl;
extern music_player g_music_player;

static void switch_only_sync(MyTaskMessageType *message);
static void switch_play_sync(MyTaskMessageType *message);
static void play_one_sync(MyTaskMessageType *message);
static void moniter_rssi_sync(MyTaskMessageType *message);

static void my_task(void *priv)
{
    MyTaskMessageType message;
    
    os_sem_create(&sem_net_music_dec_end, 0);

    printf("Init message queue\n");
    my_blocking_queue_init(&g_message_queue, sizeof(MyTaskMessageType), MY_TASK_MESSAGE_QUEUE_CAPACITY);

    while (true)
    {
        /* 从消息队列取消息，若无消息则阻塞 */
        my_blocking_queue_dequeue(&g_message_queue, &message);

        printf("My task get message: %d\n", message.msg);

        switch (message.msg) {
        case MY_TASK_MESSAGE_SWITCH_PLAY:
            switch_play_sync(&message);
            break;
        case MY_TASK_MESSAGE_SWITCH_ONLY:
            switch_only_sync(&message);
            break;
        case MY_TASK_MESSAGE_PLAY_ONE:
            play_one_sync(&message);
            break;
        case MY_TASK_MESSAGE_MONITER_RSSI:
            moniter_rssi_sync(&message);
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

static void switch_only(char *sn)
{
    int i = 0;
    uart_tx_buf[i++] = 0xA0;
    memcpy(uart_tx_buf + i, sn, SN_MAX_SIZE);
    i += SN_MAX_SIZE;
    dev_write(uart0_hdl, uart_tx_buf, i);
}

static void switch_only_sync(MyTaskMessageType *message)
{
    ASSERT(message->msg == MY_TASK_MESSAGE_SWITCH_ONLY);
    MyMessageSwitchOnly *param = &message->switch_only;

    /* 切换设备 */
    switch_only(param->sn);

    /* 执行任务完成回调 */
    if (param->cb != NULL) {
        ((void (*)(void* priv))param->cb)(param->priv);
    }
}

static void play_end_callback(void *priv)
{
    log_i("play end\n");
    os_sem_post(&sem_net_music_dec_end);
}


static void switch_play_sync(MyTaskMessageType *message)
{
    log_i("switch_play_sync");
    ASSERT(message->msg == MY_TASK_MESSAGE_SWITCH_PLAY);
    log_i("A");
    MyMessageSwitchPlay *param = &message->switch_play;
    /* 切换设备 */
    log_i("B");
    switch_only(param->sn);

    log_i("C");
    /* 播放音频 */
    app_music_play_net_music_with_callback(&g_music_player, param->audio_url, play_end_callback, NULL);

    log_i("D");
    /* 等待播放结束 */
    os_sem_pend(&sem_net_music_dec_end, 0);

    log_i("E");
    /* 执行任务完成回调 */
    if (param->cb != NULL) {
        ((void (*)(void* priv))param->cb)(param->priv);
    }
}

#include "app_mode/lwl_app_core.h"

static void play_one_sync(MyTaskMessageType *message)
{
    ASSERT(message->msg == MY_TASK_MESSAGE_PLAY_ONE);
    MyMessagePlayOne *param = &message->play_one;

    if (param->audio_enable) {
        /* 记录当前播放的音频message_id */
        g_playing_audio_message_id = param->audio_message_id;

        /* 播放音频 */
        app_music_play_net_music_with_callback(&g_music_player, param->audio_url, play_end_callback, NULL);

        /* 等待播放结束 */
        os_sem_pend(&sem_net_music_dec_end, 0);
    }

    /* 执行任务完成回调 */
    if (param->cb != NULL) {
        ((void (*)(void* priv))param->cb)(param->priv);
    }
}

static void moniter_rssi_sync(MyTaskMessageType *message)
{
    ASSERT(message->msg == MY_TASK_MESSAGE_MONITER_RSSI);
    MyMessageMoniterRSSI *param = &message->moniter_rssi;

    /* 使能BLE SCAN 获取设备RSSI */
    ble_start_scan_rssi(param->rssi_cb_priv, param->rssi_cb);

    /* 执行任务完成回调 */
    if (param->cb != NULL) {
        ((void (*)(void* priv))param->cb)(param->priv);
    }
}

/**
 * 打印字符串，蓝牙广播名可能包含\0字符（...）
 */
void printf_dev_name(char *name, u16 len) {
    for (int i = 0; i< len; ++i) {
        printf("%c", name[i]);
    }
}

/**
 * @name my_task_switch_play
 * @brief This function is used to create a specific task message and enqueue it into a message queue.
 *
 * @param sn      Pointer to a character string indicating the E-end sequence number.
 * @param audio_url    Pointer to a character string indicating the audio source URL.
 * @param priv    A pointer to user-defined private data.
 * @param cb      A pointer to a callback function.
 */
void my_task_switch_play(char *sn, char* audio_url, void* priv, void* cb)
{
    /* 创建消息 */
    MyTaskMessageType message;
    message.msg = MY_TASK_MESSAGE_SWITCH_PLAY;
    memcpy(message.switch_play.sn, sn, SN_MAX_SIZE);
    strcpy(message.switch_play.audio_url, audio_url);
    message.switch_play.priv = priv;
    message.switch_play.cb = cb;
    printf("Put message: %d\n", message.msg);
    printf("sn         : ");    printf_dev_name(message.switch_only.sn, SN_MAX_SIZE); printf("\n");
    printf("audio_url  : %s\n", message.switch_play.audio_url);
    /* 消息入队 */
    my_blocking_queue_enqueue(&g_message_queue, &message);
}

/**
 * @name my_task_switch_only
 * @brief This function is used to create a specific task message and enqueue it into a message queue.
 *
 * @param name    Pointer to a character string indicating the E-end sequence number.
 * @param priv    A pointer to user-defined private data.
 * @param cb      A pointer to a callback function.
 */
void my_task_switch_only(char *sn, void* priv, void* cb)
{
    /* 创建消息 */
    MyTaskMessageType message;
    message.msg = MY_TASK_MESSAGE_SWITCH_ONLY;
    memcpy(message.switch_only.sn, sn, SN_MAX_SIZE);
    message.switch_only.priv = priv;
    message.switch_only.cb = cb;
    printf("Put message: %d\n", message.msg);
    printf("sn         : ");    printf_dev_name(message.switch_only.sn, SN_MAX_SIZE); printf("\n");
    /* 消息入队 */
    my_blocking_queue_enqueue(&g_message_queue, &message);
}

/**
 * @name my_task_switch_play
 * @brief This function is used to create a specific task message and enqueue it into a message queue.
 *
 * @param audio_messageId Audio messsage id (LingOS).
 * @param audio_url    Pointer to a character string indicating the audio source URL.
 * @param priv    A pointer to user-defined private data.
 * @param cb      A pointer to a callback function.
 */
void my_task_play_one(int audio_message_id, char* audio_url, void* priv, void* cb)
{
    /* 创建消息 */
    MyTaskMessageType message;
    message.msg = MY_TASK_MESSAGE_PLAY_ONE;
    strcpy(message.play_one.audio_url, audio_url);
    message.play_one.audio_enable = true;
    message.play_one.audio_message_id = audio_message_id;
    message.play_one.priv = priv;
    message.play_one.cb = cb;
    printf("Put message: %d\n", message.msg);
    printf("audio_url  : %s\n", message.play_one.audio_url);
    /* 消息入队 */
    my_blocking_queue_enqueue(&g_message_queue, &message);
}


/**
 * @name my_task_play_one
 * @brief This function is used to create a specific task message and enqueue it into a message queue.
 *
 * @param rssi_cb_priv A pointer to user-defined private data.
 * @param rssi_cb      A pointer to a callback function.
 * @param priv    A pointer to user-defined private data.
 * @param cb      A pointer to a callback function.
 */
void my_task_scan_rssi(void *rssi_cb_priv, 
        void (*rssi_cb)(void *priv, char *dev_name, u16 dev_name_len, s8 rssi), void* priv, void* cb)
{
    /* 创建消息 */
    MyTaskMessageType message;
    message.msg = MY_TASK_MESSAGE_MONITER_RSSI;
    message.moniter_rssi.rssi_cb_priv = rssi_cb_priv;
    message.moniter_rssi.rssi_cb = rssi_cb;
    message.moniter_rssi.priv = priv;
    message.moniter_rssi.cb = cb;
    printf("Put message: %d\n", message.msg);
    /* 消息入队 */
    my_blocking_queue_enqueue(&g_message_queue, &message);
    printf("Put message: %d over\n", message.msg);
}

static void audio_message_cancellation_cb(void *data, void *priv)
{
    /* 取消特定audio_only消息的播放 */
    int audio_message_id = (int)priv;
    MyTaskMessageType *pMessage = (MyTaskMessageType *)data;
    if (pMessage->msg == MY_TASK_MESSAGE_PLAY_ONE) {
        if (pMessage->play_one.audio_message_id == audio_message_id) {
            pMessage->play_one.audio_enable = false;
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

void start_async_msg_loop()
{
    printf("create my_task\n");
    if (thread_fork("my_task", 10, 1024, 0, NULL, my_task, NULL) != OS_NO_ERR) {
        printf("thread fork fail\n");
    }
}
