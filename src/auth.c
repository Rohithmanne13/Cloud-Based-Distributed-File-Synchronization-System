/*
 * =============================================================================
 * auth.c — Authentication & Role-Based Authorization Implementation
 * =============================================================================
 * Concept: Role-Based Authorization (Mandatory Concept 4.1)
 *
 * - Reads user credentials from users.dat
 * - Validates username/password
 * - Enforces permission matrix based on roles (ADMIN, USER, GUEST)
 * =============================================================================
 */

#include "../include/auth.h"
#include <sys/file.h>  /* for flock() */

/* Module-level user cache */
static UserInfo users[MAX_USERS];
static int user_count = 0;
static int users_loaded = 0;

/*
 * load_users — Read all users from users.dat
 * File format per line: id:username:password:role
 */
int load_users(UserInfo *out_users, int *count) {
    FILE *fp = fopen(USERS_FILE, "r");
    if (!fp) {
        LOG_ERROR("Failed to open users file: %s (%s)", USERS_FILE, strerror(errno));
        return -1;
    }

    *count = 0;
    char line[256];

    while (fgets(line, sizeof(line), fp) && *count < MAX_USERS) {
        /* Remove trailing newline/carriage return */
        line[strcspn(line, "\r\n")] = '\0';
        if (strlen(line) == 0 || line[0] == '#') continue;  /* skip empty/comments */

        char *saveptr;
        char *token;

        /* Parse ID */
        token = strtok_r(line, ":", &saveptr);
        if (!token) continue;
        out_users[*count].id = atoi(token);

        /* Parse Username */
        token = strtok_r(NULL, ":", &saveptr);
        if (!token) continue;
        strncpy(out_users[*count].username, token, MAX_USERNAME - 1);
        out_users[*count].username[MAX_USERNAME - 1] = '\0';

        /* Parse Password */
        token = strtok_r(NULL, ":", &saveptr);
        if (!token) continue;
        strncpy(out_users[*count].password, token, MAX_PASSWORD - 1);
        out_users[*count].password[MAX_PASSWORD - 1] = '\0';

        /* Parse Role */
        token = strtok_r(NULL, ":", &saveptr);
        if (!token) continue;
        out_users[*count].role = atoi(token);

        (*count)++;
    }

    fclose(fp);
    LOG_INFO("Loaded %d users from %s", *count, USERS_FILE);
    return 0;
}

/*
 * authenticate_user — Validate username/password, return user info
 * Returns: 0 on success, -1 on failure
 */
int authenticate_user(const char *username, const char *password, UserInfo *user) {
    /* Always re-read users.dat for fresh data */
    if (load_users(users, &user_count) < 0) {
        return -1;
    }
    users_loaded = 1;

    for (int i = 0; i < user_count; i++) {
        if (strcmp(users[i].username, username) == 0 &&
            strcmp(users[i].password, password) == 0) {
            if (user) {
                *user = users[i];
            }
            LOG_INFO("User '%s' authenticated successfully (Role: %s)",
                     username, role_to_string(users[i].role));
            return 0;
        }
    }

    LOG_WARN("Authentication FAILED for user '%s'", username);
    return -1;
}

/*
 * check_permission — Enforce role-based access control
 *
 * Permission Matrix (RBAC layer — ownership checked per-file in worker):
 * ┌───────────┬───────┬──────────────────┬───────┐
 * │ Operation │ Admin │ User             │ Guest │
 * ├───────────┼───────┼──────────────────┼───────┤
 * │ UPLOAD    │  ✓    │  ✓ (own files)   │  ✗    │
 * │ DOWNLOAD  │  ✓    │  ✓               │  ✓    │
 * │ DELETE    │  ✓    │  ✓ (own files)   │  ✗    │
 * │ LIST      │  ✓    │  ✓               │  ✓    │
 * │ VERSION   │  ✓    │  ✓               │  ✓    │
 * │ EDIT      │  ✓    │  ✓ (own files)   │  ✗    │
 * │ SAVE      │  ✓    │  ✓               │  ✗    │
 * │ UNLOCK    │  ✓    │  ✓               │  ✗    │
 * │ HISTORY   │  ✓    │  ✓               │  ✓    │
 * │ ROLLBACK  │  ✓    │  ✓ (own files)   │  ✗    │
 * │ DELVERSION│  ✓    │  ✓ (own files)   │  ✗    │
 * │ AUDIT     │  ✓    │  ✗               │  ✗    │
 * └───────────┴───────┴──────────────────┴───────┘
 *
 * Returns: 1 if allowed, 0 if denied
 */
