#include "ring_queue.h"
#include <stdlib.h>
#include <string.h>
#include <errno.h>

int ring_queue_init(RingQueue *queue, int capacity) 
{
    if (!queue) return RINGQUEUE_Null_Pointer;
    if (capacity <= 0) return RINGQUEUE_Invalid_Length;

    queue->data = (char *)malloc(capacity);
    if (!queue->data) return RINGQUEUE_Malloc_Failed;

    queue->capacity = capacity;
    queue->size = 0;
    queue->front = 0;
    queue->rear = 0;

    if (pthread_mutex_init(&queue->mutex, NULL) != 0) {
        free(queue->data);
        return RINGQUEUE_OS_Api_Error;
    }

    if (pthread_cond_init(&queue->not_empty, NULL) != 0 ||
        pthread_cond_init(&queue->not_full, NULL) != 0) {
        pthread_mutex_destroy(&queue->mutex);
        free(queue->data);
        return RINGQUEUE_OS_Api_Error;
    }

    return RINGQUEUE_Success;
}

int ring_queue_init_static(RingQueue *queue, char *buffer, int capacity) 
{
    if (!queue || !buffer) return RINGQUEUE_Null_Pointer;
    if (capacity <= 0) return RINGQUEUE_Invalid_Length;

    queue->data = buffer;
    queue->capacity = capacity;
    queue->size = 0;
    queue->front = 0;
    queue->rear = 0;

    if (pthread_mutex_init(&queue->mutex, NULL) != 0) {
        return RINGQUEUE_OS_Api_Error;
    }

    if (pthread_cond_init(&queue->not_empty, NULL) != 0 ||
        pthread_cond_init(&queue->not_full, NULL) != 0) {
        pthread_mutex_destroy(&queue->mutex);
        return RINGQUEUE_OS_Api_Error;
    }

    return RINGQUEUE_Success;
}

int ring_queue_destroy(RingQueue *queue) 
{
    if (!queue) return RINGQUEUE_Null_Pointer;

    pthread_mutex_lock(&queue->mutex);
    free(queue->data);
    queue->data = NULL;
    pthread_mutex_unlock(&queue->mutex);

    pthread_cond_destroy(&queue->not_empty);
    pthread_cond_destroy(&queue->not_full);
    pthread_mutex_destroy(&queue->mutex);

    return RINGQUEUE_Success;
}

int ring_queue_destroy_static(RingQueue *queue) 
{
    if (!queue) return RINGQUEUE_Null_Pointer;

    pthread_cond_destroy(&queue->not_empty);
    pthread_cond_destroy(&queue->not_full);
    pthread_mutex_destroy(&queue->mutex);

    return RINGQUEUE_Success;
}

int ring_queue_write(RingQueue *queue, const char *data, int len) 
{
    if (!queue || !data) return RINGQUEUE_Null_Pointer;
    if (len <= 0) return RINGQUEUE_Invalid_Length;

    pthread_mutex_lock(&queue->mutex);

    while (queue->capacity - queue->size < len) {
        pthread_cond_wait(&queue->not_full, &queue->mutex);
    }

    int write_len = len;
    int first_part = queue->capacity - queue->rear;
    if (first_part >= write_len) {
        memcpy(queue->data + queue->rear, data, write_len);
        queue->rear += write_len;
    } else {
        memcpy(queue->data + queue->rear, data, first_part);
        memcpy(queue->data, data + first_part, write_len - first_part);
        queue->rear = write_len - first_part;
    }

    if (queue->rear == queue->capacity) {
        queue->rear = 0;
    }

    queue->size += write_len;
    pthread_cond_signal(&queue->not_empty);
    pthread_mutex_unlock(&queue->mutex);

    return write_len;
}

int ring_queue_try_write(RingQueue *queue, const char *data, int len) 
{
    if (!queue || !data) return RINGQUEUE_Null_Pointer;
    if (len <= 0) return RINGQUEUE_Invalid_Length;

    pthread_mutex_lock(&queue->mutex);

    if (queue->capacity - queue->size < len) {
        pthread_mutex_unlock(&queue->mutex);
        return RINGQUEUE_Is_Full;
    }

    int write_len = len;
    int first_part = queue->capacity - queue->rear;
    if (first_part >= write_len) {
        memcpy(queue->data + queue->rear, data, write_len);
        queue->rear += write_len;
    } else {
        memcpy(queue->data + queue->rear, data, first_part);
        memcpy(queue->data, data + first_part, write_len - first_part);
        queue->rear = write_len - first_part;
    }

    if (queue->rear == queue->capacity) {
        queue->rear = 0;
    }

    queue->size += write_len;
    pthread_cond_signal(&queue->not_empty);
    pthread_mutex_unlock(&queue->mutex);

    return write_len;
}

