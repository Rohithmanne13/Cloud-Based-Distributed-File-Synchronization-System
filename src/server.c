/*
 * =============================================================================
 * server.c — Main Server Process
 * =============================================================================
 * Concept: Socket Programming     (Mandatory Concept 4.5)
 *          Role-Based Authorization(Mandatory Concept 4.1)
 *          IPC — Message Queue     (Mandatory Concept 4.6)
 *          IPC — Named Pipe        (Mandatory Concept 4.6)
 *          Concurrency Control     (Mandatory Concept 4.3)
 *
 * Architecture:
 * 1. Creates TCP socket, binds, listens
 * 2. Initializes IPC resources (message queues, shared memory, semaphore,
 *    audit named pipe)
 * 3. Starts audit logger thread (reads from named pipe → writes to log file)
 * 4. Pre-forks NUM_WORKERS worker processes (worker pool)
 * 5. Accepts client connections in main thread
 * 6. Spawns a pthread per client for concurrent handling
 * 7. Each client handler: authenticate → parse commands → enqueue task →
 *    wait for response → send back to client
 * 8. Signal handling for graceful shutdown
 * =============================================================================
 */

#include "../include/common.h"
#include "../include/auth.h"
#include "../include/ipc.h"
#include "../include/sync.h"
#include "../include/file_manager.h"
#include "../include/metadata.h"

/* External worker entry point */
extern void worker_main(int worker_id, int task_qid, int resp_qid,
                        int shmid, int semid);

/* ========================= Global State ========================== */
static int server_fd = -1;
static int task_qid = -1;
static int resp_qid = -1;
static int shmid = -1;
static int semid = -1;
static FileMetadata *metadata = NULL;
static pid_t worker_pids[NUM_WORKERS];
static volatile sig_atomic_t server_running = 1;
static int request_counter = 100;  /* Start above 0 for valid mtype */
static pthread_mutex_t counter_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Thread-safe request ID generator */
static int get_next_request_id(void) {
    pthread_mutex_lock(&counter_mutex);
    int id = request_counter++;
    if (request_counter > 99999) request_counter = 100;
    pthread_mutex_unlock(&counter_mutex);
    return id;
}

/* ========================= Client Context ======================== */
typedef struct {
    int client_fd;
    struct sockaddr_in client_addr;
} ClientContext;

/* ========================= Audit Logger Thread =================== */
/*
 * This thread reads from the named pipe (FIFO) and appends entries
 * to audit.log on disk. This demonstrates IPC via named pipes — a
 * separate mechanism from the message queues and shared memory.
 *
 * Flow: Worker processes → write to pipe → this thread reads → audit.log
 */
static void* audit_logger_thread(void *arg) {
    (void)arg;

    LOG_INFO("Audit logger thread started — opening pipe for reading...");

    /* Open pipe for reading (blocks until a writer opens) */
    int pipe_fd = open(AUDIT_PIPE_PATH, O_RDONLY);
    if (pipe_fd < 0) {
        LOG_ERROR("Audit logger: failed to open pipe: %s", strerror(errno));
        return NULL;
    }

    FILE *logfile = fopen(AUDIT_LOG_PATH, "a");
    if (!logfile) {
        LOG_ERROR("Audit logger: failed to open log file: %s", strerror(errno));
        close(pipe_fd);
        return NULL;
    }

    LOG_INFO("Audit logger: pipe connected, writing to %s", AUDIT_LOG_PATH);

    char buf[MAX_AUDIT_ENTRY];
    ssize_t n;
    while ((n = read(pipe_fd, buf, sizeof(buf) - 1)) > 0) {
        buf[n] = '\0';
        fprintf(logfile, "%s", buf);
        fflush(logfile);  /* Flush immediately for real-time logging */
    }

    LOG_INFO("Audit logger thread exiting (pipe closed by writers)");
    fclose(logfile);
    close(pipe_fd);
    return NULL;
}

/* ========================= Signal Handler ======================== */
static void server_signal_handler(int sig) {
    (void)sig;
    printf("\n");
    LOG_WARN("Received shutdown signal — cleaning up...");
    server_running = 0;

    /* Kill all workers */
    for (int i = 0; i < NUM_WORKERS; i++) {
        if (worker_pids[i] > 0) {
            kill(worker_pids[i], SIGTERM);
        }
    }

    /* Wait for workers to exit */
    for (int i = 0; i < NUM_WORKERS; i++) {
        if (worker_pids[i] > 0) {
            waitpid(worker_pids[i], NULL, 0);
            LOG_INFO("Worker %d (PID: %d) terminated", i, worker_pids[i]);
        }
    }

    /* Cleanup IPC resources */
    if (metadata) detach_shared_memory(metadata);
    cleanup_message_queue(task_qid);
    cleanup_message_queue(resp_qid);
    cleanup_shared_memory(shmid);
    cleanup_semaphore(semid);
    cleanup_audit_pipe();

    /* Close server socket */
    if (server_fd >= 0) close(server_fd);

    LOG_INFO("Server shutdown complete. All IPC resources cleaned.");
    exit(EXIT_SUCCESS);
}

