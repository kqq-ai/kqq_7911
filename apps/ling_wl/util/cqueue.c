#include "cqueue.h"

#include <os/pthread.h>

#define NEXT_INDEX(idx, cap) ((idx) + 1 == (cap) ? 0 : (idx) + 1)
#define PREV_INDEX(idx, cap) ((idx) == 0 ? (cap) - 1 : (idx) - 1)
#define QUEUE_ELEMENT(queue, index) (&(queue)->data[(index) * (queue)->element_size])

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
int my_blocking_queue_init(MyBlockingQueue *queue, int elementSize, int capacity)
{
    int ret = MYERROR_Success;
    if (queue == NULL) {
        return MYERROR_Null_Pointer;
    }

    queue->data = (void **)malloc(elementSize * capacity);
    if (queue->data == NULL) {
        printf("Error: Failed to malloc queue->data, size = %d\n", elementSize * capacity);
        return MYERROR_Malloc_Failed;
    }

    if (pthread_mutex_init(&queue->mutex, NULL) != 0) {
        printf("Error: Failed to init pthread_mutex\n");
        ret = MYERROR_OS_Api_Error;
        goto error;
    }
    if (pthread_cond_init(&queue->not_empty, NULL) != 0) {
        printf("Error: Failed to init pthread_cond\n");
        ret = MYERROR_OS_Api_Error;
        goto error;
    }
    if (pthread_cond_init(&queue->not_full, NULL) != 0) {
        printf("Error: Failed to init pthread_cond\n");
        ret = MYERROR_OS_Api_Error;
        goto error;
    }

    queue->size = 0;
    queue->front = 0;
    queue->rear = 0;
    queue->capacity = capacity;
    queue->element_size = elementSize;
    printf("queue->data: %p, size: %d\n", queue->data, queue->element_size);
    return ret;

error:
    if (queue && queue->data) {
        free(queue->data);
        queue->data = NULL;
    }
    pthread_mutex_destroy(&queue->mutex);
    pthread_cond_destroy(&queue->not_empty);
    pthread_cond_destroy(&queue->not_full);
    return ret;
}

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
int my_blocking_queue_init_static(MyBlockingQueue *queue, u8 *data, int elementSize, int capacity)
{
    int ret = MYERROR_Success;
    if (queue == NULL) {
        return MYERROR_Null_Pointer;
    }
    

    if (pthread_mutex_init(&queue->mutex, NULL) != 0) {
        printf("Error: Failed to init pthread_mutex\n");
        ret = MYERROR_OS_Api_Error;
        goto error;
    }
    if (pthread_cond_init(&queue->not_empty, NULL) != 0) {
        printf("Error: Failed to init pthread_cond\n");
        ret = MYERROR_OS_Api_Error;
        goto error;
    }
    if (pthread_cond_init(&queue->not_full, NULL) != 0) {
        printf("Error: Failed to init pthread_cond\n");
        ret = MYERROR_OS_Api_Error;
        goto error;
    }

    queue->data = data;
    queue->size = 0;
    queue->front = 0;
    queue->rear = 0;
    queue->capacity = capacity;
    queue->element_size = elementSize;
    printf("queue->data: %p, size: %d\n", queue->data, queue->element_size);
    return ret;

error:
    pthread_mutex_destroy(&queue->mutex);
    pthread_cond_destroy(&queue->not_empty);
    pthread_cond_destroy(&queue->not_full);
    return ret;
}


/**
 * @name    my_blocking_queue_destroy
 * @brief   Destroy the custom blocking queue and release all associated resources.
 * 
 * @param   queue       A pointer to the MyBlockingQueue structure which represents the blocking queue.
 * 
 * @return  0 if success, see MyErrorType
 */
int my_blocking_queue_destroy(MyBlockingQueue *queue)
{
    int ret = MYERROR_Success;

    if (queue && queue->data) {
        free(queue->data);
        queue->data = NULL;
    }
    pthread_mutex_destroy(&queue->mutex);
    pthread_cond_destroy(&queue->not_empty);
    pthread_cond_destroy(&queue->not_full);

    return ret;
}

/**
 * @name    my_blocking_queue_static_destroy
 * @brief   Destroy the custom blocking queue and release all associated resources(static).
 * 
 * @param   queue       A pointer to the MyBlockingQueue structure which represents the blocking queue.
 * 
 * @return  0 if success, see MyErrorType
 */
int my_blocking_queue_destroy_static(MyBlockingQueue *queue)
{
    int ret = MYERROR_Success;

    pthread_mutex_destroy(&queue->mutex);
    pthread_cond_destroy(&queue->not_empty);
    pthread_cond_destroy(&queue->not_full);

    return ret;
}

