/*
 * =============================================================================
 * file_manager.c — File I/O with fcntl Advisory Locking & Versioning
 * =============================================================================
 * Concept: File Locking (Mandatory Concept 4.2)
 *          Data Consistency (Mandatory Concept 4.4)
 *
 * Features:
 * - Core file read/write with fcntl advisory locks
 * - Version History: store/restore/list/delete previous file versions
 * =============================================================================
 */

#include "../include/file_manager.h"
#include <dirent.h>

#define VERSION_DIR  "./storage/.versions"

static void get_filepath(const char *filename, char *filepath, int size) {
    snprintf(filepath, size, "%s/%s", STORAGE_DIR, filename);
}

int init_storage(void) {
    struct stat st;
    const char *dirs[] = { STORAGE_DIR, VERSION_DIR };
    for (int i = 0; i < 2; i++) {
        if (stat(dirs[i], &st) == -1) {
            if (mkdir(dirs[i], 0755) < 0) {
                LOG_ERROR("Failed to create directory %s: %s", dirs[i], strerror(errno));
                return -1;
            }
            LOG_INFO("Directory created: %s", dirs[i]);
        }
    }
    return 0;
}

/* Acquire shared read lock (F_RDLCK) — multiple readers allowed */
int lock_file_read(int fd) {
    struct flock fl;
    memset(&fl, 0, sizeof(fl));
    fl.l_type = F_RDLCK;
    fl.l_whence = SEEK_SET;
    fl.l_start = 0;
    fl.l_len = 0;
    if (fcntl(fd, F_SETLKW, &fl) < 0) {
        LOG_ERROR("Read lock failed on fd %d: %s", fd, strerror(errno));
        return -1;
    }
    LOG_DEBUG("Read lock acquired on fd %d", fd);
    return 0;
}

/* Acquire exclusive write lock (F_WRLCK) — blocks all others */
int lock_file_write(int fd) {
    struct flock fl;
    memset(&fl, 0, sizeof(fl));
    fl.l_type = F_WRLCK;
    fl.l_whence = SEEK_SET;
    fl.l_start = 0;
    fl.l_len = 0;
    if (fcntl(fd, F_SETLKW, &fl) < 0) {
        LOG_ERROR("Write lock failed on fd %d: %s", fd, strerror(errno));
        return -1;
    }
    LOG_DEBUG("Write lock acquired on fd %d", fd);
    return 0;
}

/* Release lock (F_UNLCK) */
int unlock_file(int fd) {
    struct flock fl;
    memset(&fl, 0, sizeof(fl));
    fl.l_type = F_UNLCK;
    fl.l_whence = SEEK_SET;
    fl.l_start = 0;
    fl.l_len = 0;
    if (fcntl(fd, F_SETLK, &fl) < 0) {
        LOG_ERROR("Unlock failed on fd %d: %s", fd, strerror(errno));
        return -1;
    }
    LOG_DEBUG("Lock released on fd %d", fd);
    return 0;
}

/* Save file: open → write-lock → write → unlock → close */
int save_file(const char *filename, const char *data, int size) {
    char filepath[256];
    get_filepath(filename, filepath, sizeof(filepath));

    int fd = open(filepath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        LOG_ERROR("Failed to open for writing: %s (%s)", filepath, strerror(errno));
        return -1;
    }

    if (lock_file_write(fd) < 0) { close(fd); return -1; }

    ssize_t written = write(fd, data, size);
    if (written != size) {
        LOG_ERROR("Write incomplete: %zd/%d bytes", written, size);
        unlock_file(fd);
        close(fd);
        return -1;
    }

    LOG_INFO("File saved: %s (%d bytes)", filename, size);
    unlock_file(fd);
    close(fd);
    return 0;
}

/* Read file: open → read-lock → read → unlock → close */
int read_file(const char *filename, char *buffer, int *size) {
    char filepath[256];
    get_filepath(filename, filepath, sizeof(filepath));

    int fd = open(filepath, O_RDONLY);
    if (fd < 0) {
        LOG_ERROR("Failed to open for reading: %s (%s)", filepath, strerror(errno));
        return -1;
    }

    if (lock_file_read(fd) < 0) { close(fd); return -1; }

    *size = read(fd, buffer, MAX_FILE_SIZE);
    if (*size < 0) {
        LOG_ERROR("Read failed: %s", strerror(errno));
        unlock_file(fd);
        close(fd);
        return -1;
    }

    LOG_INFO("File read: %s (%d bytes)", filename, *size);
    unlock_file(fd);
    close(fd);
    return 0;
}

