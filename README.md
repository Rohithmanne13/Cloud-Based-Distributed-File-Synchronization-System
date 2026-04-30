<div align="center">
  <h1>☁️ Cloud-Based Distributed File Synchronization System</h1>
  <p><i>A robust, modular, client-server file synchronization platform demonstrating core Operating System concepts.</i></p>
</div>

## 📖 Overview
A cloud-based distributed file synchronization system in C using POSIX syscalls and System V IPC mechanisms such as message queues, shared memory, named pipes (FIFO). Supports concurrent upload, download, edit, versioning, and rollback with semaphores, fcntl locks, role-based access control, server-side storage, shared metadata, and .versions backup.

## ✨ Comprehensive Feature List
1. **Concurrent Worker Pool Model:** Pre-forked worker processes dynamically dequeue and process tasks without the overhead of per-request process creation.
2. **Application-Level Collaborative Edit Locks:** Prevents the "Lost Update" problem. Users acquire exclusive edit locks via the `EDIT` command, blocking others until changes are saved (`SAVE`) or discarded (`UNLOCK`). Includes a stale-lock timeout mechanism (300 seconds).
3. **Robust Version History & Rollback:** Automatically backs up files on every overwrite, maintaining the 5 most recent versions using FIFO rotation. Supports `HISTORY` listing, `ROLLBACK` to any previous version, and `DELVERSION`.
4. **Strict Role-Based Access Control (RBAC):** Three distinct roles (`ADMIN`, `USER`, `GUEST`). Enforces strict file ownership rules—only owners or admins can modify, delete, or rollback files.
5. **Advanced Audit Logging:** Employs a dedicated Audit Logger thread and **Named Pipes (FIFO)** to persistently record every system operation in `audit.log`. Admins can view this via the `AUDIT` command.
6. **Interactive Colored CLI Client:** Provides a user-friendly, real-time command-line interface with clear status messages and automatic reconnection on authentication failure.
7. **Secure Authentication:** User registration (`SIGNUP`) and login (`SIGNIN`) with duplicate user checking and secure role assignment. The `admin` username is reserved.
8. **Thread-Safe Request Handling:** The main server spawns a detached `pthread` for each connected client and uses mutexes for safe request ID generation.

## 🏗️ High-Level Architecture

```text
================================================================================
                           HIGH-LEVEL ARCHITECTURE
================================================================================

[ CLIENTS ]         [ SERVER ]                 [ WORKER POOL ]
(alice, bob)        (Main Process)             (4 Pre-forked Processes)

     |                   |                            |
     |--(1) Request----->|                            |
     |  (TCP Sockets)    | - Authenticates (users.dat)|
     |                   | - Validates RBAC           |
     |                   | - Enqueues task            |
     |                   |                            |
     |                   |-------(2) Passes Task----->|
     |                   |    (System V Msg Queue)    | - Dequeues task
     |                   |                            | - Locks file (fcntl)
     |                   |                            | - Performs I/O
     |                   |                            |
     |                   |                            |--(3) Updates Metadata----> [ SHARED MEM ]
     |                   |                            |                            (Protected by Semaphore)
     |                   |                            |
     |                   |                            |--(3) Sends Audit Log-----> [ NAMED PIPE ]
     |                   |                            |
     |                   |                            |--(3) Reads/Writes File---> [ STORAGE ]
     |                   |                            |                            (Files & .versions)
     |                   |<-------(4) Sends Response--|
     |                   |    (System V Msg Queue)    |
     |<-(5) Response-----|                            |
     |  (TCP Sockets)    |                            |

================================================================================
                           KEY CONCEPTS DEMONSTRATED
================================================================================
1. Sockets: Client-Server communication.
2. IPC (Message Queues): Server delegating tasks to workers.
3. IPC (Shared Memory): Global file metadata state.
4. IPC (Named Pipes): Workers sending logs to audit thread.
5. Synchronization (Semaphores): Protecting shared memory.
6. File Locking (fcntl): Safe concurrent file reads/writes.
7. Concurrency: Pre-forked worker pool handling requests.
8. Authorization: Role-based access control (Admin/User/Guest).
```

## 📂 File Structure