/**
 * @name    my_blocking_queue_enqueue
 * @brief   Enqueue a data element into the custom blocking queue.
 * 
 * @param   queue       A pointer to the MyBlockingQueue structure which represents the blocking queue.
 * @param   data        A pointer to the data element to be inserted into the queue
 * 
 * @return  0 if success, see MyErrorType
 */
int my_blocking_queue_enqueue(MyBlockingQueue *queue, void *data)
{
    pthread_mutex_lock(&queue->mutex);

    while (queue->size == queue->capacity) {
        pthread_cond_wait(&queue->not_full, &queue->mutex);
    }

    memcpy(&queue->data[queue->rear * queue->element_size], data, queue->element_size);
    queue->rear = (queue->rear == queue->capacity - 1) ? 0 : queue->rear + 1;
    queue->size++;

    pthread_cond_signal(&queue->not_empty);

    pthread_mutex_unlock(&queue->mutex);

    return MYERROR_Success;
}

/**
 * @name    my_blocking_queue_try_enqueue
 * @brief   Enqueue a data element into the custom blocking queue.
 * 
 * @param   queue       A pointer to the MyBlockingQueue structure which represents the blocking queue.
 * @param   data        A pointer to the data element to be inserted into the queue
 * 
 * @return  0 if success, see MyErrorType
 */
int my_blocking_queue_try_enqueue(MyBlockingQueue *queue, void *data)
{
    pthread_mutex_lock(&queue->mutex);

    if (queue->size == queue->capacity) {
        pthread_mutex_unlock(&queue->mutex);
        return MYERROR_Is_Full;
    }

    memcpy(&queue->data[queue->rear * queue->element_size], data, queue->element_size);
    queue->rear = (queue->rear == queue->capacity - 1) ? 0 : queue->rear + 1;
    queue->size++;

    pthread_cond_signal(&queue->not_empty);

    pthread_mutex_unlock(&queue->mutex);

    return MYERROR_Success;
}

/**
 * @name    my_blocking_queue_dequeue
 * @brief   Dequeue an element from the custom blocking queue.
 * 
 * @param   queue       A pointer to the MyBlockingQueue structure which represents the blocking queue.
 * @param   data        A pointer to the memory location where the dequeued element will be copied.
 * 
 * @return  0 if success, see MyErrorType
 */
int my_blocking_queue_dequeue(MyBlockingQueue *queue, void *data)
{
    pthread_mutex_lock(&queue->mutex);

    while (queue->size == 0) {
        pthread_cond_wait(&queue->not_empty, &queue->mutex);
    }

    memcpy(data, &queue->data[queue->front * queue->element_size], queue->element_size);
    queue->front = (queue->front == queue->capacity - 1) ? 0 : queue->front + 1;
    queue->size --;

    pthread_cond_signal(&queue->not_full);

    pthread_mutex_unlock(&queue->mutex);

    return MYERROR_Success;
}

/**
 * @name    my_blocking_queue_try_dequeue
 * @brief   Dequeue an element from the custom blocking queue.
 * 
 * @param   queue       A pointer to the MyBlockingQueue structure which represents the blocking queue.
 * @param   data        A pointer to the memory location where the dequeued element will be copied.
 * 
 * @return  0 if success, see MyErrorType
 */
int my_blocking_queue_try_dequeue(MyBlockingQueue *queue, void *data)
{
    pthread_mutex_lock(&queue->mutex);

    if (queue->size == 0) {
        pthread_mutex_unlock(&queue->mutex);
        return MYERROR_Is_Empty;
    }

    memcpy(data, &queue->data[queue->front * queue->element_size], queue->element_size);
    queue->front = (queue->front == queue->capacity - 1) ? 0 : queue->front + 1;
    queue->size --;

    pthread_cond_signal(&queue->not_full);

    pthread_mutex_unlock(&queue->mutex);

    return MYERROR_Success;
}

/**
 * @name    my_blocking_queue_try_peek
 * @brief   Peek an element from the custom blocking queue.
 * 
 * @param   queue       A pointer to the MyBlockingQueue structure which represents the blocking queue.
 * @param   data        A pointer to the memory location where the dequeued element will be copied.
 * 
 * @return  0 if success, see MyErrorType
 */
int my_blocking_queue_try_peek(MyBlockingQueue *queue, void *data)
{
    pthread_mutex_lock(&queue->mutex);

    if (queue->size == 0) {
        pthread_mutex_unlock(&queue->mutex);
        return MYERROR_Is_Empty;
    }

    memcpy(data, &queue->data[queue->front * queue->element_size], queue->element_size);

    pthread_mutex_unlock(&queue->mutex);

    return MYERROR_Success;
}

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
int my_blocking_queue_traverse(MyBlockingQueue *queue, void (*callback)(void *, void *), void *priv)
{
    if (queue == NULL || callback == NULL) {
        return MYERROR_Null_Pointer;
    }

    pthread_mutex_lock(&queue->mutex);
    int index = queue->front;
    for (int i = 0; i < queue->size; ++i) {
        callback(&queue->data[index * queue->element_size], priv);
        index = (index == queue->capacity - 1)? 0 : index + 1;
    }
    pthread_mutex_unlock(&queue->mutex);

    return MYERROR_Success;
}

