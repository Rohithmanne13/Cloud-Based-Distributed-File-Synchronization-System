/*
 * =============================================================================
 * client.c — Interactive Client Application
 * =============================================================================
 * Concept: Socket Programming (Mandatory Concept 4.5)
 *
 * Connects to the server via TCP socket, provides an interactive CLI:
 * - LOGIN with username/password
 * - UPLOAD/DOWNLOAD/DELETE/LIST/VERSION commands
 * - EDIT/SAVE/UNLOCK — collaborative edit lock workflow
 * - HISTORY/ROLLBACK — version history management
 * - AUDIT — admin audit log
 * - Colored terminal output for professional appearance
 * =============================================================================
 */

#include "../include/common.h"

/* ========================= Helper Functions ====================== */

static void print_banner(void) {
    printf(COLOR_CYAN);
    printf("╔══════════════════════════════════════════════════════════╗\n");
    printf("║     Cloud-Based Distributed File Sync System             ║\n");
    printf("╚══════════════════════════════════════════════════════════╝\n");
    printf(COLOR_RESET);
}

static void print_help(void) {
    printf(COLOR_YELLOW);
    printf("\n╔══════════════════════════════════════════════════════════╗\n");
    printf("║                  AVAILABLE COMMANDS                      ║\n");
    printf("╠══════════════════════════════════════════════════════════╣\n");
    printf("║                                                          ║\n");
    printf("║  [File Operations]                                       ║\n");
    printf("║  UPLOAD   <filename>   Upload a local file               ║\n");
    printf("║  DOWNLOAD <filename>   Download a file from server       ║\n");
    printf("║  DELETE   <filename>   Delete a file (owner/admin)       ║\n");
    printf("║  LIST                  List all stored files             ║\n");
    printf("║  VERSION  <filename>   View file version info            ║\n");
    printf("║                                                          ║\n");
    printf("║  [Edit Lock - Collaborative]                             ║\n");
    printf("║  EDIT     <filename>   Lock file + download for edit     ║\n");
    printf("║  SAVE     <filename>   Upload edits + release lock       ║\n");
    printf("║  UNLOCK   <filename>   Release lock (discard edits)      ║\n");
    printf("║                                                          ║\n");
    printf("║  [Version History]                                       ║\n");
    printf("║  HISTORY  <filename>   View version history              ║\n");
    printf("║  ROLLBACK <file> <v>   Restore to version v              ║\n");
    printf("║  DELVERSION <f> <v>    Delete version v from history     ║\n");
    printf("║                                                          ║\n");
    printf("║  [System]                                                ║\n");
    printf("║  AUDIT                 View audit log (admin only)       ║\n");
    printf("║  HELP                  Show this help menu               ║\n");
    printf("║  SIGNOUT               Sign out and disconnect           ║\n");
    printf("║                                                          ║\n");
    printf("╚══════════════════════════════════════════════════════════╝\n");
    printf(COLOR_RESET);
}

static void print_status(int status, const char *message) {
    switch (status) {
        case STATUS_SUCCESS:
            printf(COLOR_GREEN "  ✓ SUCCESS: %s" COLOR_RESET "\n", message);
            break;
        case STATUS_AUTH_FAIL:
            printf(COLOR_RED "  ✗ AUTH FAIL: %s" COLOR_RESET "\n", message);
            break;
        case STATUS_PERMISSION_DENIED:
            printf(COLOR_RED "  ✗ DENIED: %s" COLOR_RESET "\n", message);
            break;
        case STATUS_FILE_NOT_FOUND:
            printf(COLOR_YELLOW "  ✗ NOT FOUND: %s" COLOR_RESET "\n", message);
            break;
        case STATUS_FILE_LOCKED:
            printf(COLOR_YELLOW "  [LOCKED]: %s" COLOR_RESET "\n", message);
            break;
        case STATUS_USER_EXISTS:
            printf(COLOR_YELLOW "  ✗ USER EXISTS: %s" COLOR_RESET "\n", message);
            break;
        default:
            printf(COLOR_RED "  ✗ ERROR: %s" COLOR_RESET "\n", message);
            break;
    }
}

