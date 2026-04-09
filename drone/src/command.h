/**
 * @file command.h
 * @brief Command template substitution and execution.
 *
 * Provides functions to format command strings by replacing {placeholder}
 * tokens and execute them with timeout support.
 * Also provides native HTTP client for faster API calls.
 */
#ifndef ALINK_COMMAND_H
#define ALINK_COMMAND_H

#include "alink_types.h"
#include "http_client.h"

typedef struct {
    long pace_exec_us;    /* microseconds between commands */
    int exec_timeout_ms;  /* timeout for command execution in milliseconds */
    log_level_t log_level;
    /* Serializes cmd_exec_with_timeout() and cmd_http_get() across threads
     * so the profile worker and tx_monitor don't race each other — e.g. two
     * concurrent HTTP GETs to the camera majestic API. */
    pthread_mutex_t exec_mutex;
} cmd_ctx_t;

void cmd_init(cmd_ctx_t *ctx, long pace_exec_us, log_level_t log_level);
void cmd_format(char *dest, size_t dest_size, const char *tmpl,
                int count, const char **keys, const char **values);

/**
 * Execute a shell command with timeout using fork()/exec().
 * Faster than system() and prevents hanging.
 * 
 * @param ctx     Command context with timeout settings
 * @param command Command string to execute
 * @return 0 on success, non-zero on failure or timeout
 */
int cmd_exec_with_timeout(const cmd_ctx_t *ctx, const char *command);

/**
 * Execute an HTTP GET request using native socket (no curl).
 * Much faster than spawning curl process.
 * 
 * @param host      Target host (e.g., "localhost")
 * @param port      Target port (e.g., 80)
 * @param path      URL path with query string
 * @param response  Buffer for response body (optional, can be NULL)
 * @param resp_size Size of response buffer
 * @param ctx       Command context for timeout and verbose settings
 * @return 0 on success, non-zero on failure
 */
int cmd_http_get(const char *host, int port, const char *path,
                 char *response, size_t resp_size, const cmd_ctx_t *ctx);

#endif /* ALINK_COMMAND_H */