/**
 * @name my_blocking_queue_clear
 * @brief Clear the custom blocking queue.
 * 
 * @param   queue       A pointer to the MyBlockingQueue structure which represents the blocking queue.
 * 
 * @return  0 if success, see MyErrorType
 */
int my_blocking_queue_clear(MyBlockingQueue *queue)
{
    if (queue == NULL) {
        return MYERROR_Null_Pointer;
    }
    pthread_mutex_lock(&queue->mutex);
    queue->size = queue->front = queue->rear = 0;
    pthread_mutex_unlock(&queue->mutex);

    return MYERROR_Success;
}

/**
 * @name    my_blocking_queue_size
 * @brief   Get the number of elements in the custom blocking queue.
 * 
 * @param   queue       A pointer to the MyBlockingQueue structure which represents the blocking queue.
 * 
 * @return  The number of elements in the queue, or MYERROR_Null_Pointer if the queue pointer is NULL.
 */
int my_blocking_queue_size(MyBlockingQueue *queue)
{
    if (queue == NULL) {
        return -1;
    }
    return queue->size;
}

/**
 * @name    my_blocking_queue_is_empty
 * @brief   Check if the custom blocking queue is empty.
 * 
 * @param   queue       A pointer to the MyBlockingQueue structure which represents the blocking queue.
 * 
 * @return  1 if the queue is full, 0 otherwise. If the queue pointer is NULL, return MYERROR_Null_Pointer.
 */
int my_blocking_queue_is_empty(MyBlockingQueue *queue)
{
    if (queue == NULL) {
        return MYERROR_Null_Pointer;
    }
    return queue->size == 0;
}

/**
 * @name    my_blocking_queue_is_full
 * @brief   Check if the custom blocking queue is full.
 * 
 * @param   queue       A pointer to the MyBlockingQueue structure which represents the blocking queue.
 * 
 * @return  1 if the queue is full, 0 otherwise. If the queue pointer is NULL, return MYERROR_Null_Pointer.
 */
int my_blocking_queue_is_full(MyBlockingQueue *queue)
{
    if (queue == NULL) {
        return MYERROR_Null_Pointer;
    }
    return queue->size == queue->capacity;
}

/**
 * @name    my_blocking_queue_try_remove_if
 * @brief   Remove the first element in the queue that matches the condition (no % operations)
 * 
 * @param   queue       A pointer to the MyBlockingQueue structure
 * @param   condition   A pointer to the condition function
 * @param   priv        A pointer to user-defined private data
 * 
 * @return  MYERROR_Success if removed successfully,
 *          MYERROR_Null_Pointer if queue or condition is NULL,
 *          MYERROR_Not_Found if no matching element found
 */
int my_blocking_queue_try_remove_if(MyBlockingQueue *queue, int (*condition)(void *, void *), void *priv)
{
    if (queue == NULL || condition == NULL) {
        return MYERROR_Null_Pointer;
    }

    pthread_mutex_lock(&queue->mutex);

    int removed_index = -1;
    int index = queue->front;
    for (int i = 0; i < queue->size; ++i) {
        if (condition(&queue->data[index * queue->element_size], priv)) {
            removed_index = index;
            break;
        }
        index = NEXT_INDEX(index, queue->capacity);
    }
    if (removed_index == -1) {
        pthread_mutex_unlock(&queue->mutex);
        return MYERROR_Not_Found;
    }

    if (queue->size == 1) {
        queue->front = queue->rear = 0;
    } else if (removed_index == queue->front) {
        queue->front = NEXT_INDEX(queue->front, queue->capacity);
    } else if (removed_index == PREV_INDEX(queue->rear, queue->capacity)) {
        queue->rear = PREV_INDEX(queue->rear, queue->capacity);
    } else if (queue->front < removed_index) {
        int len = removed_index - queue->front;
        int dst = NEXT_INDEX(queue->front, queue->capacity);
        int src = queue->front;
        memmove(QUEUE_ELEMENT(queue, dst), QUEUE_ELEMENT(queue, src), len * queue->element_size);
    } else if (removed_index < PREV_INDEX(queue->rear, queue->capacity)) {
        int len = PREV_INDEX(queue->rear, queue->capacity) - removed_index;
        int dst = removed_index;
        int src = NEXT_INDEX(removed_index, queue->capacity);
        memmove(QUEUE_ELEMENT(queue, dst), QUEUE_ELEMENT(queue, src), len * queue->element_size);
    }
    queue->size--;
    pthread_cond_signal(&queue->not_full);
    
    pthread_mutex_unlock(&queue->mutex);
    return MYERROR_Success;
}