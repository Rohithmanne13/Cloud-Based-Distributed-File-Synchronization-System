/*
 * =============================================================================
 * metadata.c — Shared Memory Metadata & Edit Lock Management
 * =============================================================================
 * Concept: Data Consistency (Mandatory Concept 4.4)
 *          IPC — Shared Memory (Mandatory Concept 4.6)
 *
 * All metadata operations are protected by semaphore to prevent:
 * - Race conditions
 * - Dirty reads
 * - Lost updates
 *
 * Features:
 * - File metadata CRUD (original)
 * - Persistent edit locks with stale-lock timeout (Write Lock Redesign)
 * - Edit lock management (acquire/release/check)
 * =============================================================================
 */

#include "../include/metadata.h"
#include "../include/sync.h"

/* Initialize all metadata slots to empty */
void init_metadata(FileMetadata *meta_array) {
    memset(meta_array, 0, sizeof(FileMetadata) * MAX_FILES);
    /* Initialize all lock fields to unlocked */
    for (int i = 0; i < MAX_FILES; i++) {
        meta_array[i].locked_by_user_id = -1;
    }
    LOG_INFO("Metadata array initialized (%d slots, locks cleared)", MAX_FILES);
}

/* Add a new file entry — semaphore-protected critical section */
int add_file_metadata(FileMetadata *meta_array, const char *filename,
                      const char *owner_name, int owner_id, int size, int semid) {
    sem_lock(semid);  /* ---- CRITICAL SECTION START ---- */

    /* Check if file already exists */
    for (int i = 0; i < MAX_FILES; i++) {
        if (meta_array[i].is_active &&
            strcmp(meta_array[i].filename, filename) == 0) {
            /* File exists — update version instead */
            meta_array[i].version++;
            meta_array[i].file_size = size;
            meta_array[i].last_modified = time(NULL);
            LOG_INFO("Metadata updated: %s (v%d, %d bytes)",
                     filename, meta_array[i].version, size);
            sem_unlock(semid);  /* ---- CRITICAL SECTION END ---- */
            return 0;
        }
    }

    /* Find empty slot */
    for (int i = 0; i < MAX_FILES; i++) {
        if (!meta_array[i].is_active) {
            strncpy(meta_array[i].filename, filename, MAX_FILENAME - 1);
            strncpy(meta_array[i].owner_name, owner_name, MAX_USERNAME - 1);
            meta_array[i].owner_id = owner_id;
            meta_array[i].version = 1;
            meta_array[i].file_size = size;
            meta_array[i].is_active = 1;
            meta_array[i].created = time(NULL);
            meta_array[i].last_modified = time(NULL);
            meta_array[i].locked_by_user_id = -1;  /* Unlocked by default */
            LOG_INFO("Metadata added: %s (owner: %s, %d bytes)",
                     filename, owner_name, size);
            sem_unlock(semid);  /* ---- CRITICAL SECTION END ---- */
            return 0;
        }
    }

    sem_unlock(semid);  /* ---- CRITICAL SECTION END ---- */
    LOG_ERROR("Metadata full — cannot add %s", filename);
    return -1;
}

/* Update file version and size — semaphore-protected */
int update_file_metadata(FileMetadata *meta_array, const char *filename,
                         int size, int semid) {
    sem_lock(semid);

    for (int i = 0; i < MAX_FILES; i++) {
        if (meta_array[i].is_active &&
            strcmp(meta_array[i].filename, filename) == 0) {
            meta_array[i].version++;
            meta_array[i].file_size = size;
            meta_array[i].last_modified = time(NULL);
            LOG_INFO("Metadata updated: %s → v%d", filename, meta_array[i].version);
            sem_unlock(semid);
            return 0;
        }
    }

    sem_unlock(semid);
    LOG_WARN("Metadata not found for update: %s", filename);
    return -1;
}

/* Mark file as deleted — semaphore-protected */
int remove_file_metadata(FileMetadata *meta_array, const char *filename,
                         int semid) {
    sem_lock(semid);

    for (int i = 0; i < MAX_FILES; i++) {
        if (meta_array[i].is_active &&
            strcmp(meta_array[i].filename, filename) == 0) {
            meta_array[i].is_active = 0;
            meta_array[i].locked_by_user_id = -1;  /* Clear any lock */
            LOG_INFO("Metadata removed: %s", filename);
            sem_unlock(semid);
            return 0;
        }
    }

    sem_unlock(semid);
    LOG_WARN("Metadata not found for removal: %s", filename);
    return -1;
}

