/*
 * =============================================================================
 * worker.c — Worker Pool Process (Task Executor)
 * =============================================================================
 * Concept: Concurrency Control  (Mandatory Concept 4.3)
 *          IPC — Message Queue  (Mandatory Concept 4.6)
 *          File Locking         (Mandatory Concept 4.2)
 *
 * Workers are pre-forked by the server and run an infinite loop:
 *   while(1) { msgrcv(task) → process → msgsnd(response) }
 *
 * Each worker:
 * - Dequeues tasks from the shared task message queue
 * - Performs file operations with fcntl locking
 * - Updates shared memory metadata with semaphore protection
 * - Sends response back via the response message queue
 * - Writes audit entries to the named pipe (FIFO)
 *
 * Advanced Features Handled:
 * - Edit Lock (EDIT/SAVE/UNLOCK)
 * - Version History (HISTORY/ROLLBACK/DELVERSION)
 * - Audit Log (write to named pipe after each op)
 * =============================================================================
 */

#include "../include/common.h"
#include "../include/ipc.h"
#include "../include/sync.h"
#include "../include/file_manager.h"
#include "../include/metadata.h"
#include "../include/auth.h"

/* Global state for signal handling */
static volatile sig_atomic_t worker_running = 1;

/* Signal handler for graceful shutdown */
static void worker_signal_handler(int sig) {
    (void)sig;
    worker_running = 0;
}

/* Worker context — avoids passing many parameters */
typedef struct {
    FileMetadata *metadata;
    int           semid;
    int           audit_fd;
} WorkerContext;

/*
 * process_task — Execute a single task and build the response
 */