int ring_queue_read(RingQueue *queue, char *data, int len) 
{
    if (!queue || !data) return RINGQUEUE_Null_Pointer;
    if (len <= 0) return RINGQUEUE_Invalid_Length;

    pthread_mutex_lock(&queue->mutex);

    while (queue->size < len) {
        pthread_cond_wait(&queue->not_empty, &queue->mutex);
    }

    int read_len = len;
    int first_part = queue->capacity - queue->front;
    if (first_part >= read_len) {
        memcpy(data, queue->data + queue->front, read_len);
        queue->front += read_len;
    } else {
        memcpy(data, queue->data + queue->front, first_part);
        memcpy(data + first_part, queue->data, read_len - first_part);
        queue->front = read_len - first_part;
    }

    if (queue->front == queue->capacity) {
        queue->front = 0;
    }

    queue->size -= read_len;
    pthread_cond_signal(&queue->not_full);
    pthread_mutex_unlock(&queue->mutex);

    return read_len;
}

int ring_queue_try_read(RingQueue *queue, char *data, int len) 
{
    if (!queue || !data) return RINGQUEUE_Null_Pointer;
    if (len <= 0) return RINGQUEUE_Invalid_Length;

    pthread_mutex_lock(&queue->mutex);

    if (queue->size == 0) {
        pthread_mutex_unlock(&queue->mutex);
        return RINGQUEUE_Is_Empty;
    }

    // 计算实际可读取的长度（取请求长度和可用长度的较小值）
    int read_len = (len < queue->size) ? len : queue->size;
    
    int first_part = queue->capacity - queue->front;
    if (first_part >= read_len) {
        // 不需要绕环的情况
        memcpy(data, queue->data + queue->front, read_len);
        queue->front += read_len;
    } else {
        // 需要绕环的情况
        memcpy(data, queue->data + queue->front, first_part);
        memcpy(data + first_part, queue->data, read_len - first_part);
        queue->front = read_len - first_part;
    }

    if (queue->front == queue->capacity) {
        queue->front = 0;
    }

    queue->size -= read_len;
    pthread_cond_signal(&queue->not_full);
    pthread_mutex_unlock(&queue->mutex);

    return read_len;
}

int ring_queue_peek(RingQueue *queue, char *data, int len) 
{
    if (!queue || !data) return RINGQUEUE_Null_Pointer;
    if (len <= 0) return RINGQUEUE_Invalid_Length;

    pthread_mutex_lock(&queue->mutex);

    if (queue->size < len) {
        pthread_mutex_unlock(&queue->mutex);
        return RINGQUEUE_Is_Empty;
    }

    int peek_len = len;
    int first_part = queue->capacity - queue->front;
    if (first_part >= peek_len) {
        memcpy(data, queue->data + queue->front, peek_len);
    } else {
        memcpy(data, queue->data + queue->front, first_part);
        memcpy(data + first_part, queue->data, peek_len - first_part);
    }

    pthread_mutex_unlock(&queue->mutex);
    return peek_len;
}

int ring_queue_clear(RingQueue *queue) 
{
    if (!queue) return RINGQUEUE_Null_Pointer;

    pthread_mutex_lock(&queue->mutex);
    queue->size = 0;
    queue->front = 0;
    queue->rear = 0;
    pthread_cond_signal(&queue->not_full);
    pthread_mutex_unlock(&queue->mutex);

    return RINGQUEUE_Success;
}

int ring_queue_size(RingQueue *queue) 
{
    if (!queue) return RINGQUEUE_Null_Pointer;

    pthread_mutex_lock(&queue->mutex);
    int size = queue->size;
    pthread_mutex_unlock(&queue->mutex);

    return size;
}

int ring_queue_capacity(RingQueue *queue) 
{
    if (!queue) return RINGQUEUE_Null_Pointer;
    return queue->capacity;
}

int ring_queue_is_empty(RingQueue *queue) 
{
    if (!queue) return RINGQUEUE_Null_Pointer;

    pthread_mutex_lock(&queue->mutex);
    int empty = (queue->size == 0);
    pthread_mutex_unlock(&queue->mutex);

    return empty;
}

int ring_queue_is_full(RingQueue *queue) 
{
    if (!queue) return RINGQUEUE_Null_Pointer;

    pthread_mutex_lock(&queue->mutex);
    int full = (queue->size == queue->capacity);
    pthread_mutex_unlock(&queue->mutex);

    return full;
}