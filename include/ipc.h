/*
 * =============================================================================
 * ipc.h — IPC: Message Queues, Shared Memory & Named Pipe (Audit)
 * =============================================================================
 */

#ifndef IPC_H
#define IPC_H

#include "common.h"

/* ========================= Message Queue ========================= */
int  create_task_queue(void);
int  create_response_queue(void);
int  send_task(int mqid, TaskMessage *task);
int  receive_task(int mqid, TaskMessage *task);
int  send_response(int mqid, ResponseMessage *resp);
int  receive_response(int mqid, int request_id, ResponseMessage *resp);
void cleanup_message_queue(int mqid);

/* ========================= Shared Memory (File Metadata) ========= */
int  create_shared_memory(int *shmid);
FileMetadata* attach_shared_memory(int shmid);
void detach_shared_memory(FileMetadata *ptr);
void cleanup_shared_memory(int shmid);


/* ========================= Named Pipe — Audit Log (IPC) ========== */
int  create_audit_pipe(void);
void cleanup_audit_pipe(void);
void write_audit_entry(int audit_fd, const char *username, const char *operation,
                       const char *filename, const char *result);
int  read_audit_log(char *output, int max_size);

#endif /* IPC_H */