int check_permission(int role, int operation) {
    switch (role) {
        case ROLE_ADMIN:
            return 1;   /* Admin can do everything */

        case ROLE_USER:
            /* Users can do everything except view audit log */
            /* Ownership enforcement (own files only) is handled in worker.c */
            return (operation != OP_AUDIT);

        case ROLE_GUEST:
            /* Guests can only download, list, view versions, and history */
            return (operation == OP_DOWNLOAD ||
                    operation == OP_LIST ||
                    operation == OP_VERSION ||
                    operation == OP_HISTORY);

        default:
            LOG_WARN("Unknown role %d — denying access", role);
            return 0;
    }
}

/* Convert role integer to string */
const char* role_to_string(int role) {
    switch (role) {
        case ROLE_ADMIN: return "ADMIN";
        case ROLE_USER:  return "USER";
        case ROLE_GUEST: return "GUEST";
        default:         return "UNKNOWN";
    }
}

/* Convert operation integer to string */
const char* operation_to_string(int operation) {
    switch (operation) {
        case OP_LOGIN:    return "LOGIN";
        case OP_UPLOAD:   return "UPLOAD";
        case OP_DOWNLOAD: return "DOWNLOAD";
        case OP_DELETE:   return "DELETE";
        case OP_LIST:     return "LIST";
        case OP_VERSION:  return "VERSION";
        case OP_EXIT:     return "EXIT";
        case OP_EDIT:     return "EDIT";
        case OP_SAVE:     return "SAVE";
        case OP_UNLOCK:   return "UNLOCK";
        case OP_HISTORY:  return "HISTORY";
        case OP_ROLLBACK: return "ROLLBACK";
        case OP_AUDIT:    return "AUDIT";
        case OP_DELVERSION: return "DELVERSION";
        case OP_SIGNUP:   return "SIGNUP";
        default:          return "UNKNOWN";
    }
}

/*
 * register_user — Register a new user account
 *
 * Checks for duplicate usernames, auto-assigns next user ID,
 * appends entry to users.dat, and refreshes in-memory cache.
 * Uses flock() for thread-safe file access.
 *
 * Returns:  0 on success
 *          -1 on error (file I/O, etc.)
 *          -2 if username already exists
 */
int register_user(const char *username, const char *password, int role, UserInfo *user) {
    /* Prevent using 'admin' as a username */
    if (username != NULL) {
        char lower_name[MAX_USERNAME];
        int i = 0;
        for (; username[i] && i < MAX_USERNAME - 1; i++) {
            char c = username[i];
            if (c >= 'A' && c <= 'Z') c += 32;
            lower_name[i] = c;
        }
        lower_name[i] = '\0';
        if (strcmp(lower_name, "admin") == 0) {
            LOG_WARN("Registration failed: 'admin' is a reserved username");
            return -2;
        }
    }

    /* Always re-read users.dat for fresh data (avoids stale cache issues) */
    if (load_users(users, &user_count) < 0) {
        return -1;
    }
    users_loaded = 1;

    /* Check for duplicate username + role combination */
    for (int i = 0; i < user_count; i++) {
        if (strcmp(users[i].username, username) == 0 &&
            users[i].role == role) {
            LOG_WARN("Registration failed: username '%s' with role '%s' already exists",
                     username, role_to_string(role));
            return -2;
        }
    }

    /* Find next available user ID */
    int next_id = 1;
    for (int i = 0; i < user_count; i++) {
        if (users[i].id >= next_id) {
            next_id = users[i].id + 1;
        }
    }

    /* Open users.dat for appending with file lock */
    FILE *fp = fopen(USERS_FILE, "a");
    if (!fp) {
        LOG_ERROR("Failed to open users file for registration: %s", strerror(errno));
        return -1;
    }

    /* Acquire exclusive lock for thread safety */
    if (flock(fileno(fp), LOCK_EX) < 0) {
        LOG_ERROR("Failed to lock users file: %s", strerror(errno));
        fclose(fp);
        return -1;
    }

    /* Write the new user entry */
    fprintf(fp, "%d:%s:%s:%d\n", next_id, username, password, role);
    fflush(fp);

    /* Release lock and close */
    flock(fileno(fp), LOCK_UN);
    fclose(fp);

    /* Add to in-memory cache */
    if (user_count < MAX_USERS) {
        users[user_count].id = next_id;
        strncpy(users[user_count].username, username, MAX_USERNAME - 1);
        users[user_count].username[MAX_USERNAME - 1] = '\0';
        strncpy(users[user_count].password, password, MAX_PASSWORD - 1);
        users[user_count].password[MAX_PASSWORD - 1] = '\0';
        users[user_count].role = role;
        user_count++;
    }

    /* Fill the output UserInfo */
    if (user) {
        user->id = next_id;
        strncpy(user->username, username, MAX_USERNAME - 1);
        user->username[MAX_USERNAME - 1] = '\0';
        strncpy(user->password, password, MAX_PASSWORD - 1);
        user->password[MAX_PASSWORD - 1] = '\0';
        user->role = role;
    }

    LOG_INFO("New user registered: '%s' (ID: %d, Role: %s)",
             username, next_id, role_to_string(role));
    return 0;
}