static void process_task(TaskMessage *task, ResponseMessage *resp,
                         WorkerContext *ctx) {
    /* Initialize response */
    memset(resp, 0, sizeof(ResponseMessage));
    resp->mtype = (long)task->request_id;

    const char *op_str = operation_to_string(task->operation);

    switch (task->operation) {

    /* =================================================================
     * UPLOAD — with Version History
     * ================================================================= */
    case OP_UPLOAD: {
        LOG_INFO("Processing UPLOAD: %s (%d bytes) by user '%s'",
                 task->filename, task->data_size, task->username);


        /* --- Check edit lock (deny upload if another user has lock) --- */
        char locker[MAX_USERNAME] = {0};
        if (check_edit_lock(ctx->metadata, task->filename, locker, ctx->semid)) {
            FileMetadata *fm = find_file_metadata(ctx->metadata, task->filename, ctx->semid);
            if (fm && fm->locked_by_user_id != task->user_id) {
                resp->status = STATUS_FILE_LOCKED;
                snprintf(resp->message, MAX_MSG_SIZE,
                         "Cannot upload '%s' — being edited by %s",
                         task->filename, locker);
                write_audit_entry(ctx->audit_fd, task->username, op_str,
                                  task->filename, "LOCKED");
                break;
            }
        }

        /* --- Check ownership (deny overwrite if not owner/admin) --- */
        FileMetadata *existing = find_file_metadata(ctx->metadata, task->filename, ctx->semid);
        if (existing && task->role != ROLE_ADMIN && existing->owner_id != task->user_id) {
            resp->status = STATUS_PERMISSION_DENIED;
            snprintf(resp->message, MAX_MSG_SIZE,
                     "Cannot overwrite '%s' — owned by %s",
                     task->filename, existing->owner_name);
            write_audit_entry(ctx->audit_fd, task->username, op_str,
                              task->filename, "DENIED");
            break;
        }

        /* --- Save current version before overwrite --- */
        if (existing) {
            save_version(task->filename, existing->version);
        }


// /* 🔥 ADD THIS EXACTLY HERE */
// printf("DEBUG: Backup done, sleeping before write...\n");
// sleep(10);

        /* --- Write new file (fcntl locked) --- */
        if (save_file(task->filename, task->data, task->data_size) < 0) {
            resp->status = STATUS_ERROR;
            snprintf(resp->message, MAX_MSG_SIZE,
                     "Upload failed for '%s'", task->filename);
            write_audit_entry(ctx->audit_fd, task->username, op_str,
                              task->filename, "ERROR");
            break;
        }

        /* --- Update metadata (semaphore-protected) --- */

        if (add_file_metadata(ctx->metadata, task->filename, task->username,
                              task->user_id, task->data_size, ctx->semid) < 0) {
            remove_file_from_storage(task->filename);
            resp->status = STATUS_ERROR;
            snprintf(resp->message, MAX_MSG_SIZE,
                     "Metadata error for '%s'", task->filename);
            write_audit_entry(ctx->audit_fd, task->username, op_str,
                              task->filename, "ERROR");
            break;
        }


        resp->status = STATUS_SUCCESS;
        snprintf(resp->message, MAX_MSG_SIZE,
                 "File '%s' uploaded successfully (version updated)",
                 task->filename);
        write_audit_entry(ctx->audit_fd, task->username, op_str,
                          task->filename, "SUCCESS");
        break;
    }

    /* =================================================================
     * DOWNLOAD
     * ================================================================= */
    case OP_DOWNLOAD: {
        LOG_INFO("Processing DOWNLOAD: %s by user '%s'",
                 task->filename, task->username);

        if (!find_file_metadata(ctx->metadata, task->filename, ctx->semid)) {
            resp->status = STATUS_FILE_NOT_FOUND;
            snprintf(resp->message, MAX_MSG_SIZE,
                     "File '%s' not found", task->filename);
            write_audit_entry(ctx->audit_fd, task->username, op_str,
                              task->filename, "NOT_FOUND");
            break;
        }

        int file_size = 0;
        if (read_file(task->filename, resp->data, &file_size) < 0) {
            resp->status = STATUS_ERROR;
            snprintf(resp->message, MAX_MSG_SIZE,
                     "Failed to read file '%s'", task->filename);
            write_audit_entry(ctx->audit_fd, task->username, op_str,
                              task->filename, "ERROR");
            break;
        }

        resp->status = STATUS_SUCCESS;
        resp->data_size = file_size;
        snprintf(resp->message, MAX_MSG_SIZE,
                 "File '%s' downloaded (%d bytes)", task->filename, file_size);
        write_audit_entry(ctx->audit_fd, task->username, op_str,
                          task->filename, "SUCCESS");
        break;
    }

    /* =================================================================
     * DELETE
     * ================================================================= */
    case OP_DELETE: {
        LOG_INFO("Processing DELETE: %s by user '%s'",
                 task->filename, task->username);

        FileMetadata *fmeta = find_file_metadata(ctx->metadata, task->filename, ctx->semid);
        if (!fmeta) {
            resp->status = STATUS_FILE_NOT_FOUND;
            snprintf(resp->message, MAX_MSG_SIZE,
                     "File '%s' not found", task->filename);
            write_audit_entry(ctx->audit_fd, task->username, op_str,
                              task->filename, "NOT_FOUND");
            break;
        }

        /* Check ownership */
        if (task->role != ROLE_ADMIN && fmeta->owner_id != task->user_id) {
            resp->status = STATUS_PERMISSION_DENIED;
            snprintf(resp->message, MAX_MSG_SIZE,
                     "Only the file owner or admin can delete '%s'",
                     task->filename);
            write_audit_entry(ctx->audit_fd, task->username, op_str,
                              task->filename, "DENIED");
            break;
        }

        /* Check edit lock */
        if (fmeta->locked_by_user_id != -1 &&
            fmeta->locked_by_user_id != task->user_id) {
            resp->status = STATUS_FILE_LOCKED;
            snprintf(resp->message, MAX_MSG_SIZE,
                     "Cannot delete '%s' — being edited by %s",
                     task->filename, fmeta->locked_by_username);
            write_audit_entry(ctx->audit_fd, task->username, op_str,
                              task->filename, "LOCKED");
            break;
        }

        /* Remove file */
        if (remove_file_from_storage(task->filename) < 0) {
            resp->status = STATUS_ERROR;
            snprintf(resp->message, MAX_MSG_SIZE,
                     "Failed to delete file '%s'", task->filename);
            write_audit_entry(ctx->audit_fd, task->username, op_str,
                              task->filename, "ERROR");
            break;
        }

        /* Remove metadata */
        if (remove_file_metadata(ctx->metadata, task->filename, ctx->semid) < 0) {
            resp->status = STATUS_ERROR;
            snprintf(resp->message, MAX_MSG_SIZE,
                     "Metadata error during delete of '%s'", task->filename);
            write_audit_entry(ctx->audit_fd, task->username, op_str,
                              task->filename, "ERROR");
            break;
        }


        resp->status = STATUS_SUCCESS;
        snprintf(resp->message, MAX_MSG_SIZE,
                 "File '%s' deleted successfully", task->filename);
        write_audit_entry(ctx->audit_fd, task->username, op_str,
                          task->filename, "SUCCESS");
        break;
    }

    /* =================================================================
     * LIST
     * ================================================================= */
    case OP_LIST: {
        LOG_INFO("Processing LIST by user '%s'", task->username);
        list_all_files(ctx->metadata, resp->data, MAX_FILE_SIZE, ctx->semid);
        resp->data_size = strlen(resp->data);
        resp->status = STATUS_SUCCESS;
        snprintf(resp->message, MAX_MSG_SIZE, "File listing retrieved");
        break;
    }

    /* =================================================================
     * VERSION
     * ================================================================= */
    case OP_VERSION: {
        LOG_INFO("Processing VERSION: %s by user '%s'",
                 task->filename, task->username);
        if (get_file_version(ctx->metadata, task->filename,
                             resp->data, MAX_FILE_SIZE, ctx->semid) < 0) {
            resp->status = STATUS_FILE_NOT_FOUND;
            snprintf(resp->message, MAX_MSG_SIZE,
                     "File '%s' not found", task->filename);
        } else {
            resp->data_size = strlen(resp->data);
            resp->status = STATUS_SUCCESS;
            snprintf(resp->message, MAX_MSG_SIZE,
                     "Version info for '%s'", task->filename);
        }
        break;
    }

    /* =================================================================
     * EDIT — Acquire edit lock + download file
     * ================================================================= */
    case OP_EDIT: {
        LOG_INFO("Processing EDIT: %s by user '%s'",
                 task->filename, task->username);

        /* Check ownership before allowing edit */
        FileMetadata *efm = find_file_metadata(ctx->metadata, task->filename, ctx->semid);
        if (!efm) {
            resp->status = STATUS_FILE_NOT_FOUND;
            snprintf(resp->message, MAX_MSG_SIZE,
                     "File '%s' not found", task->filename);
            write_audit_entry(ctx->audit_fd, task->username, op_str,
                              task->filename, "NOT_FOUND");
            break;
        }
        if (task->role != ROLE_ADMIN && efm->owner_id != task->user_id) {
            resp->status = STATUS_PERMISSION_DENIED;
            snprintf(resp->message, MAX_MSG_SIZE,
                     "Cannot edit '%s' — owned by %s",
                     task->filename, efm->owner_name);
            write_audit_entry(ctx->audit_fd, task->username, op_str,
                              task->filename, "DENIED");
            break;
        }

        /* Try to acquire edit lock */
        int lock_result = acquire_edit_lock(ctx->metadata, task->filename,
                                            task->user_id, task->username,
                                            ctx->semid);
        if (lock_result == -1) {
            resp->status = STATUS_FILE_NOT_FOUND;
            snprintf(resp->message, MAX_MSG_SIZE,
                     "File '%s' not found", task->filename);
            write_audit_entry(ctx->audit_fd, task->username, op_str,
                              task->filename, "NOT_FOUND");
            break;
        }
        if (lock_result == -2) {
            /* File is locked by someone else */
            char locker[MAX_USERNAME] = {0};
            check_edit_lock(ctx->metadata, task->filename, locker, ctx->semid);
            resp->status = STATUS_FILE_LOCKED;
            snprintf(resp->message, MAX_MSG_SIZE,
                     "File '%s' is being edited by %s",
                     task->filename, locker);
            write_audit_entry(ctx->audit_fd, task->username, op_str,
                              task->filename, "LOCKED");
            break;
        }

        /* Lock acquired — now download the file content */
        int file_size = 0;
        if (read_file(task->filename, resp->data, &file_size) < 0) {
            release_edit_lock(ctx->metadata, task->filename,
                              task->user_id, ctx->semid);
            resp->status = STATUS_ERROR;
            snprintf(resp->message, MAX_MSG_SIZE,
                     "Failed to read file '%s'", task->filename);
            write_audit_entry(ctx->audit_fd, task->username, op_str,
                              task->filename, "ERROR");
            break;
        }

        resp->status = STATUS_SUCCESS;
        resp->data_size = file_size;
        snprintf(resp->message, MAX_MSG_SIZE,
                 "Edit lock acquired for '%s' (%d bytes). Use SAVE to commit or UNLOCK to cancel.",
                 task->filename, file_size);
        write_audit_entry(ctx->audit_fd, task->username, op_str,
                          task->filename, "LOCK_ACQUIRED");
        break;
    }

    /* =================================================================
     * SAVE — Upload edited file + release lock
     * ================================================================= */
    case OP_SAVE: {
        LOG_INFO("Processing SAVE: %s (%d bytes) by user '%s'",
                 task->filename, task->data_size, task->username);

        if (task->role == ROLE_GUEST) {
            resp->status = STATUS_PERMISSION_DENIED;
            snprintf(resp->message, MAX_MSG_SIZE,
                     "Permission denied: Guests cannot SAVE files");
            write_audit_entry(ctx->audit_fd, task->username, op_str,
                              task->filename, "DENIED");
            break;
        }

        /* Verify caller holds the edit lock */
        FileMetadata *fm = find_file_metadata(ctx->metadata, task->filename, ctx->semid);
        if (!fm) {
            resp->status = STATUS_FILE_NOT_FOUND;
            snprintf(resp->message, MAX_MSG_SIZE,
                     "File '%s' not found", task->filename);
            break;
        }
        if (fm->locked_by_user_id != task->user_id) {
            resp->status = STATUS_PERMISSION_DENIED;
            snprintf(resp->message, MAX_MSG_SIZE,
                     "You don't hold the edit lock for '%s'", task->filename);
            write_audit_entry(ctx->audit_fd, task->username, op_str,
                              task->filename, "NOT_LOCK_HOLDER");
            break;
        }

        /* Save version before overwrite */
        save_version(task->filename, fm->version);

        /* Write new content */
        if (save_file(task->filename, task->data, task->data_size) < 0) {
            resp->status = STATUS_ERROR;
            snprintf(resp->message, MAX_MSG_SIZE,
                     "Save failed for '%s'", task->filename);
            write_audit_entry(ctx->audit_fd, task->username, op_str,
                              task->filename, "ERROR");
            break;
        }

        /* Update metadata (version++) */
        if (update_file_metadata(ctx->metadata, task->filename,
                                 task->data_size, ctx->semid) < 0) {
            resp->status = STATUS_ERROR;
            snprintf(resp->message, MAX_MSG_SIZE,
                     "Metadata error for '%s'", task->filename);
            write_audit_entry(ctx->audit_fd, task->username, op_str,
                              task->filename, "ERROR");
            break;
        }

        /* Release edit lock */
        release_edit_lock(ctx->metadata, task->filename,
                          task->user_id, ctx->semid);


        resp->status = STATUS_SUCCESS;
        snprintf(resp->message, MAX_MSG_SIZE,
                 "File '%s' saved successfully — edit lock released",
                 task->filename);
        write_audit_entry(ctx->audit_fd, task->username, op_str,
                          task->filename, "SUCCESS");
        break;
    }

    /* =================================================================
     * UNLOCK — Release edit lock without saving
     * ================================================================= */
    case OP_UNLOCK: {
        LOG_INFO("Processing UNLOCK: %s by user '%s'",
                 task->filename, task->username);

        int result = release_edit_lock(ctx->metadata, task->filename,
                                       task->user_id, ctx->semid);
        if (result == -1) {
            resp->status = STATUS_FILE_NOT_FOUND;
            snprintf(resp->message, MAX_MSG_SIZE,
                     "File '%s' not found", task->filename);
        } else if (result == -2) {
            resp->status = STATUS_PERMISSION_DENIED;
            snprintf(resp->message, MAX_MSG_SIZE,
                     "You don't hold the edit lock for '%s'", task->filename);
        } else {
            resp->status = STATUS_SUCCESS;
            snprintf(resp->message, MAX_MSG_SIZE,
                     "Edit lock released for '%s' (changes discarded)",
                     task->filename);
        }
        write_audit_entry(ctx->audit_fd, task->username, op_str,
                          task->filename,
                          resp->status == STATUS_SUCCESS ? "UNLOCKED" : "FAILED");
        break;
    }

    /* =================================================================
     * HISTORY — View version history of a file
     * ================================================================= */
    case OP_HISTORY: {
        LOG_INFO("Processing HISTORY: %s by user '%s'",
                 task->filename, task->username);

        if (!find_file_metadata(ctx->metadata, task->filename, ctx->semid)) {
            resp->status = STATUS_FILE_NOT_FOUND;
            snprintf(resp->message, MAX_MSG_SIZE,
                     "File '%s' not found", task->filename);
            break;
        }

        list_versions(task->filename, resp->data, MAX_FILE_SIZE);
        resp->data_size = strlen(resp->data);
        resp->status = STATUS_SUCCESS;
        snprintf(resp->message, MAX_MSG_SIZE,
                 "Version history for '%s'", task->filename);
        break;
    }

    /* =================================================================
     * ROLLBACK — Restore file to a previous version
     * ================================================================= */
    case OP_ROLLBACK: {
        LOG_INFO("Processing ROLLBACK: %s to v%d by user '%s'",
                 task->filename, task->target_version, task->username);

        FileMetadata *fm = find_file_metadata(ctx->metadata, task->filename, ctx->semid);
        if (!fm) {
            resp->status = STATUS_FILE_NOT_FOUND;
            snprintf(resp->message, MAX_MSG_SIZE,
                     "File '%s' not found", task->filename);
            write_audit_entry(ctx->audit_fd, task->username, op_str,
                              task->filename, "NOT_FOUND");
            break;
        }

        /* Check ownership */
        if (task->role != ROLE_ADMIN && fm->owner_id != task->user_id) {
            resp->status = STATUS_PERMISSION_DENIED;
            snprintf(resp->message, MAX_MSG_SIZE,
                     "Cannot rollback '%s' — owned by %s",
                     task->filename, fm->owner_name);
            write_audit_entry(ctx->audit_fd, task->username, op_str,
                              task->filename, "DENIED");
            break;
        }

        /* Check edit lock */
        if (fm->locked_by_user_id != -1 &&
            fm->locked_by_user_id != task->user_id) {
            resp->status = STATUS_FILE_LOCKED;
            snprintf(resp->message, MAX_MSG_SIZE,
                     "Cannot rollback '%s' — being edited by %s",
                     task->filename, fm->locked_by_username);
            break;
        }

        /* Save current version before rollback */
        save_version(task->filename, fm->version);

        /* Restore the requested version */
        int new_size = restore_version(task->filename, task->target_version);
        if (new_size < 0) {
            resp->status = STATUS_ERROR;
            snprintf(resp->message, MAX_MSG_SIZE,
                     "Version %d not found for '%s'",
                     task->target_version, task->filename);
            write_audit_entry(ctx->audit_fd, task->username, op_str,
                              task->filename, "VERSION_NOT_FOUND");
            break;
        }

        /* Update metadata */
        update_file_metadata(ctx->metadata, task->filename, new_size, ctx->semid);


        resp->status = STATUS_SUCCESS;
        snprintf(resp->message, MAX_MSG_SIZE,
                 "File '%s' rolled back to version %d (%d bytes)",
                 task->filename, task->target_version, new_size);
        write_audit_entry(ctx->audit_fd, task->username, op_str,
                          task->filename, "SUCCESS");
        break;
    }


    /* =================================================================
     * AUDIT — View audit log (admin only)
     * ================================================================= */
    case OP_AUDIT: {
        LOG_INFO("Processing AUDIT by user '%s'", task->username);

        read_audit_log(resp->data, MAX_FILE_SIZE);
        resp->data_size = strlen(resp->data);
        resp->status = STATUS_SUCCESS;
        snprintf(resp->message, MAX_MSG_SIZE, "Audit log retrieved");
        break;
    }

    /* =================================================================
     * DELVERSION — Delete a specific version from history
     * ================================================================= */
    case OP_DELVERSION: {
        LOG_INFO("Processing DELVERSION: %s v%d by user '%s'",
                 task->filename, task->target_version, task->username);

        FileMetadata *fm = find_file_metadata(ctx->metadata, task->filename, ctx->semid);
        if (!fm) {
            resp->status = STATUS_FILE_NOT_FOUND;
            snprintf(resp->message, MAX_MSG_SIZE,
                     "File '%s' not found", task->filename);
            write_audit_entry(ctx->audit_fd, task->username, op_str,
                              task->filename, "NOT_FOUND");
            break;
        }

        /* Check ownership (only owner or admin can delete versions) */
        if (task->role != ROLE_ADMIN && fm->owner_id != task->user_id) {
            resp->status = STATUS_PERMISSION_DENIED;
            snprintf(resp->message, MAX_MSG_SIZE,
                     "Only the file owner or admin can delete versions of '%s'",
                     task->filename);
            write_audit_entry(ctx->audit_fd, task->username, op_str,
                              task->filename, "DENIED");
            break;
        }

        /* Delete the specific version */
        if (delete_version(task->filename, task->target_version) < 0) {
            resp->status = STATUS_ERROR;
            snprintf(resp->message, MAX_MSG_SIZE,
                     "Version %d not found for '%s'",
                     task->target_version, task->filename);
            write_audit_entry(ctx->audit_fd, task->username, op_str,
                              task->filename, "VERSION_NOT_FOUND");
            break;
        }

        resp->status = STATUS_SUCCESS;
        snprintf(resp->message, MAX_MSG_SIZE,
                 "Version %d of '%s' deleted successfully",
                 task->target_version, task->filename);
        write_audit_entry(ctx->audit_fd, task->username, op_str,
                          task->filename, "SUCCESS");
        break;
    }

    default:
        resp->status = STATUS_ERROR;
        snprintf(resp->message, MAX_MSG_SIZE, "Unknown operation: %d",
                 task->operation);
        break;
    }
}

