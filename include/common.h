// =============================================================================
// common.h — Shared Constants, Structures, and Macros
// Cloud-Based Distributed File Synchronization System
// =============================================================================
// This header defines all shared data structures, constants, and macros used
// across the client, server, and worker components.
// =============================================================================

#ifndef COMMON_H // Prevent multiple inclusions of this header file
#define COMMON_H // Define the macro to mark this file as included

#include <stdio.h>      // Standard input/output definitions (printf, scanf, etc.)
#include <stdlib.h>     // Standard library functions (malloc, free, exit, etc.)
#include <string.h>     // String manipulation functions (strcpy, strcmp, etc.)
#include <unistd.h>     // POSIX operating system API (fork, read, write, close, etc.)
#include <sys/types.h>  // Definitions for data types used in system calls (pid_t, etc.)
#include <sys/socket.h> // Definitions for socket programming (socket, connect, etc.)
#include <netinet/in.h> // Constants and structures for Internet domain addresses (sockaddr_in)
#include <arpa/inet.h>  // Definitions for internet operations (inet_addr, etc.)
#include <sys/ipc.h>    // Inter-process communication access structure (ftok, etc.)
#include <sys/msg.h>    // Message queue structures and functions (msgget, msgsnd, msgrcv)
#include <sys/shm.h>    // Shared memory facility (shmget, shmat, shmdt)
#include <sys/sem.h>    // Semaphore facility (semget, semop, semctl)
#include <sys/stat.h>   // Data returned by the stat() function (file info)
#include <fcntl.h>      // File control options (open, O_RDONLY, fcntl locks, etc.)
#include <signal.h>     // Signal handling (signal, kill, SIGINT, etc.)
#include <pthread.h>    // POSIX threads library (pthread_create, etc.)
#include <errno.h>      // System error numbers (errno)
#include <time.h>       // Time types and functions (time, ctime, etc.)
#include <sys/wait.h>   // Declarations for waiting (wait, waitpid, etc.)

// ========================= Configuration =========================
#define SERVER_PORT     8080    // The port number on which the server listens
#define MAX_CLIENTS     10      // Maximum number of concurrent client connections
#define NUM_WORKERS     4       // Worker pool size (optimal for demo)
#define MAX_FILES       100     // Maximum number of files tracked in shared memory
#define MAX_FILENAME    64      // Maximum length allowed for a filename
#define MAX_USERNAME    32      // Maximum length allowed for a username
#define MAX_PASSWORD    32      // Maximum length allowed for a password
#define MAX_FILE_SIZE   4096    // Maximum file content size per transfer
#define MAX_MSG_SIZE    256     // Maximum size for status messages in responses
#define STORAGE_DIR     "./storage" // Directory path where the server stores files

// ========================= IPC Keys ==============================
#define TASK_QUEUE_KEY  0x1234  // Key for the message queue used to send tasks to workers
#define RESP_QUEUE_KEY  0x1235  // Key for the message queue used by workers to send responses
#define SHM_KEY         0x1236  // Key for the shared memory segment storing file metadata
#define SEM_KEY         0x1237  // Key for the semaphore used to protect shared memory access

// ========================= Operations ============================
#define OP_LOGIN        0   // Operation code for logging in
#define OP_UPLOAD       1   // Operation code for uploading a file
#define OP_DOWNLOAD     2   // Operation code for downloading a file
#define OP_DELETE       3   // Operation code for deleting a file
#define OP_LIST         4   // Operation code for listing all files
#define OP_VERSION      5   // Operation code for checking file version/metadata
#define OP_EXIT         6   // Operation code for signing out / exiting
#define OP_EDIT         7   // Acquire edit lock + download file
#define OP_SAVE         8   // Upload edited file + release lock
#define OP_UNLOCK       9   // Release edit lock without saving
#define OP_HISTORY      10  // View version history of a file
#define OP_ROLLBACK     11  // Restore a file to a previous version
#define OP_QUOTA        12  // (removed — kept for enum gap)
#define OP_AUDIT        13  // View audit log (admin only)
#define OP_DELVERSION   14  // Delete a specific version from history
#define OP_SIGNUP       15  // Register a new user account

// ========================= Roles =================================
#define ROLE_ADMIN      0   // Administrator role with full access
#define ROLE_USER       1   // Regular user role, can modify their own files
#define ROLE_GUEST      2   // Guest role, restricted to read-only operations

// ========================= Status Codes ==========================
#define STATUS_SUCCESS           0  // Indicates the operation was successful
#define STATUS_ERROR             1  // Indicates a general error occurred
#define STATUS_AUTH_FAIL         2  // Indicates authentication failed (wrong password/user not found)
#define STATUS_PERMISSION_DENIED 3  // Indicates the user lacks permission for the operation
#define STATUS_FILE_NOT_FOUND    4  // Indicates the requested file does not exist
#define STATUS_FILE_EXISTS       5  // Indicates the file already exists (not used actively here)
#define STATUS_FILE_LOCKED       6  // Indicates the file is locked by another user for editing
#define STATUS_USER_EXISTS       7  // Indicates sign up failed because username already exists

// ========================= Advanced Feature Config ================
#define LOCK_TIMEOUT_SECS  300  // Edit lock timeout: 5 minutes before lock is considered stale
#define MAX_VERSIONS       5    // Keep last N versions per file before rotating old ones out

