#ifndef __RING_QUEUE_H__
#define __RING_QUEUE_H__

#include <os/pthread.h>

typedef enum {
    RINGQUEUE_Success = 0,
    RINGQUEUE_Null_Pointer = -1,
    RINGQUEUE_Malloc_Failed = -2,
    RINGQUEUE_OS_Api_Error = -3,
    RINGQUEUE_Is_Full = -4,
    RINGQUEUE_Is_Empty = -5,
    RINGQUEUE_Invalid_Length = -6,
} RingQueueErrorType;

typedef struct {
    char *data;             // 数据缓冲区
    int capacity;           // 队列总容量
    volatile int size;      // 当前数据量
    volatile int front;     // 读指针
    volatile int rear;      // 写指针
    pthread_mutex_t mutex;  // 互斥锁
    pthread_cond_t not_empty; // 非空条件变量
    pthread_cond_t not_full;  // 非满条件变量
} RingQueue;

/**
 * @name    ring_queue_init
 * @brief   初始化环形队列
 * 
 * @param   queue    指向环形队列结构的指针
 * @param   capacity 队列容量
 * 
 * @return  0成功，其他见RingQueueErrorType
 */
int ring_queue_init(RingQueue *queue, int capacity);

/**
 * @name    ring_queue_init_static
 * @brief   静态初始化环形队列
 * 
 * @param   queue    指向环形队列结构的指针
 * @param   buffer   外部提供的缓冲区
 * @param   capacity 队列容量
 * 
 * @return  0成功，其他见RingQueueErrorType
 */
int ring_queue_init_static(RingQueue *queue, char *buffer, int capacity);

/**
 * @name    ring_queue_destroy
 * @brief   销毁环形队列
 * 
 * @param   queue    指向环形队列结构的指针
 * 
 * @return  0成功，其他见RingQueueErrorType
 */
int ring_queue_destroy(RingQueue *queue);

/**
 * @name    ring_queue_destroy_static
 * @brief   销毁静态初始化的环形队列
 * 
 * @param   queue    指向环形队列结构的指针
 * 
 * @return  0成功，其他见RingQueueErrorType
 */
int ring_queue_destroy_static(RingQueue *queue);

/**
 * @name    ring_queue_write
 * @brief   向队列写入数据(阻塞)
 * 
 * @param   queue    指向环形队列结构的指针
 * @param   data     要写入的数据
 * @param   len      要写入的长度
 * 
 * @return  实际写入的长度，负数见RingQueueErrorType
 */
int ring_queue_write(RingQueue *queue, const char *data, int len);

/**
 * @name    ring_queue_try_write
 * @brief   尝试向队列写入数据(非阻塞)
 * 
 * @param   queue    指向环形队列结构的指针
 * @param   data     要写入的数据
 * @param   len      要写入的长度
 * 
 * @return  实际写入的长度，负数见RingQueueErrorType
 */
int ring_queue_try_write(RingQueue *queue, const char *data, int len);

/**
 * @name    ring_queue_read
 * @brief   从队列读取数据(阻塞)
 * 
 * @param   queue    指向环形队列结构的指针
 * @param   data     存储读取数据的缓冲区
 * @param   len      要读取的长度
 * 
 * @return  实际读取的长度，负数见RingQueueErrorType
 */
int ring_queue_read(RingQueue *queue, char *data, int len);

/**
 * @name    ring_queue_try_read
 * @brief   尝试从队列读取数据(非阻塞)
 * 
 * @param   queue    指向环形队列结构的指针
 * @param   data     存储读取数据的缓冲区
 * @param   len      要读取的长度
 * 
 * @return  实际读取的长度，负数见RingQueueErrorType
 */
int ring_queue_try_read(RingQueue *queue, char *data, int len);

/**
 * @name    ring_queue_try_read
 * @brief   尝试从队列读取数据(非阻塞)
 * 
 * @param   queue    指向环形队列结构的指针
 * @param   data     存储读取数据的缓冲区
 * @param   len      要读取的长度
 * @param   timeout_ms 超时时间，单位毫秒
 * 
 * @return  实际读取的长度，负数见RingQueueErrorType
 */
int ring_queue_try_read_timeout(RingQueue *queue, char *data, int len, int timeout_ms);

/**
 * @name    ring_queue_peek
 * @brief   查看队列中的数据但不移除(非阻塞)
 * 
 * @param   queue    指向环形队列结构的指针
 * @param   data     存储数据的缓冲区
 * @param   len      要查看的长度
 * 
 * @return  实际查看的长度，负数见RingQueueErrorType
 */
int ring_queue_peek(RingQueue *queue, char *data, int len);

/**
 * @name    ring_queue_clear
 * @brief   清空队列
 * 
 * @param   queue    指向环形队列结构的指针
 * 
 * @return  0成功，其他见RingQueueErrorType
 */
int ring_queue_clear(RingQueue *queue);

/**
 * @name    ring_queue_size
 * @brief   获取队列当前数据量
 * 
 * @param   queue    指向环形队列结构的指针
 * 
 * @return  当前数据量，负数见RingQueueErrorType
 */
int ring_queue_size(RingQueue *queue);

/**
 * @name    ring_queue_capacity
 * @brief   获取队列容量
 * 
 * @param   queue    指向环形队列结构的指针
 * 
 * @return  队列容量，负数见RingQueueErrorType
 */
int ring_queue_capacity(RingQueue *queue);

/**
 * @name    ring_queue_is_empty
 * @brief   检查队列是否为空
 * 
 * @param   queue    指向环形队列结构的指针
 * 
 * @return  1为空，0非空，负数见RingQueueErrorType
 */
int ring_queue_is_empty(RingQueue *queue);

/**
 * @name    ring_queue_is_full
 * @brief   检查队列是否已满
 * 
 * @param   queue    指向环形队列结构的指针
 * 
 * @return  1已满，0未满，负数见RingQueueErrorType
 */
int ring_queue_is_full(RingQueue *queue);

#endif // __RING_QUEUE_H__