/* Read a local file into a buffer */
static int read_local_file(const char *filename, char *buffer, int max_size) {
    FILE *fp = fopen(filename, "rb");
    if (!fp) {
        printf(COLOR_RED "  ✗ Cannot open local file: %s (%s)"
               COLOR_RESET "\n", filename, strerror(errno));
        return -1;
    }

    int size = fread(buffer, 1, max_size, fp);
    fclose(fp);

    if (size <= 0) {
        printf(COLOR_RED "  ✗ File is empty or read error" COLOR_RESET "\n");
        return -1;
    }

    return size;
}

/* Helper to get role string */
static const char* get_role_string(int role) {
    if (role == ROLE_ADMIN) return "ADMIN";
    if (role == ROLE_USER) return "USER";
    if (role == ROLE_GUEST) return "GUEST";
    return "UNKNOWN";
}

/* Save downloaded data to a local file */
static int save_local_file(const char *filename, const char *data, int size, const char *username, int role) {
    char dir[] = "downloads";
    struct stat st;
    if (stat(dir, &st) == -1) {
        mkdir(dir, 0755);
    }

    char role_dir[128];
    snprintf(role_dir, sizeof(role_dir), "downloads/%s", get_role_string(role));
    if (stat(role_dir, &st) == -1) {
        mkdir(role_dir, 0755);
    }

    char user_dir[256];
    snprintf(user_dir, sizeof(user_dir), "downloads/%s/%s", get_role_string(role), username);
    if (stat(user_dir, &st) == -1) {
        mkdir(user_dir, 0755);
    }

    char filepath[512];
    snprintf(filepath, sizeof(filepath), "downloads/%s/%s/%s", get_role_string(role), username, filename);

    FILE *fp = fopen(filepath, "wb");
    if (!fp) {
        printf(COLOR_RED "  ✗ Cannot create local file: %s"
               COLOR_RESET "\n", filepath);
        return -1;
    }

    fwrite(data, 1, size, fp);
    fclose(fp);

    printf(COLOR_GREEN "  ✓ File saved to: %s" COLOR_RESET "\n", filepath);
    return 0;
}

