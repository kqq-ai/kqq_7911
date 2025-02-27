#include "threadpool.h"
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>

static void *threadpool_worker(void *threadpool);

threadpool_t *threadpool_create(int thread_count, int queue_size) {
    threadpool_t *pool;
    int i;
    pthread_attr_t attr;
    size_t stack_size = 1024 * 4;  // 设置栈大小为1MB

    if (thread_count <= 0 || queue_size <= 0) {
        return NULL;
    }

    pool = (threadpool_t *)malloc(sizeof(threadpool_t));
    if (pool == NULL) {
        return NULL;
    }

    pool->thread_count = thread_count;
    pool->queue_size = queue_size;
    pool->head = pool->tail = pool->count = 0;
    pool->shutdown = false;

    pool->threads = (pthread_t *)malloc(sizeof(pthread_t) * thread_count);
    pool->queue = (threadpool_task_t *)malloc(sizeof(threadpool_task_t) * queue_size);

    if (pool->threads == NULL || pool->queue == NULL) {
        free(pool->threads);
        free(pool->queue);
        free(pool);
        return NULL;
    }

    if (pthread_mutex_init(&(pool->lock), NULL) != 0 ||
        pthread_cond_init(&(pool->notify), NULL) != 0) {
        free(pool->threads);
        free(pool->queue);
        free(pool);
        return NULL;
    }

    // 初始化线程属性并设置栈大小
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, stack_size);

    for (i = 0; i < thread_count; i++) {
        if (pthread_create(&(pool->threads[i]), &attr, threadpool_worker, (void*)pool) != 0) {
            threadpool_destroy(pool);
            pthread_attr_destroy(&attr);  // 清理线程属性
            return NULL;
        }
    }

    pthread_attr_destroy(&attr);  // 清理线程属性

    return pool;
}

void threadpool_destroy(threadpool_t *pool) {
    int i;

    if (pool == NULL) {
        return;
    }

    pthread_mutex_lock(&(pool->lock));
    pool->shutdown = true;
    pthread_cond_broadcast(&(pool->notify));
    pthread_mutex_unlock(&(pool->lock));

    for (i = 0; i < pool->thread_count; i++) {
        pthread_join(pool->threads[i], NULL);
    }

    free(pool->threads);
    free(pool->queue);
    free(pool);
}

bool threadpool_add_task(threadpool_t *pool, void (*function)(void *), void *arg) {
    if (pool == NULL || function == NULL) {
        return false;
    }

    pthread_mutex_lock(&(pool->lock));

    if (pool->count == pool->queue_size) {
        pthread_mutex_unlock(&(pool->lock));
        return false;
    }

    pool->queue[pool->tail].function = function;
    pool->queue[pool->tail].arg = arg;
    pool->tail = (pool->tail + 1) % pool->queue_size;
    pool->count++;

    pthread_cond_signal(&(pool->notify));
    pthread_mutex_unlock(&(pool->lock));

    return true;
}

static void *threadpool_worker(void *threadpool) {
    threadpool_t *pool = (threadpool_t *)threadpool;
    threadpool_task_t task;

    while (1) {
        pthread_mutex_lock(&(pool->lock));

        while (pool->count == 0 && !pool->shutdown) {
            pthread_cond_wait(&(pool->notify), &(pool->lock));
        }

        if (pool->shutdown) {
            break;
        }

        task.function = pool->queue[pool->head].function;
        task.arg = pool->queue[pool->head].arg;
        pool->head = (pool->head + 1) % pool->queue_size;
        pool->count--;

        pthread_mutex_unlock(&(pool->lock));

        (*(task.function))(task.arg);
    }

    pthread_mutex_unlock(&(pool->lock));
    pthread_exit(NULL);
    return NULL;
}