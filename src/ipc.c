/*
 * =============================================================================
 * ipc.c — Inter-Process Communication Implementation
 * =============================================================================
 * Concept: IPC — Message Queues, Shared Memory & Named Pipes (Concept 4.6)
 *
 * IPC Mechanisms used:
 * 1. System V Message Queues — task distribution (server → worker) and
 *    response delivery (worker → server)
 * 2. System V Shared Memory — file metadata
 * 3. Named Pipe (FIFO) — audit log delivery (worker → audit logger)
 * =============================================================================
 */

#include "../include/ipc.h"

/* ========================= Message Queue ========================= */

/*
 * create_task_queue — Create System V message queue for task distribution
 * Server enqueues tasks; workers dequeue and process them.
 */
int create_task_queue(void) {
    int mqid = msgget(TASK_QUEUE_KEY, IPC_CREAT | 0666);
    if (mqid < 0) {
        LOG_ERROR("Failed to create task message queue: %s", strerror(errno));
        return -1;
    }
    LOG_INFO("Task message queue created (ID: %d)", mqid);
    return mqid;
}

/*
 * create_response_queue — Create System V message queue for responses
 * Workers send responses; server handler threads receive them (filtered by request_id).
 */
int create_response_queue(void) {
    int mqid = msgget(RESP_QUEUE_KEY, IPC_CREAT | 0666);
    if (mqid < 0) {
        LOG_ERROR("Failed to create response message queue: %s", strerror(errno));
        return -1;
    }
    LOG_INFO("Response message queue created (ID: %d)", mqid);
    return mqid;
}

/*
 * send_task — Enqueue a task into the task message queue
 * mtype is set to 1 so any worker can dequeue it.
 */
int send_task(int mqid, TaskMessage *task) {
    task->mtype = 1;  /* All tasks have mtype=1 for worker pool distribution */
    size_t msg_size = sizeof(TaskMessage) - sizeof(long);

    if (msgsnd(mqid, task, msg_size, 0) < 0) {
        LOG_ERROR("Failed to send task to queue: %s", strerror(errno));
        return -1;
    }
    LOG_DEBUG("Task enqueued (op: %d, file: %s, req_id: %d)",
              task->operation, task->filename, task->request_id);
    return 0;
}

/*
 * receive_task — Dequeue a task from the task message queue
 * Workers call this in a loop. Blocks until a task is available.
 * Returns: 0 on success, -2 if interrupted by signal, -1 on error.
 */
int receive_task(int mqid, TaskMessage *task) {
    size_t msg_size = sizeof(TaskMessage) - sizeof(long);

    if (msgrcv(mqid, task, msg_size, 1, 0) < 0) {
        if (errno == EINTR || errno == EIDRM) {
            return -2;  /* Interrupted or queue removed — graceful exit */
        }
        LOG_ERROR("Failed to receive task from queue: %s", strerror(errno));
        return -1;
    }
    LOG_DEBUG("Task dequeued (op: %d, file: %s, req_id: %d)",
              task->operation, task->filename, task->request_id);
    return 0;
}

/*
 * send_response — Enqueue a response into the response message queue
 * mtype is set to request_id so the correct server thread can receive it.
 */
int send_response(int mqid, ResponseMessage *resp) {
    size_t msg_size = sizeof(ResponseMessage) - sizeof(long);

    if (msgsnd(mqid, resp, msg_size, 0) < 0) {
        LOG_ERROR("Failed to send response to queue: %s", strerror(errno));
        return -1;
    }
    LOG_DEBUG("Response enqueued (mtype/req_id: %ld, status: %d)",
              resp->mtype, resp->status);
    return 0;
}

/*
 * receive_response — Dequeue a response filtered by request_id
 * The server handler thread blocks until the matching response arrives.
 */
int receive_response(int mqid, int request_id, ResponseMessage *resp) {
    size_t msg_size = sizeof(ResponseMessage) - sizeof(long);

    if (msgrcv(mqid, resp, msg_size, (long)request_id, 0) < 0) {
        LOG_ERROR("Failed to receive response (req_id: %d): %s",
                  request_id, strerror(errno));
        return -1;
    }
    LOG_DEBUG("Response dequeued (req_id: %d, status: %d)",
              request_id, resp->status);
    return 0;
}

/* Clean up a message queue */
void cleanup_message_queue(int mqid) {
    if (mqid < 0) return;
    if (msgctl(mqid, IPC_RMID, NULL) < 0) {
        LOG_ERROR("Failed to remove message queue %d: %s", mqid, strerror(errno));
    } else {
        LOG_INFO("Message queue %d removed", mqid);
    }
}

/* ========================= Shared Memory (File Metadata) ========= */

/*
 * create_shared_memory — Allocate System V shared memory segment
 * Holds an array of FileMetadata[MAX_FILES].
 */