/* Helper to remove all versions when file is deleted */
static void remove_all_versions(const char *filename) {
    char dir[256];
    snprintf(dir, sizeof(dir), "%s/%s", VERSION_DIR, filename);

    DIR *d = opendir(dir);
    if (!d) return;

    struct dirent *entry;
    while ((entry = readdir(d)) != NULL) {
        if (strcmp(entry->d_name, ".") != 0 && strcmp(entry->d_name, "..") != 0) {
            char path[512];
            snprintf(path, sizeof(path), "%s/%s", dir, entry->d_name);
            remove(path);
        }
    }
    closedir(d);
    rmdir(dir);
    LOG_INFO("All versions removed for: %s", filename);
}

/* Remove file from storage */
int remove_file_from_storage(const char *filename) {
    char filepath[256];
    get_filepath(filename, filepath, sizeof(filepath));
    if (remove(filepath) < 0) {
        LOG_ERROR("Failed to remove: %s (%s)", filepath, strerror(errno));
        return -1;
    }
    LOG_INFO("File removed: %s", filename);
    
    /* Clean up all versions associated with the file */
    remove_all_versions(filename);
    
    return 0;
}

/* Check if file exists */
int file_exists(const char *filename) {
    char filepath[256];
    get_filepath(filename, filepath, sizeof(filepath));
    struct stat st;
    return (stat(filepath, &st) == 0);
}

/* =============================================================================
 * VERSION HISTORY — Store / Restore / List / Delete previous file versions
 *
 * Directory layout:
 *   storage/.versions/<filename>/v1, v2, v3, ...
 * Each version file is a copy of the file at that point in time.
 * =============================================================================
 */

/* Copy src file to dst file (helper for versioning) */
static int copy_file(const char *src, const char *dst) {
    int src_fd = open(src, O_RDONLY);
    if (src_fd < 0) return -1;

    int dst_fd = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (dst_fd < 0) { close(src_fd); return -1; }

    char buf[4096];
    ssize_t n;
    while ((n = read(src_fd, buf, sizeof(buf))) > 0) {
        if (write(dst_fd, buf, n) != n) {
            close(src_fd); close(dst_fd);
            return -1;
        }
    }
    close(src_fd);
    close(dst_fd);
    return 0;
}

/* Ensure the per-file version directory exists */
static int ensure_version_dir(const char *filename) {
    char dir[256];
    snprintf(dir, sizeof(dir), "%s/%s", VERSION_DIR, filename);
    struct stat st;
    if (stat(dir, &st) == -1) {
        if (mkdir(dir, 0755) < 0) {
            LOG_ERROR("Failed to create version dir: %s (%s)", dir, strerror(errno));
            return -1;
        }
    }
    return 0;
}

/* Save current file as version N */
int save_version(const char *filename, int version) {
    if (ensure_version_dir(filename) < 0) return -1;

    char src[256], dst[256];
    snprintf(src, sizeof(src), "%s/%s", STORAGE_DIR, filename);
    snprintf(dst, sizeof(dst), "%s/%s/v%d", VERSION_DIR, filename, version);

    struct stat st;
    if (stat(src, &st) != 0) {
        LOG_DEBUG("No file to version: %s", filename);
        return 0;
    }

    if (copy_file(src, dst) < 0) {
        LOG_ERROR("Failed to save version %d of %s", version, filename);
        return -1;
    }
    LOG_INFO("Version saved: %s v%d", filename, version);

    /* Enforce MAX_VERSIONS limit */
    cleanup_old_versions(filename, MAX_VERSIONS);
    return 0;
}

/* Restore file from version N */
int restore_version(const char *filename, int version) {
    char src[256], dst[256];
    snprintf(src, sizeof(src), "%s/%s/v%d", VERSION_DIR, filename, version);
    snprintf(dst, sizeof(dst), "%s/%s", STORAGE_DIR, filename);

    struct stat st;
    if (stat(src, &st) != 0) {
        LOG_ERROR("Version %d not found for %s", version, filename);
        return -1;
    }

    if (copy_file(src, dst) < 0) {
        LOG_ERROR("Failed to restore version %d of %s", version, filename);
        return -1;
    }
    LOG_INFO("Restored: %s → v%d (%ld bytes)", filename, version, (long)st.st_size);
    return (int)st.st_size;  /* Return file size for metadata update */
}