/* Find file metadata — semaphore-protected read */
FileMetadata* find_file_metadata(FileMetadata *meta_array,
                                 const char *filename, int semid) {
    sem_lock(semid);

    for (int i = 0; i < MAX_FILES; i++) {
        if (meta_array[i].is_active &&
            strcmp(meta_array[i].filename, filename) == 0) {
            sem_unlock(semid);
            return &meta_array[i];
        }
    }

    sem_unlock(semid);
    return NULL;
}

/* List all active files — semaphore-protected */
int list_all_files(FileMetadata *meta_array, char *output,
                   int max_size, int semid) {
    sem_lock(semid);

    int offset = 0;
    int count = 0;

    offset += snprintf(output + offset, max_size - offset,
        "\n╔════════════════════╦═══════╦════════╦════════════════╦═════════════════╗\n"
        "║ Filename           ║ Ver   ║ Size   ║ Owner          ║ Lock Status     ║\n"
        "╠════════════════════╬═══════╬════════╬════════════════╬═════════════════╣\n");

    for (int i = 0; i < MAX_FILES && offset < max_size - 120; i++) {
        if (meta_array[i].is_active) {
            char lock_buf[40];
            if (meta_array[i].locked_by_user_id != -1) {
                snprintf(lock_buf, sizeof(lock_buf), "LOCKED: %s",
                         meta_array[i].locked_by_username);
            } else {
                snprintf(lock_buf, sizeof(lock_buf), "unlocked");
            }
            offset += snprintf(output + offset, max_size - offset,
                "║ %-18s ║ v%-4d ║ %5dB ║ %-14s ║ %-15s ║\n",
                meta_array[i].filename,
                meta_array[i].version,
                meta_array[i].file_size,
                meta_array[i].owner_name,
                lock_buf);
            count++;
        }
    }

    if (count == 0) {
        offset += snprintf(output + offset, max_size - offset,
            "║              (no files stored)                                        ║\n");
    }

    snprintf(output + offset, max_size - offset,
        "╚════════════════════╩═══════╩════════╩════════════════╩═════════════════╝\n"
        "  Total files: %d\n", count);

    sem_unlock(semid);
    return count;
}

/* Get version info for a specific file — semaphore-protected */
int get_file_version(FileMetadata *meta_array, const char *filename,
                     char *output, int max_size, int semid) {
    sem_lock(semid);

    for (int i = 0; i < MAX_FILES; i++) {
        if (meta_array[i].is_active &&
            strcmp(meta_array[i].filename, filename) == 0) {
            char created_str[64], modified_str[64];
            strftime(created_str, sizeof(created_str), "%Y-%m-%d %H:%M:%S",
                     localtime(&meta_array[i].created));
            strftime(modified_str, sizeof(modified_str), "%Y-%m-%d %H:%M:%S",
                     localtime(&meta_array[i].last_modified));

            const char *lock_info = "Unlocked";
            char lock_buf[64];
            if (meta_array[i].locked_by_user_id != -1) {
                snprintf(lock_buf, sizeof(lock_buf), "Locked by %s",
                         meta_array[i].locked_by_username);
                lock_info = lock_buf;
            }

            char size_str[32];
            snprintf(size_str, sizeof(size_str), "%d bytes", meta_array[i].file_size);

            snprintf(output, max_size,
                "\n╔═══════════════════════════════════════╗\n"
                "║         FILE VERSION INFO             ║\n"
                "╠═══════════════════════════════════════╣\n"
                "║ File:     %-27s ║\n"
                "║ Version:  %-27d ║\n"
                "║ Size:     %-27s ║\n"
                "║ Owner:    %-27s ║\n"
                "║ Created:  %-27s ║\n"
                "║ Modified: %-27s ║\n"
                "║ Lock:     %-27s ║\n"
                "╚═══════════════════════════════════════╝\n",
                meta_array[i].filename,
                meta_array[i].version,
                size_str,
                meta_array[i].owner_name,
                created_str, modified_str, lock_info);

            sem_unlock(semid);
            return 0;
        }
    }

    sem_unlock(semid);
    snprintf(output, max_size, "File '%s' not found.\n", filename);
    return -1;
}