/*
 * worker_main — Entry point for each worker process
 *
 * Called after fork() in server.c. Runs infinite dequeue-process-respond loop.
 */
void worker_main(int worker_id, int task_qid, int resp_qid,
                 int shmid, int semid) {

    /* Install signal handlers */
    signal(SIGTERM, worker_signal_handler);
    signal(SIGINT, worker_signal_handler);

    /* Attach to shared memory */
    FileMetadata *metadata = attach_shared_memory(shmid);
    if (!metadata) {
        LOG_ERROR("Worker %d: Failed to attach shared memory", worker_id);
        exit(EXIT_FAILURE);
    }


    /* Open audit named pipe for writing (IPC via FIFO) */
    int audit_fd = open(AUDIT_PIPE_PATH, O_WRONLY);
    if (audit_fd < 0) {
        LOG_WARN("Worker %d: Could not open audit pipe — auditing disabled", worker_id);
    }

    /* Build worker context */
    WorkerContext ctx;
    ctx.metadata = metadata;
    ctx.semid = semid;
    ctx.audit_fd = audit_fd;

    LOG_INFO("Worker %d started (PID: %d) — waiting for tasks...",
             worker_id, getpid());

    /* ===== Main Worker Loop ===== */
    while (worker_running) {
        TaskMessage task;
        memset(&task, 0, sizeof(task));

        /* Block until a task is available in the queue */
        int ret = receive_task(task_qid, &task);
        if (ret == -2) {
            /* Interrupted by signal — exit gracefully */
            break;
        }
        if (ret < 0) {
            LOG_ERROR("Worker %d: Error receiving task", worker_id);
            continue;
        }

        LOG_INFO("Worker %d: Picked up task (op: %s, file: %s, req_id: %d)",
                 worker_id, operation_to_string(task.operation),
                 task.filename, task.request_id);

        /* Process the task */
        ResponseMessage resp;
        process_task(&task, &resp, &ctx);

        /* Send response back via response queue */
        if (send_response(resp_qid, &resp) < 0) {
            LOG_ERROR("Worker %d: Failed to send response for req_id %d",
                      worker_id, task.request_id);
        } else {
            LOG_INFO("Worker %d: Task completed (req_id: %d, status: %d)",
                     worker_id, task.request_id, resp.status);
        }
    }

    /* Cleanup */
    if (audit_fd >= 0) close(audit_fd);
    detach_shared_memory(metadata);
    LOG_INFO("Worker %d (PID: %d) shutting down gracefully", worker_id, getpid());
    exit(EXIT_SUCCESS);
}
