/*
 * =============================================================================
 * auth.h — Authentication & Role-Based Authorization
 * =============================================================================
 */

#ifndef AUTH_H
#define AUTH_H

#include "common.h"

#define MAX_USERS   20
#define USERS_FILE  "users.dat"

/* Load all users from users.dat into the users array */
int load_users(UserInfo *users, int *count);

/* Authenticate a user by username/password; fills UserInfo on success */
int authenticate_user(const char *username, const char *password, UserInfo *user);

/* Check if a role has permission for an operation (1=allowed, 0=denied) */
int check_permission(int role, int operation);

/* Convert role integer to human-readable string */
const char* role_to_string(int role);

/* Convert operation integer to human-readable string */
const char* operation_to_string(int operation);

/* Register a new user — appends to users.dat, returns 0 on success, -1 on error, -2 if user exists */
int register_user(const char *username, const char *password, int role, UserInfo *user);

#endif /* AUTH_H */
