# ==============================================================================
# Makefile — Cloud-Based Distributed File Synchronization System
# ==============================================================================
# Usage:
#   make          — Build both server and client
#   make server   — Build server only
#   make client   — Build client only
#   make clean    — Remove all build artifacts
#   make run-server — Build and run the server
#   make run-client — Build and run the client
#   make ipc-clean  — Remove leftover IPC resources
# ==============================================================================

CC      = gcc
CFLAGS  = -Wall -Wextra -pthread -I include
LDFLAGS = -lpthread -lrt

# Source files
SERVER_SRC = src/server.c src/auth.c src/ipc.c src/sync.c \
             src/file_manager.c src/metadata.c src/worker.c
CLIENT_SRC = src/client.c

# Targets
SERVER = server
CLIENT = client

.PHONY: all clean run-server run-client ipc-clean setup

all: setup $(SERVER) $(CLIENT)
	@echo ""
	@echo "\033[1;32m╔═══════════════════════════════════════╗\033[0m"
	@echo "\033[1;32m║  Build successful!                    ║\033[0m"
	@echo "\033[1;32m║  Run: ./server  (in terminal 1)       ║\033[0m"
	@echo "\033[1;32m║  Run: ./client  (in terminal 2)       ║\033[0m"
	@echo "\033[1;32m╚═══════════════════════════════════════╝\033[0m"

$(SERVER): $(SERVER_SRC)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)
	@echo "\033[1;32m  ✓ Server built successfully\033[0m"

$(CLIENT): $(CLIENT_SRC)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)
	@echo "\033[1;32m  ✓ Client built successfully\033[0m"

setup:
	@mkdir -p storage

clean:
	-rm -f $(SERVER) $(CLIENT) $(SERVER).exe $(CLIENT).exe 2>/dev/null || true
	-rm -rf storage downloads audit.log 2>/dev/null || true
	-rm -f /tmp/dfs_audit_pipe 2>/dev/null || true
	@echo 1:admin:admin123:0 > users.dat
	@echo "\033[1;33m  ✓ Clean complete (users.dat reset to admin only)\033[0m"

run-server: $(SERVER)
	./$(SERVER)

run-client: $(CLIENT)
	./$(CLIENT)

# Remove leftover System V IPC resources (useful after crashes)
ipc-clean:
	@echo "Removing leftover IPC resources..."
	-ipcrm -Q 0x1234 2>/dev/null || true
	-ipcrm -Q 0x1235 2>/dev/null || true
	-ipcrm -M 0x1236 2>/dev/null || true
	-ipcrm -S 0x1237 2>/dev/null || true
	rm -f /tmp/dfs_audit_pipe
	@echo "\033[1;32m  ✓ IPC resources cleaned\033[0m"
