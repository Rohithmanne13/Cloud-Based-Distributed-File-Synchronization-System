/*
 * =============================================================================
 * file_manager.h — File I/O, Locking & Version Management
 * =============================================================================
 */

#ifndef FILE_MANAGER_H
#define FILE_MANAGER_H

#include "common.h"

/* Storage initialization */
int init_storage(void);

/* File locking (fcntl advisory locks) */
int lock_file_read(int fd);
int lock_file_write(int fd);
int unlock_file(int fd);

/* Core file operations */
int save_file(const char *filename, const char *data, int size);
int read_file(const char *filename, char *buffer, int *size);
int remove_file_from_storage(const char *filename);
int file_exists(const char *filename);

/* --- Feature: Version History --- */
int save_version(const char *filename, int version);
int restore_version(const char *filename, int version);
int list_versions(const char *filename, char *output, int max_size);
int cleanup_old_versions(const char *filename, int max_keep);
int delete_version(const char *filename, int version);

#endif /* FILE_MANAGER_H */