/* ========================= Client Handler Thread ================= */

/*
 * handle_client — Thread function for each connected client
 *
 * Flow:
 * 1. Receive LOGIN request → authenticate
 * 2. Loop: receive command → check permission → enqueue task →
 *    wait for worker response → send back to client
 */
static void* handle_client(void *arg) {
    ClientContext *ctx = (ClientContext *)arg;
    int client_fd = ctx->client_fd;
    char client_ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &ctx->client_addr.sin_addr, client_ip, sizeof(client_ip));
    free(ctx);

    pthread_detach(pthread_self());

    LOG_INFO("Client connected from %s (fd: %d)", client_ip, client_fd);

    /* ---- Phase 1: Authentication (SIGNIN or SIGNUP) ---- */
    ClientRequest req;
    ServerResponse resp;

    /* Wait for LOGIN or SIGNUP request */
    memset(&req, 0, sizeof(req));
    ssize_t bytes = recv(client_fd, &req, sizeof(req), MSG_WAITALL);
    if (bytes <= 0 || (req.operation != OP_LOGIN && req.operation != OP_SIGNUP)) {
        memset(&resp, 0, sizeof(resp));
        resp.status = STATUS_AUTH_FAIL;
        snprintf(resp.message, MAX_MSG_SIZE, "Expected SIGNIN or SIGNUP as first command");
        send(client_fd, &resp, sizeof(resp), 0);
        close(client_fd);
        return NULL;
    }

    UserInfo user;
    memset(&resp, 0, sizeof(resp));

    if (req.operation == OP_SIGNUP) {
        /* ---- SIGNUP: Register a new user ---- */
        int role = req.data_size;  /* Role is passed via data_size field */

        int result = register_user(req.username, req.password, role, &user);
        if (result == -2) {
            resp.status = STATUS_USER_EXISTS;
            snprintf(resp.message, MAX_MSG_SIZE,
                     "Username '%s' already exists. Please choose another.", req.username);
            send(client_fd, &resp, sizeof(resp), 0);
            close(client_fd);
            LOG_WARN("Client %s: Signup failed — username '%s' already exists",
                     client_ip, req.username);
            return NULL;
        } else if (result < 0) {
            resp.status = STATUS_ERROR;
            snprintf(resp.message, MAX_MSG_SIZE,
                     "Registration failed due to server error");
            send(client_fd, &resp, sizeof(resp), 0);
            close(client_fd);
            LOG_ERROR("Client %s: Signup failed for '%s'", client_ip, req.username);
            return NULL;
        }

        resp.status = STATUS_SUCCESS;
        resp.data_size = user.role;
        snprintf(resp.message, MAX_MSG_SIZE,
                 "Account created! Welcome, %s! Role: %s. Type HELP for commands.",
                 user.username, role_to_string(user.role));
        send(client_fd, &resp, sizeof(resp), 0);

        LOG_INFO("Client %s: User '%s' registered and logged in (Role: %s)",
                 client_ip, user.username, role_to_string(user.role));

    } else {
        /* ---- SIGNIN: Authenticate existing user ---- */
        if (authenticate_user(req.username, req.password, &user) < 0) {
            resp.status = STATUS_AUTH_FAIL;
            snprintf(resp.message, MAX_MSG_SIZE,
                     "Authentication failed for '%s'", req.username);
            send(client_fd, &resp, sizeof(resp), 0);
            close(client_fd);
            LOG_WARN("Client %s: Authentication failed for '%s'",
                     client_ip, req.username);
            return NULL;
        }

        resp.status = STATUS_SUCCESS;
        resp.data_size = user.role;
        snprintf(resp.message, MAX_MSG_SIZE,
                 "Welcome back, %s! Role: %s. Type HELP for commands.",
                 user.username, role_to_string(user.role));
        send(client_fd, &resp, sizeof(resp), 0);

        LOG_INFO("Client %s: User '%s' signed in (Role: %s)",
                 client_ip, user.username, role_to_string(user.role));
    }

    /* ---- Phase 2: Command Loop ---- */
    while (server_running) {
        memset(&req, 0, sizeof(req));
        bytes = recv(client_fd, &req, sizeof(req), MSG_WAITALL);

        if (bytes <= 0) {
            LOG_INFO("Client %s ('%s') disconnected", client_ip, user.username);
            break;
        }

        if (req.operation == OP_EXIT) {
            memset(&resp, 0, sizeof(resp));
            resp.status = STATUS_SUCCESS;
            snprintf(resp.message, MAX_MSG_SIZE, "Signed out. Goodbye, %s!", user.username);
            send(client_fd, &resp, sizeof(resp), 0);
            LOG_INFO("Client %s ('%s') signed out", client_ip, user.username);
            break;
        }

        LOG_INFO("Client '%s': %s %s",
                 user.username, operation_to_string(req.operation), req.filename);

        /* ---- Permission Check (Role-Based Authorization) ---- */
        if (!check_permission(user.role, req.operation)) {
            memset(&resp, 0, sizeof(resp));
            resp.status = STATUS_PERMISSION_DENIED;
            snprintf(resp.message, MAX_MSG_SIZE,
                     "Permission denied: %s cannot %s",
                     role_to_string(user.role),
                     operation_to_string(req.operation));
            send(client_fd, &resp, sizeof(resp), 0);
            LOG_WARN("Permission denied: %s (%s) tried %s",
                     user.username, role_to_string(user.role),
                     operation_to_string(req.operation));
            continue;
        }

        /* ---- Enqueue Task to Message Queue ---- */
        TaskMessage task;
        memset(&task, 0, sizeof(task));
        task.mtype = 1;
        task.request_id = get_next_request_id();
        task.operation = req.operation;
        task.user_id = user.id;
        task.role = user.role;
        strncpy(task.username, user.username, MAX_USERNAME - 1);
        strncpy(task.filename, req.filename, MAX_FILENAME - 1);
        if (req.data_size > 0 && req.data_size <= MAX_FILE_SIZE) {
            memcpy(task.data, req.data, req.data_size);
            task.data_size = req.data_size;
        }

        /* For ROLLBACK/DELVERSION: pass target version (sent via data_size from client) */
        if (req.operation == OP_ROLLBACK || req.operation == OP_DELVERSION) {
            task.target_version = req.data_size;
            task.data_size = 0;
        }

        if (send_task(task_qid, &task) < 0) {
            memset(&resp, 0, sizeof(resp));
            resp.status = STATUS_ERROR;
            snprintf(resp.message, MAX_MSG_SIZE, "Server error: task queue full");
            send(client_fd, &resp, sizeof(resp), 0);
            continue;
        }

        /* ---- Wait for Worker Response ---- */
        ResponseMessage worker_resp;
        memset(&worker_resp, 0, sizeof(worker_resp));

        if (receive_response(resp_qid, task.request_id, &worker_resp) < 0) {
            memset(&resp, 0, sizeof(resp));
            resp.status = STATUS_ERROR;
            snprintf(resp.message, MAX_MSG_SIZE,
                     "Server error: no response from worker");
            send(client_fd, &resp, sizeof(resp), 0);
            continue;
        }

        /* ---- Forward Response to Client ---- */
        memset(&resp, 0, sizeof(resp));
        resp.status = worker_resp.status;
        strncpy(resp.message, worker_resp.message, MAX_MSG_SIZE - 1);
        if (worker_resp.data_size > 0) {
            memcpy(resp.data, worker_resp.data, worker_resp.data_size);
            resp.data_size = worker_resp.data_size;
        }

        send(client_fd, &resp, sizeof(resp), 0);
    }

    close(client_fd);
    return NULL;
}