// Audit log via named pipe (IPC)
#define AUDIT_PIPE_PATH    "/tmp/dfs_audit_pipe" // Path to the named pipe used for sending logs
#define AUDIT_LOG_PATH     "./audit.log"         // Path to the persistent log file on disk
#define MAX_AUDIT_ENTRY    512                   // Maximum size of a single audit log entry string

// ========================= ANSI Colors ===========================
#define COLOR_RESET     "\033[0m"       // ANSI code to reset text formatting
#define COLOR_RED       "\033[1;31m"    // ANSI code for bold red text
#define COLOR_GREEN     "\033[1;32m"    // ANSI code for bold green text
#define COLOR_YELLOW    "\033[1;33m"    // ANSI code for bold yellow text
#define COLOR_BLUE      "\033[1;34m"    // ANSI code for bold blue text
#define COLOR_MAGENTA   "\033[1;35m"    // ANSI code for bold magenta text
#define COLOR_CYAN      "\033[1;36m"    // ANSI code for bold cyan text
#define COLOR_WHITE     "\033[1;37m"    // ANSI code for bold white text
#define COLOR_DIM       "\033[2m"       // ANSI code for dim text
#define COLOR_BOLD      "\033[1m"       // ANSI code for bold text

// ========================= Structures ============================

// Client request — sent over TCP socket
typedef struct {
    int operation;                      // The operation code (e.g., OP_UPLOAD)
    char username[MAX_USERNAME];        // The username requesting the operation
    char password[MAX_PASSWORD];        // The password for authentication
    char filename[MAX_FILENAME];        // Target filename for the operation
    int data_size;                      // Size of the data payload in bytes
    char data[MAX_FILE_SIZE];           // File content or data payload
} ClientRequest;                        // Type alias for the client request struct

// Server response — sent over TCP socket
typedef struct {
    int status;                         // Status code resulting from the operation
    char message[MAX_MSG_SIZE];         // Human-readable status or error message
    int data_size;                      // Size of the data payload returned
    char data[MAX_FILE_SIZE];           // File content or data payload returned
} ServerResponse;                       // Type alias for the server response struct

// File metadata — stored in shared memory
typedef struct {
    char filename[MAX_FILENAME];        // The name of the file
    char owner_name[MAX_USERNAME];      // Username of the user who owns the file
    int  owner_id;                      // User ID of the owner
    int  version;                       // Current version number of the file
    int  file_size;                     // Current size of the file in bytes
    int  is_active;                     // 1 = file exists, 0 = slot is empty/deleted
    time_t created;                     // Timestamp of when the file was first uploaded
    time_t last_modified;               // Timestamp of the last modification/save
    // --- Edit lock fields (Feature: Write Lock Redesign) --- 
    int  locked_by_user_id;             // User ID holding the edit lock (-1 = unlocked)
    char locked_by_username[MAX_USERNAME]; // Username holding the edit lock
    time_t lock_acquired_at;            // Timestamp when the lock was acquired (for stale checking)
} FileMetadata;                         // Type alias for file metadata struct

// Task message — sent via message queue (server → worker)
typedef struct {
    long mtype;                         // Must be > 0; set to 1 for task distribution
    int  request_id;                    // Unique ID to match the response to the client
    int  operation;                     // The requested operation code
    int  user_id;                       // ID of the authenticated user
    int  role;                          // Role of the authenticated user
    char username[MAX_USERNAME];        // Username making the request
    char filename[MAX_FILENAME];        // Target filename
    char data[MAX_FILE_SIZE];           // Data payload for the task
    int  data_size;                     // Size of the data payload
    int  target_version;                // For OP_ROLLBACK: which version to restore
} TaskMessage;                          // Type alias for task message struct

// Response message — sent via message queue (worker → server)
typedef struct {
    long mtype;                         // Set to request_id so server thread reads correct response
    int  status;                        // Resulting status code from worker execution
    char message[MAX_MSG_SIZE];         // Status/error message from worker
    char data[MAX_FILE_SIZE];           // Data payload returning from worker
    int  data_size;                     // Size of the returned data
} ResponseMessage;                      // Type alias for response message struct

// User information
typedef struct {
    int  id;                            // Unique internal user ID
    char username[MAX_USERNAME];        // User's login name
    char password[MAX_PASSWORD];        // User's password (plaintext for demo)
    int  role;                          // User's role (ROLE_ADMIN, ROLE_USER, ROLE_GUEST)
} UserInfo;                             // Type alias for user information struct

// ========================= Logging Macros ========================
// Macro to log general info messages with green coloring
#define LOG_INFO(fmt, ...) \
    printf(COLOR_GREEN "[INFO ][PID:%-5d][%s] " fmt COLOR_RESET "\n", \
           getpid(), __func__, ##__VA_ARGS__)

// Macro to log error messages to standard error with red coloring
#define LOG_ERROR(fmt, ...) \
    fprintf(stderr, COLOR_RED "[ERROR][PID:%-5d][%s] " fmt COLOR_RESET "\n", \
            getpid(), __func__, ##__VA_ARGS__)

// Macro to log warning messages with yellow coloring
#define LOG_WARN(fmt, ...) \
    printf(COLOR_YELLOW "[WARN ][PID:%-5d][%s] " fmt COLOR_RESET "\n", \
           getpid(), __func__, ##__VA_ARGS__)

// Macro to log debug messages with cyan coloring
#define LOG_DEBUG(fmt, ...) \
    printf(COLOR_CYAN "[DEBUG][PID:%-5d][%s] " fmt COLOR_RESET "\n", \
           getpid(), __func__, ##__VA_ARGS__)

#endif // End of COMMON_H inclusion guard