/* List all stored versions of a file */
int list_versions(const char *filename, char *output, int max_size) {
    char dir[256];
    snprintf(dir, sizeof(dir), "%s/%s", VERSION_DIR, filename);

    DIR *d = opendir(dir);
    if (!d) {
        snprintf(output, max_size, "File '%s' not found or has no history.\n", filename);
        return -1;
    }

    int offset = snprintf(output, max_size,
        "\n╔═══════════════════════════════════════╗\n"
        "║       VERSION HISTORY: %-14s ║\n"
        "╠═══════╦═════════════╦═════════════════╣\n"
        "║ Ver   ║ Size        ║ Date            ║\n"
        "╠═══════╬═════════════╬═════════════════╣\n", filename);

    struct dirent *entry;
    int count = 0;
    
    typedef struct { int ver; char name[256]; } VerEntry;
    VerEntry entries[256];

    while ((entry = readdir(d)) != NULL && count < 256) {
        if (entry->d_name[0] == 'v') {
            entries[count].ver = atoi(entry->d_name + 1);
            strncpy(entries[count].name, entry->d_name, 255);
            count++;
        }
    }
    closedir(d);

    /* Sort descending (newest first) */
    for (int i = 0; i < count - 1; i++) {
        for (int j = i + 1; j < count; j++) {
            if (entries[i].ver < entries[j].ver) {
                VerEntry tmp = entries[i];
                entries[i] = entries[j];
                entries[j] = tmp;
            }
        }
    }

    int print_count = 0;
    for (int i = 0; i < count && offset < max_size - 100; i++) {
        char fpath[512];
        snprintf(fpath, sizeof(fpath), "%s/%s", dir, entries[i].name);
        struct stat st;
        if (stat(fpath, &st) == 0) {
            char time_str[32];
            strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M",
                     localtime(&st.st_mtime));
            offset += snprintf(output + offset, max_size - offset,
                "║ v%-4d ║ %7ldB    ║ %-15s ║\n",
                entries[i].ver, (long)st.st_size, time_str);
            print_count++;
        }
    }

    if (print_count == 0) {
        offset += snprintf(output + offset, max_size - offset,
            "║       (no versions stored)            ║\n");
    }

    snprintf(output + offset, max_size - offset,
        "╚═══════╩═════════════╩═════════════════╝\n"
        " Total versions: %d (max kept: %d)\n", print_count, MAX_VERSIONS);

    return print_count;
}

/* Remove oldest versions if count exceeds max_keep */
int cleanup_old_versions(const char *filename, int max_keep) {
    char dir[256];
    snprintf(dir, sizeof(dir), "%s/%s", VERSION_DIR, filename);

    /* Collect all version numbers */
    DIR *d = opendir(dir);
    if (!d) return 0;

    int versions[256];
    int count = 0;
    struct dirent *entry;
    while ((entry = readdir(d)) != NULL && count < 256) {
        if (entry->d_name[0] == 'v') {
            versions[count++] = atoi(entry->d_name + 1);
        }
    }
    closedir(d);

    if (count <= max_keep) return 0;

    /* Sort ascending (simple bubble sort — tiny array) */
    for (int i = 0; i < count - 1; i++) {
        for (int j = i + 1; j < count; j++) {
            if (versions[i] > versions[j]) {
                int tmp = versions[i];
                versions[i] = versions[j];
                versions[j] = tmp;
            }
        }
    }

    /* Remove oldest (keep the last max_keep) */
    int to_remove = count - max_keep;
    for (int i = 0; i < to_remove; i++) {
        char path[512];
        snprintf(path, sizeof(path), "%s/%s/v%d", VERSION_DIR, filename, versions[i]);
        remove(path);
        LOG_DEBUG("Removed old version: %s v%d", filename, versions[i]);
    }

    return to_remove;
}

/* Delete a specific version of a file */
int delete_version(const char *filename, int version) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s/v%d", VERSION_DIR, filename, version);

    struct stat st;
    if (stat(path, &st) != 0) {
        LOG_ERROR("Version %d not found for %s", version, filename);
        return -1;
    }

    if (remove(path) < 0) {
        LOG_ERROR("Failed to delete version %d of %s: %s",
                  version, filename, strerror(errno));
        return -1;
    }

    LOG_INFO("Deleted version %d of %s", version, filename);
    return 0;
}
