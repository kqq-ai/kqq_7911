#ifndef __RWLOCK_H__
#define __RWLOCK_H__

#include "rwlock.h"

#include "cpu.h"
#include <os/pthread.h>

typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t read_cond;
    pthread_cond_t write_cond;
    int readers;
    int writers_waiting;
    int writer_active;
} rwlock_t;

void rwlock_init(rwlock_t *rwlock);
void rwlock_acquire_read(rwlock_t *rwlock);
void rwlock_release_read(rwlock_t *rwlock);
void rwlock_acquire_write(rwlock_t *rwlock);
void rwlock_release_write(rwlock_t *rwlock);
void rwlock_destroy(rwlock_t *rwlock);

#endif // __RWLOCK_H__