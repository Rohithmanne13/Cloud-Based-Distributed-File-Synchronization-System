/*
 * =============================================================================
 * metadata.h — Shared Memory Metadata & Edit Locks
 * =============================================================================
 */

#ifndef METADATA_H
#define METADATA_H

#include "common.h"

/* Initialize metadata array (zero out all entries) */
void init_metadata(FileMetadata *meta_array);

/* Add a new file entry (semaphore-protected) */
int add_file_metadata(FileMetadata *meta_array, const char *filename,
                      const char *owner_name, int owner_id, int size, int semid);

/* Update an existing file's version and size (semaphore-protected) */
int update_file_metadata(FileMetadata *meta_array, const char *filename,
                         int size, int semid);

/* Mark a file as deleted (semaphore-protected) */
int remove_file_metadata(FileMetadata *meta_array, const char *filename,
                         int semid);

/* Find a file's metadata (semaphore-protected); returns pointer or NULL */
FileMetadata* find_file_metadata(FileMetadata *meta_array,
                                 const char *filename, int semid);

/* List all active files into output buffer (semaphore-protected) */
int list_all_files(FileMetadata *meta_array, char *output,
                   int max_size, int semid);

/* Get version info for a specific file (semaphore-protected) */
int get_file_version(FileMetadata *meta_array, const char *filename,
                     char *output, int max_size, int semid);

/* --- Feature: Edit Lock (persistent application-level lock) --- */
int acquire_edit_lock(FileMetadata *meta_array, const char *filename,
                      int user_id, const char *username, int semid);
int release_edit_lock(FileMetadata *meta_array, const char *filename,
                      int user_id, int semid);
int check_edit_lock(FileMetadata *meta_array, const char *filename,
                    char *locker_name, int semid);


#endif /* METADATA_H */
