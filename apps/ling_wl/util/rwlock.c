#include "rwlock.h"

#include "cpu.h"
#include <os/pthread.h>

void rwlock_init(rwlock_t *rwlock) {
    pthread_mutex_init(&rwlock->mutex, NULL);
    pthread_cond_init(&rwlock->read_cond, NULL);
    pthread_cond_init(&rwlock->write_cond, NULL);
    rwlock->readers = 0;
    rwlock->writers_waiting = 0;
    rwlock->writer_active = 0;
}

void rwlock_acquire_read(rwlock_t *rwlock) {
    pthread_mutex_lock(&rwlock->mutex);
    
    while (rwlock->writer_active) {
        pthread_cond_wait(&rwlock->read_cond, &rwlock->mutex);
    }
    
    rwlock->readers++;
    
    pthread_mutex_unlock(&rwlock->mutex);
}

void rwlock_release_read(rwlock_t *rwlock) {
    pthread_mutex_lock(&rwlock->mutex);
    
    rwlock->readers--;
    
    if (rwlock->readers == 0 && rwlock->writers_waiting > 0) {
        pthread_cond_signal(&rwlock->write_cond);
    }
    
    pthread_mutex_unlock(&rwlock->mutex);
}

void rwlock_acquire_write(rwlock_t *rwlock) {
    pthread_mutex_lock(&rwlock->mutex);
    
    rwlock->writers_waiting++;
    
    while (rwlock->readers > 0 || rwlock->writer_active) {
        pthread_cond_wait(&rwlock->write_cond, &rwlock->mutex);
    }
    
    rwlock->writers_waiting--;
    
    rwlock->writer_active = 1;
    
    pthread_mutex_unlock(&rwlock->mutex);
}

void rwlock_release_write(rwlock_t *rwlock) {
    pthread_mutex_lock(&rwlock->mutex);
    
    rwlock->writer_active = 0;
    
    if (rwlock->writers_waiting > 0) {
        pthread_cond_signal(&rwlock->write_cond);
    } else {
        pthread_cond_broadcast(&rwlock->read_cond);
    }
    
    pthread_mutex_unlock(&rwlock->mutex);
}

void rwlock_destroy(rwlock_t *rwlock) {
    pthread_cond_destroy(&rwlock->write_cond);
    pthread_cond_destroy(&rwlock->read_cond);
    pthread_mutex_destroy(&rwlock->mutex);
}
