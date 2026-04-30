/*
 * =============================================================================
 * sync.c — Synchronization Primitives Implementation
 * =============================================================================
 * Concept: Concurrency Control (Mandatory Concept 4.3)
 *          Data Consistency   (Mandatory Concept 4.4)
 *
 * Uses System V semaphores as binary semaphores (mutex behavior) to:
 * - Protect shared memory (file metadata) from race conditions
 * - Prevent dirty reads and lost updates
 * =============================================================================
 */

#include "../include/sync.h"

/*
 * create_semaphore — Create and initialize a binary semaphore
 * Initial value = 1 (unlocked). Acts as a mutex for shared memory.
 */
int create_semaphore(void) {
    int semid = semget(SEM_KEY, 1, IPC_CREAT | 0666);
    if (semid < 0) {
        LOG_ERROR("Failed to create semaphore: %s", strerror(errno));
        return -1;
    }

    /* Initialize to 1 (binary semaphore = unlocked) */
    union semun arg;
    arg.val = 1;
    if (semctl(semid, 0, SETVAL, arg) < 0) {
        LOG_ERROR("Failed to initialize semaphore: %s", strerror(errno));
        return -1;
    }

    LOG_INFO("Semaphore created and initialized to 1 (ID: %d)", semid);
    return semid;
}

/*
 * sem_lock — P (wait/proberen) operation
 * Decrements semaphore. Blocks if value is 0 (another process holds it).
 * SEM_UNDO ensures cleanup if process terminates unexpectedly.
 */
void sem_lock(int semid) {
    struct sembuf sb;
    sb.sem_num = 0;
    sb.sem_op  = -1;        /* Decrement (lock) */
    sb.sem_flg = SEM_UNDO;  /* Undo on process exit for safety */

    if (semop(semid, &sb, 1) < 0) {
        if (errno != EINTR && errno != EIDRM) {
            LOG_ERROR("Semaphore lock (P) failed: %s", strerror(errno));
        }
    }
}

/*
 * sem_unlock — V (signal/verhogen) operation
 * Increments semaphore. Wakes up a waiting process if any.
 */
void sem_unlock(int semid) {
    struct sembuf sb;
    sb.sem_num = 0;
    sb.sem_op  = 1;         /* Increment (unlock) */
    sb.sem_flg = SEM_UNDO;

    if (semop(semid, &sb, 1) < 0) {
        if (errno != EINTR && errno != EIDRM) {
            LOG_ERROR("Semaphore unlock (V) failed: %s", strerror(errno));
        }
    }
}

/* Destroy the semaphore set */
void cleanup_semaphore(int semid) {
    if (semid < 0) return;
    if (semctl(semid, 0, IPC_RMID) < 0) {
        LOG_ERROR("Failed to remove semaphore %d: %s", semid, strerror(errno));
    } else {
        LOG_INFO("Semaphore %d removed", semid);
    }
}