/* ========================= Print Banner ========================== */
static void print_banner(void) {
    printf(COLOR_CYAN);
    printf("╔══════════════════════════════════════════════════════════╗\n");
    printf("║     Cloud-Based Distributed File Sync System            ║\n");
    printf("║     Server v2.0 | Worker Pool + Advanced Features       ║\n");
    printf("╚══════════════════════════════════════════════════════════╝\n");
    printf(COLOR_RESET);
}

/* ========================= Main ================================== */
int main(void) {
    print_banner();

    /* Install signal handlers */
    signal(SIGINT, server_signal_handler);
    signal(SIGTERM, server_signal_handler);

    /* Initialize storage directory */
    if (init_storage() < 0) {
        LOG_ERROR("Failed to initialize storage");
        return EXIT_FAILURE;
    }

    /* ---- Create IPC Resources ---- */
    LOG_INFO("Initializing IPC resources...");

    task_qid = create_task_queue();
    if (task_qid < 0) return EXIT_FAILURE;

    resp_qid = create_response_queue();
    if (resp_qid < 0) return EXIT_FAILURE;

    if (create_shared_memory(&shmid) < 0) return EXIT_FAILURE;

    metadata = attach_shared_memory(shmid);
    if (!metadata) return EXIT_FAILURE;

    init_metadata(metadata);

    semid = create_semaphore();
    if (semid < 0) return EXIT_FAILURE;


    /* ---- Create Audit Named Pipe (FIFO) — IPC mechanism #3 ---- */
    if (create_audit_pipe() < 0) {
        LOG_WARN("Audit pipe creation failed — auditing will be disabled");
    }

    /* ---- Start Audit Logger Thread ---- */
    pthread_t audit_tid;
    if (pthread_create(&audit_tid, NULL, audit_logger_thread, NULL) != 0) {
        LOG_WARN("Failed to start audit logger thread: %s", strerror(errno));
    } else {
        pthread_detach(audit_tid);
        LOG_INFO("Audit logger thread launched");
    }

    /* ---- Create TCP Socket (before fork so children can close it) ---- */
    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        LOG_ERROR("Socket creation failed: %s", strerror(errno));
        server_signal_handler(SIGTERM);
        return EXIT_FAILURE;
    }

    /* Allow port reuse */
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    /* ---- Fork Worker Pool ---- */
    LOG_INFO("Forking %d worker processes...", NUM_WORKERS);

    for (int i = 0; i < NUM_WORKERS; i++) {
        pid_t pid = fork();
        if (pid < 0) {
            LOG_ERROR("Failed to fork worker %d: %s", i, strerror(errno));
            server_signal_handler(SIGTERM);
            return EXIT_FAILURE;
        }
        if (pid == 0) {
            /* Child process — close inherited server socket, become a worker */
            close(server_fd);
            worker_main(i, task_qid, resp_qid, shmid, semid);
            exit(EXIT_SUCCESS);  /* Should not reach here */
        }
        worker_pids[i] = pid;
        LOG_INFO("Worker %d forked (PID: %d)", i, pid);
    }

    /* ---- Bind Socket ---- */
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(SERVER_PORT);

    if (bind(server_fd, (struct sockaddr *)&server_addr,
             sizeof(server_addr)) < 0) {
        LOG_ERROR("Bind failed on port %d: %s", SERVER_PORT, strerror(errno));
        server_signal_handler(SIGTERM);
        return EXIT_FAILURE;
    }

    /* Listen */
    if (listen(server_fd, MAX_CLIENTS) < 0) {
        LOG_ERROR("Listen failed: %s", strerror(errno));
        server_signal_handler(SIGTERM);
        return EXIT_FAILURE;
    }

    printf(COLOR_GREEN);
    printf("╔══════════════════════════════════════════════════════════╗\n");
    printf("║  Server listening on port %-6d                        ║\n", SERVER_PORT);
    printf("║  Workers: %d | Max clients: %-4d                       ║\n",
           NUM_WORKERS, MAX_CLIENTS);
    printf("║  IPC: MsgQueue + SharedMem + NamedPipe (Audit)          ║\n");
    printf("║  Features: EditLock | Versions | DelVer | Audit         ║\n");
    printf("║  Auth: SignUp/SignIn | RBAC (Admin/User/Guest)          ║\n");
    printf("║  Press Ctrl+C for graceful shutdown                     ║\n");
    printf("╚══════════════════════════════════════════════════════════╝\n");
    printf(COLOR_RESET);

    /* ---- Accept Loop ---- */
    while (server_running) {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);

        int client_fd = accept(server_fd,
                               (struct sockaddr *)&client_addr, &addr_len);
        if (client_fd < 0) {
            if (errno == EINTR) continue;  /* Interrupted by signal */
            LOG_ERROR("Accept failed: %s", strerror(errno));
            continue;
        }

        /* Create client context */
        ClientContext *ctx = malloc(sizeof(ClientContext));
        if (!ctx) {
            LOG_ERROR("Memory allocation failed for client context");
            close(client_fd);
            continue;
        }
        ctx->client_fd = client_fd;
        ctx->client_addr = client_addr;

        /* Spawn thread for this client */
        pthread_t tid;
        if (pthread_create(&tid, NULL, handle_client, ctx) != 0) {
            LOG_ERROR("Failed to create thread for client: %s", strerror(errno));
            free(ctx);
            close(client_fd);
            continue;
        }

        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, ip, sizeof(ip));
        LOG_INFO("Client handler thread spawned for %s (fd: %d)", ip, client_fd);
    }

    server_signal_handler(SIGTERM);
    return EXIT_SUCCESS;
}
