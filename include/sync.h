/*
 * =============================================================================
 * sync.h — Synchronization Primitives (System V Semaphores)
 * =============================================================================
 */

#ifndef SYNC_H
#define SYNC_H

#include "common.h"

/* Union required by semctl() */
union semun {
    int val;
    struct semid_ds *buf;
    unsigned short *array;
};

/* Create and initialize a binary semaphore (mutex) */
int  create_semaphore(void);

/* Acquire the semaphore (P / wait / lock) */
void sem_lock(int semid);

/* Release the semaphore (V / signal / unlock) */
void sem_unlock(int semid);

/* Destroy the semaphore */
void cleanup_semaphore(int semid);

#endif /* SYNC_H */
