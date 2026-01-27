#include "ringbuffer.h"


/**
 * @name    my_ring_buffer_init
 * @brief   Init custom ringbuffer
 * 
 * @param   ringbuffer  A pointer to the MyRingBuffer structure which represents the ringbuffer.
 * @param   elementSize The size (in bytes) of each ringbuffer element.
 * @param   capacity    The maximum capacity of the ringbuffer.
 * 
 * @return  0 if success, see MyRBErrorType
 */
int my_ring_buffer_init(MyRingBuffer *ringbuffer, int elementSize, int capacity)
{
    int ret = MYRBERROR_Success;
    if (ringbuffer== NULL) {
        return MYRBERROR_Null_Pointer;
    }

    // 修复：添加整数溢出检查和类型修正
    size_t total_size = (size_t)elementSize * (size_t)capacity;
    // 检查整数溢出
    if (total_size < (size_t)elementSize || total_size < (size_t)capacity) {
        printf("Error: Integer overflow in ringbuffer allocation: elementSize=%d, capacity=%d\n", 
               elementSize, capacity);
        return MYRBERROR_Malloc_Failed;
    }
    // 类型修正：应该是void*而不是void**
    ringbuffer->data = (void *)malloc(total_size);
    if (ringbuffer->data == NULL) {
        printf("Error: Failed to malloc ringbuffer->data, size = %zu\n", total_size);
        return MYRBERROR_Malloc_Failed;
    }

    if (pthread_mutex_init(&ringbuffer->mutex, NULL) != 0) {
        printf("Error: Failed to init pthread_mutex\n");
        ret = MYRBERROR_OS_Api_Error;
        goto error;
    }

    ringbuffer->size = 0;
    ringbuffer->front = 0;
    ringbuffer->rear = 0;
    ringbuffer->capacity = capacity;
    ringbuffer->element_size = elementSize;
    printf("ringbuffer->data: %p, size: %d\n", ringbuffer->data, ringbuffer->element_size);
    return ret;

error:
    if (ringbuffer && ringbuffer->data) {
        free(ringbuffer->data);
        ringbuffer->data = NULL;
    }
    return ret;
}

/**
 * @name    my_ring_buffer_init_static
 * @brief   Init custom ringbuffer(static)
 * 
 * @param   ringbuffer  A pointer to the MyRingBuffer structure which represents the ringbuffer.
 * @param   data        A pointer to the data memory(size == elementSize * capacity).
 * @param   elementSize The size (in bytes) of each ringbuffer element.
 * @param   capacity    The maximum capacity of the ringbuffer.
 * 
 * @return  0 if success, see MyRBErrorType
 */
int my_ring_buffer_init_static(MyRingBuffer *ringbuffer, u8 *data, int elementSize, int capacity)
{
    int ret = MYRBERROR_Success;
    if (ringbuffer== NULL) {
        return MYRBERROR_Null_Pointer;
    }

    if (pthread_mutex_init(&ringbuffer->mutex, NULL) != 0) {
        printf("Error: Failed to init pthread_mutex\n");
        ret = MYRBERROR_OS_Api_Error;
        goto error;
    }

    ringbuffer->data = data;
    ringbuffer->size = 0;
    ringbuffer->front = 0;
    ringbuffer->rear = 0;
    ringbuffer->capacity = capacity;
    ringbuffer->element_size = elementSize;
    printf("ringbuffer->data: %p, size: %d\n", ringbuffer->data, ringbuffer->element_size);
    return ret;

error:
    pthread_mutex_destroy(&ringbuffer->mutex);
    return ret;
}


/**
 * @name    my_ring_buffer_destroy
 * @brief   Destroy the custom ringbuffer and release all associated resources.
 * 
 * @param   ringbuffer  A pointer to the MyRingBuffer structure which represents the ringbuffer.
 * 
 * @return  0 if success, see MyRBErrorType
 */
int my_ring_buffer_destroy(MyRingBuffer *ringbuffer)
{
    int ret = MYRBERROR_Success;

    if (ringbuffer && ringbuffer->data) {
        free(ringbuffer->data);
        ringbuffer->data = NULL;
    }

    pthread_mutex_destroy(&ringbuffer->mutex);

    return ret;
}

/**
 * @name    my_ring_buffer_static_destroy
 * @brief   Destroy the custom ringbuffer and release all associated resources(static).
 * 
 * @param   ringbuffer  A pointer to the MyRingBuffer structure which represents the ringbuffer.
 * 
 * @return  0 if success, see MyRBErrorType
 */
int my_ring_buffer_destroy_static(MyRingBuffer *ringbuffer)
{
    int ret = MYRBERROR_Success;

    pthread_mutex_destroy(&ringbuffer->mutex);

    return ret;
}