| Layer | Components | Source Files |
|-------|-----------|-------------|
| **Client Layer** | Client 1–3, Interactive CLI | `src/client.c` |
| **Authentication** | Signup, Signin, RBAC Matrix | `src/auth.c`, `users.dat` |
| **Server Process** | Main process, pthread per client | `src/server.c` |
| **IPC Layer** | MsgQueue × 2, SharedMem, Semaphore, Named Pipe | `src/ipc.c`, `src/sync.c` |
| **Worker Pool** | Worker 0–3 (fork), 12 operations | `src/worker.c` |
| **Storage & Versioning** | `./storage/`, `.versions/`, fcntl locks | `src/file_manager.c`, `src/metadata.c` |

## ⚙️ All 12 Operations

| # | Command | Flow | OS Concepts |
|---|---------|------|-------------|
| 1 | **UPLOAD** | ownership → lock check → save_version → save_file → add_metadata | fcntl, semaphore, versioning |
| 2 | **DOWNLOAD** | find metadata → read_file (fcntl read lock) → return | fcntl F_RDLCK |
| 3 | **DELETE** | ownership → lock check → remove file → remove metadata → cleanup versions | fcntl, semaphore |
| 4 | **LIST** | read shared memory → formatted table | semaphore-protected read |
| 5 | **VERSION** | lookup metadata → format details | semaphore-protected read |
| 6 | **EDIT** | ownership → acquire_edit_lock → read_file → return | app-level lock + fcntl |
| 7 | **SAVE** | verify lock holder → save_version → save_file → update_metadata → release lock | fcntl, semaphore, versioning |
| 8 | **UNLOCK** | release_edit_lock (no save) | semaphore |
| 9 | **HISTORY** | scan .versions/ dir → sorted table | stat(), readdir() |
| 10 | **ROLLBACK** | ownership → save current → restore_version → update metadata | file copy, semaphore |
| 11 | **DELVERSION** | ownership → delete specific version file | remove() |
| 12 | **AUDIT** | read audit.log → formatted table (admin only) | Named Pipe IPC |

## 💻 System Calls Used

| Category | System Calls |
|----------|-------------|
| **Sockets** | `socket`, `bind`, `listen`, `accept`, `connect`, `send`, `recv` |
| **Message Queue** | `msgget`, `msgsnd`, `msgrcv`, `msgctl` |
| **Shared Memory** | `shmget`, `shmat`, `shmdt`, `shmctl` |
| **Semaphores** | `semget`, `semop`, `semctl` |
| **File Locking** | `fcntl(F_SETLKW)`, `fcntl(F_SETLK)` |
| **Process** | `fork`, `wait`, `kill`, `signal` |
| **File I/O** | `open`, `read`, `write`, `close`, `stat`, `mkdir`, `remove` |
| **Threading** | `pthread_create`, `pthread_detach`, `pthread_mutex_lock/unlock` |
| **Named Pipe** | `mkfifo`, `unlink` |
| **File Lock (auth)** | `flock` |

## 🚀 Getting Started

### Prerequisites
- Linux or WSL environment (relies on POSIX / System V APIs)
- GCC Compiler and `make`

### Build Instructions
```bash
# Compile both the server and the client
make

# Clean build artifacts and resets users.dat to default admin
make clean

# Remove leftover IPC resources (run this in case of a server crash)
make ipc-clean
```

### Running the System
Open at least two terminals.

**Terminal 1 (Server):**
```bash
./server
```

**Terminal 2 & 3 (Clients):**
```bash
./client 127.0.0.1 8080
```

### Test Accounts
| Username | Password   | Role   | Permissions                                  |
|----------|------------|--------|----------------------------------------------|
| `admin`  | `admin123` | ADMIN  | Unrestricted global access                   |
| `alice`  | `pass123`  | USER   | Can modify own files, edit, upload, download |
| `bob`    | `pass456`  | USER   | Can modify own files, edit, upload, download |
| `guest`  | `guest`    | GUEST  | Read-only access (List, Download, History)   |

## 🛡️ Challenges Solved
- **The "Lost Update" Problem:** Solved using persistent edit locks in shared memory.
- **Concurrent Disk I/O Corruption:** Prevented by combining shared memory semaphores with `fcntl()` file-level locks.
- **Infinite Storage Consumption:** Managed via a Rolling Window (FIFO) versioning mechanism.
- **Resource Leaks:** Handled via custom `SIGINT`/`SIGTERM` signal handlers to safely clean up IPC structures.
- **Deadlocks during Collaboration:** Addressed using a Stale-Lock timeout mechanism (300 seconds) to forcibly release abandoned edit locks.