/* ========================= Main ================================== */
int main(int argc, char *argv[]) {
    print_banner();

    const char *server_ip = "127.0.0.1";
    int server_port = SERVER_PORT;

    if (argc >= 2) server_ip = argv[1];
    if (argc >= 3) server_port = atoi(argv[2]);

    /* ---- Create Socket & Connect ---- */
    int sock_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (sock_fd < 0) {
        printf(COLOR_RED "  ✗ Socket creation failed: %s"
               COLOR_RESET "\n", strerror(errno));
        return EXIT_FAILURE;
    }

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(server_port);

    if (inet_pton(AF_INET, server_ip, &server_addr.sin_addr) <= 0) {
        printf(COLOR_RED "  ✗ Invalid server address: %s"
               COLOR_RESET "\n", server_ip);
        close(sock_fd);
        return EXIT_FAILURE;
    }

    printf(COLOR_DIM "  Connecting to %s:%d..." COLOR_RESET "\n",
           server_ip, server_port);

    if (connect(sock_fd, (struct sockaddr *)&server_addr,
                sizeof(server_addr)) < 0) {
        printf(COLOR_RED "  ✗ Connection failed: %s"
               COLOR_RESET "\n", strerror(errno));
        close(sock_fd);
        return EXIT_FAILURE;
    }

    printf(COLOR_GREEN "  ✓ Connected to server!" COLOR_RESET "\n\n");

    /* ---- Pre-Login Menu ---- */
    char username[MAX_USERNAME], password[MAX_PASSWORD];
    int current_role = -1;
    ClientRequest req;
    ServerResponse resp;
    ssize_t bytes;
    int ch;
    struct sockaddr_in reconnect_addr = server_addr;  /* Save for reconnection */

auth_menu:
    printf(COLOR_BOLD "\n  ┌─── WELCOME ──────────────────────────────┐" COLOR_RESET "\n");
    printf(COLOR_BOLD "  │" COLOR_RESET "  1. " COLOR_GREEN "SIGNUP" COLOR_RESET "  — Create a new account      " COLOR_BOLD "│" COLOR_RESET "\n");
    printf(COLOR_BOLD "  │" COLOR_RESET "  2. " COLOR_CYAN "SIGNIN" COLOR_RESET "  — Log into existing account " COLOR_BOLD "│" COLOR_RESET "\n");
    printf(COLOR_BOLD "  │" COLOR_RESET "  3. " COLOR_RED "EXIT" COLOR_RESET "    — Quit                      " COLOR_BOLD "│" COLOR_RESET "\n");
    printf(COLOR_BOLD "  └────────────────────────────────────────────┘" COLOR_RESET "\n");
    printf("  Select option (1-3): ");

    int option = 0;
    if (scanf("%d", &option) != 1) {
        /* Invalid input (non-numeric) — flush and retry */
        while ((ch = getchar()) != '\n' && ch != EOF);
        printf(COLOR_RED "  ✗ Invalid input. Please enter 1, 2, or 3." COLOR_RESET "\n");
        goto auth_menu;
    }
    /* Consume leftover newline */
    while ((ch = getchar()) != '\n' && ch != EOF);

    if (option == 3) {
        printf(COLOR_DIM "\n  Goodbye!\n" COLOR_RESET);
        close(sock_fd);
        return EXIT_SUCCESS;
    }

    if (option == 1) {
        /* ---- SIGNUP Flow ---- */
        int role_choice;

        printf(COLOR_BOLD "\n  ┌─── SIGNUP ──────────────────────┐" COLOR_RESET "\n");
        printf(COLOR_BOLD "  │" COLOR_RESET " Username: ");
        if (scanf("%31s", username) != 1) { close(sock_fd); return EXIT_FAILURE; }
        printf(COLOR_BOLD "  │" COLOR_RESET " Password: ");
        if (scanf("%31s", password) != 1) { close(sock_fd); return EXIT_FAILURE; }

        /* Role selection (no admin allowed) */
        printf(COLOR_BOLD "  │" COLOR_RESET "\n");
        printf(COLOR_BOLD "  │" COLOR_RESET " Select Role:\n");
        printf(COLOR_BOLD "  │" COLOR_RESET "   1. " COLOR_CYAN "USER" COLOR_RESET "   — Upload, download, edit files\n");
        printf(COLOR_BOLD "  │" COLOR_RESET "   2. " COLOR_YELLOW "GUEST" COLOR_RESET "  — Read-only access\n");
        printf(COLOR_BOLD "  │" COLOR_RESET " Enter choice (1-2): ");
        if (scanf("%d", &role_choice) != 1 || role_choice < 1 || role_choice > 2) {
            printf(COLOR_RED "  │ ✗ Invalid role. Defaulting to USER." COLOR_RESET "\n");
            role_choice = 1;
        }
        printf(COLOR_BOLD "  └────────────────────────────────┘" COLOR_RESET "\n");

        /* Map choice to role constant: 1→ROLE_USER(1), 2→ROLE_GUEST(2) */
        int role = (role_choice == 2) ? ROLE_GUEST : ROLE_USER;

        /* Send SIGNUP request (role passed via data_size) */
        memset(&req, 0, sizeof(req));
        req.operation = OP_SIGNUP;
        strncpy(req.username, username, MAX_USERNAME - 1);
        strncpy(req.password, password, MAX_PASSWORD - 1);
        req.data_size = role;

        send(sock_fd, &req, sizeof(req), 0);

        memset(&resp, 0, sizeof(resp));
        bytes = recv(sock_fd, &resp, sizeof(resp), MSG_WAITALL);

        if (bytes <= 0 || resp.status != STATUS_SUCCESS) {
            print_status(resp.status, resp.message);
            if (resp.status == STATUS_USER_EXISTS) {
                printf(COLOR_YELLOW "  Try signing in instead, or pick a different username." COLOR_RESET "\n");
            }
        } else {
            printf(COLOR_GREEN "\n  ✓ %s" COLOR_RESET "\n", resp.message);
            printf(COLOR_YELLOW "  Please sign in with your new account." COLOR_RESET "\n");
        }

        /* Disconnect and reconnect to return to auth menu */
        close(sock_fd);
        sock_fd = socket(AF_INET, SOCK_STREAM, 0);
        if (sock_fd < 0 || connect(sock_fd, (struct sockaddr *)&reconnect_addr, sizeof(reconnect_addr)) < 0) {
            printf(COLOR_RED "  ✗ Reconnection failed." COLOR_RESET "\n");
            return EXIT_FAILURE;
        }
        if (bytes <= 0 || resp.status != STATUS_SUCCESS) {
            printf(COLOR_DIM "  (Reconnected to server)" COLOR_RESET "\n");
        }
        goto auth_menu;

    } else if (option == 2) {
        /* ---- SIGNIN Flow ---- */
        printf(COLOR_BOLD "\n  ┌─── SIGNIN ──────────────────────┐" COLOR_RESET "\n");
        printf(COLOR_BOLD "  │" COLOR_RESET " Username: ");
        if (scanf("%31s", username) != 1) { close(sock_fd); return EXIT_FAILURE; }
        printf(COLOR_BOLD "  │" COLOR_RESET " Password: ");
        if (scanf("%31s", password) != 1) { close(sock_fd); return EXIT_FAILURE; }
        printf(COLOR_BOLD "  └────────────────────────────────┘" COLOR_RESET "\n");

        /* Send LOGIN request */
        memset(&req, 0, sizeof(req));
        req.operation = OP_LOGIN;
        strncpy(req.username, username, MAX_USERNAME - 1);
        strncpy(req.password, password, MAX_PASSWORD - 1);

        send(sock_fd, &req, sizeof(req), 0);

        memset(&resp, 0, sizeof(resp));
        bytes = recv(sock_fd, &resp, sizeof(resp), MSG_WAITALL);

        if (bytes <= 0 || resp.status != STATUS_SUCCESS) {
            print_status(resp.status, resp.message);
            if (resp.status == STATUS_AUTH_FAIL) {
                printf(COLOR_YELLOW "  User not found or wrong password. Please sign up first." COLOR_RESET "\n");
            }
            /* Reconnect and retry */
            close(sock_fd);
            sock_fd = socket(AF_INET, SOCK_STREAM, 0);
            if (sock_fd < 0 || connect(sock_fd, (struct sockaddr *)&reconnect_addr, sizeof(reconnect_addr)) < 0) {
                printf(COLOR_RED "  ✗ Reconnection failed." COLOR_RESET "\n");
                return EXIT_FAILURE;
            }
            printf(COLOR_DIM "  (Reconnected to server)" COLOR_RESET "\n");
            goto auth_menu;
        }

    } else {
        printf(COLOR_YELLOW "  Invalid option. Try again." COLOR_RESET "\n");
        goto auth_menu;
    }

    printf(COLOR_GREEN "\n  ✓ %s" COLOR_RESET "\n", resp.message);
    current_role = resp.data_size;  /* Set role sent by server */
    print_help();

    /* ---- Command Loop ---- */
    char input[256];
    /* Consume leftover newline from scanf */
    while ((ch = getchar()) != '\n' && ch != EOF);

    while (1) {
        printf(COLOR_MAGENTA "\n  %s" COLOR_RESET " > ", username);
        fflush(stdout);

        if (!fgets(input, sizeof(input), stdin)) break;
        input[strcspn(input, "\r\n")] = '\0';

        if (strlen(input) == 0) continue;

        /* Parse command — up to 3 tokens for ROLLBACK <file> <version> */
        char command[32] = {0};
        char filename[MAX_FILENAME] = {0};
        char arg3[32] = {0};
        sscanf(input, "%31s %63s %31s", command, filename, arg3);

        /* Convert command to uppercase */
        for (int i = 0; command[i]; i++) {
            if (command[i] >= 'a' && command[i] <= 'z')
                command[i] -= 32;
        }

        /* Handle local commands */
        if (strcmp(command, "HELP") == 0) {
            print_help();
            continue;
        }

        if (strcmp(command, "SIGNOUT") == 0 || strcmp(command, "EXIT") == 0 || strcmp(command, "QUIT") == 0) {
            memset(&req, 0, sizeof(req));
            req.operation = OP_EXIT;
            send(sock_fd, &req, sizeof(req), 0);

            memset(&resp, 0, sizeof(resp));
            recv(sock_fd, &resp, sizeof(resp), MSG_WAITALL);
            print_status(resp.status, resp.message);
            break;
        }

        /* Build request */
        memset(&req, 0, sizeof(req));

        if (strcmp(command, "UPLOAD") == 0) {
            if (strlen(filename) == 0) {
                printf(COLOR_YELLOW "  Usage: UPLOAD <filename>" COLOR_RESET "\n");
                continue;
            }
            req.operation = OP_UPLOAD;
            strncpy(req.filename, filename, MAX_FILENAME - 1);

            /* Read local file */
            req.data_size = read_local_file(filename, req.data, MAX_FILE_SIZE);
            if (req.data_size < 0) continue;

            printf(COLOR_DIM "  Uploading '%s' (%d bytes)..."
                   COLOR_RESET "\n", filename, req.data_size);

        } else if (strcmp(command, "DOWNLOAD") == 0) {
            if (strlen(filename) == 0) {
                printf(COLOR_YELLOW "  Usage: DOWNLOAD <filename>" COLOR_RESET "\n");
                continue;
            }
            req.operation = OP_DOWNLOAD;
            strncpy(req.filename, filename, MAX_FILENAME - 1);

        } else if (strcmp(command, "DELETE") == 0) {
            if (strlen(filename) == 0) {
                printf(COLOR_YELLOW "  Usage: DELETE <filename>" COLOR_RESET "\n");
                continue;
            }
            req.operation = OP_DELETE;
            strncpy(req.filename, filename, MAX_FILENAME - 1);

        } else if (strcmp(command, "LIST") == 0) {
            req.operation = OP_LIST;

        } else if (strcmp(command, "VERSION") == 0) {
            if (strlen(filename) == 0) {
                printf(COLOR_YELLOW "  Usage: VERSION <filename>" COLOR_RESET "\n");
                continue;
            }
            req.operation = OP_VERSION;
            strncpy(req.filename, filename, MAX_FILENAME - 1);

        } else if (strcmp(command, "EDIT") == 0) {
            if (strlen(filename) == 0) {
                printf(COLOR_YELLOW "  Usage: EDIT <filename>" COLOR_RESET "\n");
                continue;
            }
            req.operation = OP_EDIT;
            strncpy(req.filename, filename, MAX_FILENAME - 1);
            printf(COLOR_DIM "  Requesting edit lock for '%s'..."
                   COLOR_RESET "\n", filename);

        } else if (strcmp(command, "SAVE") == 0) {
            if (strlen(filename) == 0) {
                printf(COLOR_YELLOW "  Usage: SAVE <filename>" COLOR_RESET "\n");
                continue;
            }
            req.operation = OP_SAVE;
            strncpy(req.filename, filename, MAX_FILENAME - 1);

            /* Read the edited file from downloads/<ROLE>/<USERNAME>/ */
            char edit_path[512];
            snprintf(edit_path, sizeof(edit_path), "downloads/%s/%s/%s", get_role_string(current_role), username, filename);
            req.data_size = read_local_file(edit_path, req.data, MAX_FILE_SIZE);
            if (req.data_size < 0) {
                printf(COLOR_YELLOW "  Tip: Edit the file in downloads/%s/%s/%s, then SAVE"
                       COLOR_RESET "\n", get_role_string(current_role), username, filename);
                continue;
            }
            printf(COLOR_DIM "  Saving '%s' (%d bytes) and releasing lock..."
                   COLOR_RESET "\n", filename, req.data_size);

        } else if (strcmp(command, "UNLOCK") == 0) {
            if (strlen(filename) == 0) {
                printf(COLOR_YELLOW "  Usage: UNLOCK <filename>" COLOR_RESET "\n");
                continue;
            }
            req.operation = OP_UNLOCK;
            strncpy(req.filename, filename, MAX_FILENAME - 1);

        } else if (strcmp(command, "HISTORY") == 0) {
            if (strlen(filename) == 0) {
                printf(COLOR_YELLOW "  Usage: HISTORY <filename>" COLOR_RESET "\n");
                continue;
            }
            req.operation = OP_HISTORY;
            strncpy(req.filename, filename, MAX_FILENAME - 1);

        } else if (strcmp(command, "ROLLBACK") == 0) {
            if (strlen(filename) == 0 || strlen(arg3) == 0) {
                printf(COLOR_YELLOW "  Usage: ROLLBACK <filename> <version>"
                       COLOR_RESET "\n");
                continue;
            }
            req.operation = OP_ROLLBACK;
            strncpy(req.filename, filename, MAX_FILENAME - 1);
            req.data_size = atoi(arg3);  /* Reuse data_size to pass version */
            printf(COLOR_DIM "  Rolling back '%s' to version %d..."
                   COLOR_RESET "\n", filename, req.data_size);

        } else if (strcmp(command, "DELVERSION") == 0) {
            if (strlen(filename) == 0 || strlen(arg3) == 0) {
                printf(COLOR_YELLOW "  Usage: DELVERSION <filename> <version>"
                       COLOR_RESET "\n");
                continue;
            }
            req.operation = OP_DELVERSION;
            strncpy(req.filename, filename, MAX_FILENAME - 1);
            req.data_size = atoi(arg3);  /* Reuse data_size to pass version */
            printf(COLOR_DIM "  Deleting version %d of '%s'..."
                   COLOR_RESET "\n", req.data_size, filename);


        } else if (strcmp(command, "AUDIT") == 0) {
            req.operation = OP_AUDIT;

        } else {
            printf(COLOR_YELLOW "  Unknown command: %s. Type HELP."
                   COLOR_RESET "\n", command);
            continue;
        }

        /* Send request */
        send(sock_fd, &req, sizeof(req), 0);

        /* Receive response */
        memset(&resp, 0, sizeof(resp));
        bytes = recv(sock_fd, &resp, sizeof(resp), MSG_WAITALL);

        if (bytes <= 0) {
            printf(COLOR_RED "  ✗ Lost connection to server"
                   COLOR_RESET "\n");
            break;
        }

        /* Display response */
        print_status(resp.status, resp.message);

        /* Show data if present */
        if (resp.data_size > 0) {
            if (req.operation == OP_DOWNLOAD || req.operation == OP_EDIT) {
                /* Save downloaded/edit file locally */
                save_local_file(filename, resp.data, resp.data_size, username, current_role);
                if (req.operation == OP_EDIT) {
                    printf(COLOR_CYAN
                           "  [TIP] Edit the file at downloads/%s/%s/%s, then run SAVE %s"
                           COLOR_RESET "\n", get_role_string(current_role), username, filename, filename);
                }
            } else {
                /* Display data (LIST, VERSION, HISTORY, AUDIT output) */
                printf("%.*s", resp.data_size, resp.data);
            }
        }
    }

    close(sock_fd);
    printf(COLOR_DIM "\n  Disconnected.\n" COLOR_RESET);
    return EXIT_SUCCESS;
}