/**
 * @name    my_ring_buffer_write
 * @brief   write a data element into the custom ringbuffer.
 * 
 * @param   ringbuffer  A pointer to the MyRingBuffer structure which represents the ringbuffer.
 * @param   data        A pointer to the data element to be inserted into the ringbuffer
 * 
 * @return  0 if success, see MyRBErrorType
 */
int my_ring_buffer_write(MyRingBuffer *ringbuffer, void *data)
{
    pthread_mutex_lock(&ringbuffer->mutex);

    if (ringbuffer->size == ringbuffer->capacity) {
        ringbuffer->front = (ringbuffer->front == ringbuffer->capacity - 1) ? 0 : ringbuffer->front + 1;

        memcpy(&ringbuffer->data[ringbuffer->rear * ringbuffer->element_size], data, ringbuffer->element_size);
        ringbuffer->rear = (ringbuffer->rear == ringbuffer->capacity - 1) ? 0 : ringbuffer->rear + 1;
    } else {
        memcpy(&ringbuffer->data[ringbuffer->rear * ringbuffer->element_size], data, ringbuffer->element_size);
        ringbuffer->rear = (ringbuffer->rear == ringbuffer->capacity - 1) ? 0 : ringbuffer->rear + 1;
        ringbuffer->size++;
    }

    pthread_mutex_unlock(&ringbuffer->mutex);

    return MYRBERROR_Success;
}

/**
 * @name    my_ring_buffer_read
 * @brief   read an element from the custom ringbuffer.
 * 
 * @param   ringbuffer  A pointer to the MyRingBuffer structure which represents the ringbuffer.
 * @param   data        A pointer to the memory location where the readd element will be copied.
 * 
 * @return  0 if success, see MyRBErrorType
 */
int my_ring_buffer_read(MyRingBuffer *ringbuffer, void *data)
{
    pthread_mutex_lock(&ringbuffer->mutex);

    while (ringbuffer->size == 0) {
        pthread_mutex_unlock(&ringbuffer->mutex);
        return MYRBERROR_Is_Empty;
    }

    memcpy(data, &ringbuffer->data[ringbuffer->front * ringbuffer->element_size], ringbuffer->element_size);
    ringbuffer->front = (ringbuffer->front == ringbuffer->capacity - 1) ? 0 : ringbuffer->front + 1;
    ringbuffer->size --;

    pthread_mutex_unlock(&ringbuffer->mutex);

    return MYRBERROR_Success;
}

/**
 * @name my_ring_buffer_clear
 * @brief Clear the custom ringbuffer.
 * 
 * @param   ringbuffer  A pointer to the MyRingBuffer structure which represents the ringbuffer.
 * 
 * @return  0 if success, see MyRBErrorType
 */
int my_ring_buffer_clear(MyRingBuffer *ringbuffer)
{
    if (ringbuffer== NULL) {
        return MYRBERROR_Null_Pointer;
    }
    pthread_mutex_lock(&ringbuffer->mutex);
    ringbuffer->size = ringbuffer->front = ringbuffer->rear = 0;
    pthread_mutex_unlock(&ringbuffer->mutex);

    return MYRBERROR_Success;
}

/**
 * @name    my_ring_buffer_size
 * @brief   Get the number of elements in the custom ringbuffer.
 * 
 * @param   ringbuffer  A pointer to the MyRingBuffer structure which represents the ringbuffer.
 * 
 * @return  The number of elements in the ringbuffer, or MYRBERROR_Null_Pointer if the ringbuffer pointer is NULL.
 */
int my_ring_buffer_size(MyRingBuffer *ringbuffer)
{
    if (ringbuffer== NULL) {
        return -1;
    }
    return ringbuffer->size;
}

/**
 * @name    my_ring_buffer_is_empty
 * @brief   Check if the custom ringbuffer is empty.
 * 
 * @param   ringbuffer  A pointer to the MyRingBuffer structure which represents the ringbuffer.
 * 
 * @return  1 if the ringbuffer is empty, 0 otherwise. If the ringbuffer pointer is NULL, return MYRBERROR_Null_Pointer.
 */
int my_ring_buffer_is_empty(MyRingBuffer *ringbuffer)
{
    if (ringbuffer== NULL) {
        return MYRBERROR_Null_Pointer;
    }
    return ringbuffer->size == 0;
}

/**
 * @name    my_ring_buffer_is_full
 * @brief   Check if the custom ringbuffer is full.
 * 
 * @param   ringbuffer  A pointer to the MyRingBuffer structure which represents the ringbuffer.
 * 
 * @return  1 if the ringbuffer is full, 0 otherwise. If the ringbuffer pointer is NULL, return MYRBERROR_Null_Pointer.
 */
int my_ring_buffer_is_full(MyRingBuffer *ringbuffer)
{
    if (ringbuffer== NULL) {
        return MYRBERROR_Null_Pointer;
    }
    return ringbuffer->size == ringbuffer->capacity;
}