int create_shared_memory(int *shmid) {
    size_t shm_size = sizeof(FileMetadata) * MAX_FILES;

    *shmid = shmget(SHM_KEY, shm_size, IPC_CREAT | 0666);
    if (*shmid < 0) {
        LOG_ERROR("Failed to create shared memory: %s", strerror(errno));
        return -1;
    }
    LOG_INFO("Shared memory created (ID: %d, size: %zu bytes)", *shmid, shm_size);
    return 0;
}

/* Attach to existing shared memory segment */
FileMetadata* attach_shared_memory(int shmid) {
    void *ptr = shmat(shmid, NULL, 0);
    if (ptr == (void *)-1) {
        LOG_ERROR("Failed to attach shared memory: %s", strerror(errno));
        return NULL;
    }
    LOG_INFO("Shared memory attached at %p", ptr);
    return (FileMetadata *)ptr;
}

/* Detach from shared memory segment */
void detach_shared_memory(FileMetadata *ptr) {
    if (shmdt(ptr) < 0) {
        LOG_ERROR("Failed to detach shared memory: %s", strerror(errno));
    }
}

/* Destroy shared memory segment */
void cleanup_shared_memory(int shmid) {
    if (shmid < 0) return;
    if (shmctl(shmid, IPC_RMID, NULL) < 0) {
        LOG_ERROR("Failed to remove shared memory %d: %s", shmid, strerror(errno));
    } else {
        LOG_INFO("Shared memory %d removed", shmid);
    }
}


/* ========================= Named Pipe — Audit Log ================ */
/*
 * Concept: IPC via Named Pipe (FIFO)
 *
 * Workers write audit entries to a named pipe (FIFO).
 * A dedicated audit logger thread in the server reads from the pipe
 * and appends entries to audit.log on disk.
 *
 * This demonstrates a THIRD IPC mechanism (in addition to message
 * queues and shared memory) as required by the project guidelines.
 */

/* Create the named pipe (FIFO) */
int create_audit_pipe(void) {
    /* Remove any stale pipe */
    unlink(AUDIT_PIPE_PATH);

    if (mkfifo(AUDIT_PIPE_PATH, 0666) < 0) {
        LOG_ERROR("Failed to create audit pipe %s: %s",
                  AUDIT_PIPE_PATH, strerror(errno));
        return -1;
    }
    LOG_INFO("Audit named pipe created: %s", AUDIT_PIPE_PATH);
    return 0;
}

/* Remove the named pipe */
void cleanup_audit_pipe(void) {
    unlink(AUDIT_PIPE_PATH);
    LOG_INFO("Audit pipe removed");
}

/*
 * write_audit_entry — Write a structured log entry to the named pipe
 * Called by worker processes after each operation.
 * Format: [timestamp] username OPERATION filename → RESULT
 */
void write_audit_entry(int audit_fd, const char *username, const char *operation,
                       const char *filename, const char *result) {
    if (audit_fd < 0) return;  /* Pipe not available — skip silently */

    char entry[MAX_AUDIT_ENTRY];
    time_t now = time(NULL);
    char time_str[32];
    strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", localtime(&now));

    int len = snprintf(entry, sizeof(entry),
                       "[%s] %-10s %-10s %-20s → %s\n",
                       time_str, username, operation,
                       filename[0] ? filename : "(none)", result);

    if (len > 0) {
        /* Atomic write (< PIPE_BUF = 4096) — safe for concurrent writers */
        ssize_t ret = write(audit_fd, entry, len);
        (void)ret;  /* Fire-and-forget: don't block on audit failures */
    }
}

/*
 * read_audit_log — Read the last entries from audit.log file
 * Returns the content in output buffer for the AUDIT command.
 */
int read_audit_log(char *output, int max_size) {
    FILE *fp = fopen(AUDIT_LOG_PATH, "r");
    if (!fp) {
        snprintf(output, max_size, "No audit log available yet.\n");
        return 0;
    }

    /* Read all lines, keep last N that fit */
    int offset = 0;
    offset += snprintf(output + offset, max_size - offset,
        "\n╔══════════════════════════════════════════════════════════════════════════╗\n"
        "║                              AUDIT LOG                                ║\n"
        "╠══════════════════════════════════════════════════════════════════════════╣\n");

    char line[MAX_AUDIT_ENTRY];
    int count = 0;
    while (fgets(line, sizeof(line), fp) && offset < max_size - MAX_AUDIT_ENTRY) {
        line[strcspn(line, "\n")] = '\0';
        offset += snprintf(output + offset, max_size - offset,
                           "║ %-72s ║\n", line);
        count++;
    }

    if (count == 0) {
        offset += snprintf(output + offset, max_size - offset,
            "║                          (no entries yet)                              ║\n");
    }

    snprintf(output + offset, max_size - offset,
        "╚══════════════════════════════════════════════════════════════════════════╝\n"
        " Total entries: %d\n", count);

    fclose(fp);
    return count;
}
