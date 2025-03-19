#ifndef __MY_RING_BUFFER_H__
#define __MY_RING_BUFFER_H__

#include <os/pthread.h>

typedef enum {
    MYRBERROR_Success = 0,
    MYRBERROR_Null_Pointer = -1,
    MYRBERROR_Malloc_Failed = -2,
    MYRBERROR_OS_Api_Error = -3,
    MYRBERROR_Is_Full = -4,
    MYRBERROR_Is_Empty = -5,
} MyRBErrorType;

typedef struct {
    char *data;
    int capacity;
    int element_size;
    volatile int size;
    volatile int front;
    volatile int rear;
    pthread_mutex_t mutex;
} MyRingBuffer;

/**
 * @name    my_ring_buffer_init
 * @brief   Init custom ringbuffer
 * 
 * @param   ringbuffer  A pointer to the MyRingBuffer structure which represents the ringbuffer.
 * @param   elementSize The size (in bytes) of each ringbuffer element.
 * @param   capacity    The maximum capacity of the ringbuffer.
 * 
 * @return  0 if success, see MyErrorType
 */
int my_ring_buffer_init(MyRingBuffer *ringbuffer, int elementSize, int capacity);

/**
 * @name    my_ring_buffer_init_static
 * @brief   Init custom ringbuffer(static)
 * 
 * @param   ringbuffer  A pointer to the MyRingBuffer structure which represents the ringbuffer.
 * @param   data        A pointer to the data memory(size == elementSize * capacity).
 * @param   elementSize The size (in bytes) of each ringbuffer element.
 * @param   capacity    The maximum capacity of the ringbuffer.
 * 
 * @return  0 if success, see MyErrorType
 */
int my_ring_buffer_init_static(MyRingBuffer *ringbuffer, u8 *data, int elementSize, int capacity);

/**
 * @name    my_ring_buffer_destroy
 * @brief   Destroy the custom ringbuffer and release all associated resources.
 * 
 * @param   ringbuffer  A pointer to the MyRingBuffer structure which represents the ringbuffer.
 * 
 * @return  0 if success, see MyErrorType
 */
int my_ring_buffer_destroy(MyRingBuffer *ringbuffer);

/**
 * @name    my_ring_buffer_static_destroy
 * @brief   Destroy the custom ringbuffer and release all associated resources(static).
 * 
 * @param   ringbuffer  A pointer to the MyRingBuffer structure which represents the ringbuffer.
 * 
 * @return  0 if success, see MyErrorType
 */
int my_ring_buffer_destroy_static(MyRingBuffer *ringbuffer);

/**
 * @name    my_ring_buffer_write
 * @brief   write a data element into the custom ringbuffer.
 * 
 * @param   ringbuffer  A pointer to the MyRingBuffer structure which represents the ringbuffer.
 * @param   data        A pointer to the data element to be inserted into the ringbuffer
 * 
 * @return  0 if success, see MyErrorType
 */
int my_ring_buffer_write(MyRingBuffer *ringbuffer, void *data);

/**
 * @name    my_ring_buffer_read
 * @brief   read an element from the custom ringbuffer.
 * 
 * @param   ringbuffer  A pointer to the MyRingBuffer structure which represents the ringbuffer.
 * @param   data        A pointer to the memory location where the readd element will be copied.
 * 
 * @return  0 if success, see MyErrorType
 */
int my_ring_buffer_read(MyRingBuffer *ringbuffer, void *data);

/**
 * @name my_ring_buffer_clear
 * @brief Clear the custom ringbuffer.
 * 
 * @param   ringbuffer  A pointer to the MyRingBuffer structure which represents the ringbuffer.
 * 
 * @return  0 if success, see MyErrorType
 */
int my_ring_buffer_clear(MyRingBuffer *ringbuffer);

/**
 * @name    my_ring_buffer_size
 * @brief   Get the number of elements in the custom ringbuffer.
 * 
 * @param   ringbuffer  A pointer to the MyRingBuffer structure which represents the ringbuffer.
 * 
 * @return  The number of elements in the ringbuffer, or MYERROR_Null_Pointer if the ringbuffer pointer is NULL.
 */
// int my_ring_buffer_size(MyRingBuffer *ringbuffer);

/**
 * @name    my_ring_buffer_is_empty
 * @brief   Check if the custom ringbuffer is empty.
 * 
 * @param   ringbuffer  A pointer to the MyRingBuffer structure which represents the ringbuffer.
 * 
 * @return  1 if the ringbuffer is empty, 0 otherwise. If the ringbuffer pointer is NULL, return MYERROR_Null_Pointer.
 */
// int my_ring_buffer_is_empty(MyRingBuffer *ringbuffer);

/**
 * @name    my_ring_buffer_is_full
 * @brief   Check if the custom ringbuffer is full.
 * 
 * @param   ringbuffer  A pointer to the MyRingBuffer structure which represents the ringbuffer.
 * 
 * @return  1 if the ringbuffer is full, 0 otherwise. If the ringbuffer pointer is NULL, return MYERROR_Null_Pointer.
 */
// int my_ring_buffer_is_full(MyRingBuffer *ringbuffer);


#endif