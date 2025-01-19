#ifndef __C_QUEUE_H__
#define __C_QUEUE_H__

#include <os/pthread.h>

typedef enum {
    MYERROR_Success = 0,
    MYERROR_Null_Pointer = -1,
    MYERROR_Malloc_Failed = -2,
    MYERROR_OS_Api_Error = -3,
} MyErrorType;

typedef struct {
    char *data;
    int capacity;
    int element_size;
    volatile int size;
    volatile int front;
    volatile int rear;
    pthread_mutex_t mutex;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
} MyBlockingQueue;

/**
 * @name    my_blocking_queue_init
 * @brief   Init custom blocking queue
 * 
 * @param   queue       A pointer to the MyBlockingQueue structure which represents the blocking queue.
 * @param   elementSize The size (in bytes) of each queue element.
 * @param   capacity    The maximum capacity of the queue.
 * 
 * @return  0 if success, see MyErrorType
 */
int my_blocking_queue_init(MyBlockingQueue *queue, int elementSize, int capacity);

/**
 * @name    my_blocking_queue_init_static
 * @brief   Init custom blocking queue(static)
 * 
 * @param   queue       A pointer to the MyBlockingQueue structure which represents the blocking queue.
 * @param   data        A pointer to the data memory(size == elementSize * capacity).
 * @param   elementSize The size (in bytes) of each queue element.
 * @param   capacity    The maximum capacity of the queue.
 * 
 * @return  0 if success, see MyErrorType
 */
int my_blocking_queue_init_static(MyBlockingQueue *queue, u8 *data, int elementSize, int capacity);

/**
 * @name    my_blocking_queue_destroy
 * @brief   Destroy the custom blocking queue and release all associated resources.
 * 
 * @param   queue       A pointer to the MyBlockingQueue structure which represents the blocking queue.
 * 
 * @return  0 if success, see MyErrorType
 */
int my_blocking_queue_destroy(MyBlockingQueue *queue);

/**
 * @name    my_blocking_queue_static_destroy
 * @brief   Destroy the custom blocking queue and release all associated resources(static).
 * 
 * @param   queue       A pointer to the MyBlockingQueue structure which represents the blocking queue.
 * 
 * @return  0 if success, see MyErrorType
 */
int my_blocking_queue_destroy_static(MyBlockingQueue *queue);

/**
 * @name    my_blocking_queue_enqueue
 * @brief   Enqueue a data element into the custom blocking queue.
 * 
 * @param   queue       A pointer to the MyBlockingQueue structure which represents the blocking queue.
 * @param   data        A pointer to the data element to be inserted into the queue
 * 
 * @return  0 if success, see MyErrorType
 */
int my_blocking_queue_enqueue(MyBlockingQueue *queue, void *data);

/**
 * @name    my_blocking_queue_dequeue
 * @brief   Dequeue an element from the custom blocking queue.
 * 
 * @param   queue       A pointer to the MyBlockingQueue structure which represents the blocking queue.
 * @param   data        A pointer to the memory location where the dequeued element will be copied.
 * 
 * @return  0 if success, see MyErrorType
 */
int my_blocking_queue_dequeue(MyBlockingQueue *queue, void *data);


/**
 * @name    my_blocking_queue_traverse
 * @brief   Traverse the custom blocking queue and execute a callback function for each element.
 * 
 * @param   queue       A pointer to the MyBlockingQueue structure which represents the blocking queue.
 * @param   callback    A pointer to the callback function which will be called for each element in the queue.
 *                     The callback function should take a pointer to the element as its parameter and return void.
 * @param   priv        A pointer to user-defined private data.
 * 
 * @return  0 if success, see MyErrorType
 */
int my_blocking_queue_traverse(MyBlockingQueue *queue, void (*callback)(void *, void *), void *priv);

/**
 * @name my_blocking_queue_clear
 * @brief Clear the custom blocking queue.
 * 
 * @param   queue       A pointer to the MyBlockingQueue structure which represents the blocking queue.
 * 
 * @return  0 if success, see MyErrorType
 */
int my_blocking_queue_clear(MyBlockingQueue *queue);

/**
 * @name    my_blocking_queue_size
 * @brief   Get the number of elements in the custom blocking queue.
 * 
 * @param   queue       A pointer to the MyBlockingQueue structure which represents the blocking queue.
 * 
 * @return  The number of elements in the queue, or MYERROR_Null_Pointer if the queue pointer is NULL.
 */
int my_blocking_queue_size(MyBlockingQueue *queue);

/**
 * @name    my_blocking_queue_is_empty
 * @brief   Check if the custom blocking queue is empty.
 * 
 * @param   queue       A pointer to the MyBlockingQueue structure which represents the blocking queue.
 * 
 * @return  1 if the queue is empty, 0 otherwise. If the queue pointer is NULL, return MYERROR_Null_Pointer.
 */
int my_blocking_queue_is_empty(MyBlockingQueue *queue);

/**
 * @name    my_blocking_queue_is_full
 * @brief   Check if the custom blocking queue is full.
 * 
 * @param   queue       A pointer to the MyBlockingQueue structure which represents the blocking queue.
 * 
 * @return  1 if the queue is full, 0 otherwise. If the queue pointer is NULL, return MYERROR_Null_Pointer.
 */
int my_blocking_queue_is_full(MyBlockingQueue *queue);


#endif