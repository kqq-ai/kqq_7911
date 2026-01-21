#ifndef __THREADPOOL_H__
#define __THREADPOOL_H__

#include <os/pthread.h>

typedef struct {
    void (*function)(void *);
    void *arg;
} threadpool_task_t;

typedef struct {
    pthread_mutex_t lock;
    pthread_cond_t notify;
    pthread_t *threads;
    threadpool_task_t *queue;
    int thread_count;
    int queue_size;
    int head;
    int tail;
    int count;
    bool shutdown;
} threadpool_t;

threadpool_t *threadpool_create(int thread_count, int queue_size);
void threadpool_destroy(threadpool_t *pool);
bool threadpool_add_task(threadpool_t *pool, void (*function)(void *), void *arg);

#endif // __THREADPOOL_H__