/* =============================================================================
 * EDIT LOCK — Persistent application-level file locking
 *
 * A user acquires an edit lock via EDIT command. The lock persists until
 * they SAVE or UNLOCK. Other users are denied access to modify the file.
 * Stale locks (held > LOCK_TIMEOUT_SECS) are auto-released.
 * =============================================================================
 */

/*
 * acquire_edit_lock — Try to lock a file for editing
 * Returns: 0 = lock acquired, -1 = file not found, -2 = already locked
 */
int acquire_edit_lock(FileMetadata *meta_array, const char *filename,
                      int user_id, const char *username, int semid) {
    sem_lock(semid);

    for (int i = 0; i < MAX_FILES; i++) {
        if (meta_array[i].is_active &&
            strcmp(meta_array[i].filename, filename) == 0) {

            /* Check if already locked */
            if (meta_array[i].locked_by_user_id != -1) {
                /* Check for stale lock (timeout-based deadlock prevention) */
                time_t now = time(NULL);
                if (now - meta_array[i].lock_acquired_at > LOCK_TIMEOUT_SECS) {
                    LOG_WARN("Stale lock on '%s' by '%s' auto-released (held %lds)",
                             filename, meta_array[i].locked_by_username,
                             (long)(now - meta_array[i].lock_acquired_at));
                    /* Fall through to acquire */
                } else if (meta_array[i].locked_by_user_id == user_id) {
                    /* Same user already holds lock — allow re-entry */
                    sem_unlock(semid);
                    return 0;
                } else {
                    /* Another user holds the lock — deny */
                    sem_unlock(semid);
                    return -2;
                }
            }

            /* Acquire the lock */
            meta_array[i].locked_by_user_id = user_id;
            strncpy(meta_array[i].locked_by_username, username, MAX_USERNAME - 1);
            meta_array[i].lock_acquired_at = time(NULL);
            LOG_INFO("Edit lock acquired: '%s' by '%s' (uid: %d)",
                     filename, username, user_id);

            sem_unlock(semid);
            return 0;
        }
    }

    sem_unlock(semid);
    return -1;  /* File not found */
}

/*
 * release_edit_lock — Release the edit lock on a file
 * Only the lock holder (or admin override) can release.
 * Returns: 0 = released, -1 = file not found, -2 = not lock holder
 */
int release_edit_lock(FileMetadata *meta_array, const char *filename,
                      int user_id, int semid) {
    sem_lock(semid);

    for (int i = 0; i < MAX_FILES; i++) {
        if (meta_array[i].is_active &&
            strcmp(meta_array[i].filename, filename) == 0) {

            if (meta_array[i].locked_by_user_id == -1) {
                sem_unlock(semid);
                return 0;  /* Already unlocked */
            }

            if (meta_array[i].locked_by_user_id != user_id) {
                sem_unlock(semid);
                return -2;  /* Not the lock holder */
            }

            meta_array[i].locked_by_user_id = -1;
            memset(meta_array[i].locked_by_username, 0, MAX_USERNAME);
            meta_array[i].lock_acquired_at = 0;
            LOG_INFO("Edit lock released: '%s' (uid: %d)", filename, user_id);

            sem_unlock(semid);
            return 0;
        }
    }

    sem_unlock(semid);
    return -1;
}

/*
 * check_edit_lock — Check if a file is currently edit-locked
 * Returns: 0 = unlocked, 1 = locked (fills locker_name)
 */
int check_edit_lock(FileMetadata *meta_array, const char *filename,
                    char *locker_name, int semid) {
    sem_lock(semid);

    for (int i = 0; i < MAX_FILES; i++) {
        if (meta_array[i].is_active &&
            strcmp(meta_array[i].filename, filename) == 0) {

            if (meta_array[i].locked_by_user_id != -1) {
                /* Check for stale lock */
                time_t now = time(NULL);
                if (now - meta_array[i].lock_acquired_at > LOCK_TIMEOUT_SECS) {
                    meta_array[i].locked_by_user_id = -1;
                    sem_unlock(semid);
                    return 0;  /* Stale lock auto-released */
                }
                if (locker_name) {
                    strncpy(locker_name, meta_array[i].locked_by_username,
                            MAX_USERNAME - 1);
                }
                sem_unlock(semid);
                return 1;  /* Locked */
            }

            sem_unlock(semid);
            return 0;  /* Unlocked */
        }
    }

    sem_unlock(semid);
    return 0;
}

