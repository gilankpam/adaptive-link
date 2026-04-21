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
#include <stdint.h>

typedef struct {
    long pace_exec_us;    /* microseconds between commands */
    int exec_timeout_ms;  /* timeout for command execution in milliseconds */
    log_level_t log_level;
    /* Serializes cmd_exec_with_timeout(), cmd_http_get(), and cmd_wfb_*
     * across threads so the profile worker and tx_monitor don't race each
     * other — e.g. two concurrent HTTP GETs to the camera majestic API. */
    pthread_mutex_t exec_mutex;

    /* wfb_tx control socket — lazily opened on first cmd_wfb_* call. */
    int wfb_ctl_fd;       /* -1 until first use */
    int wfb_ctl_port;     /* from alink_config_t.wfb_control_port */
} cmd_ctx_t;

void cmd_init(cmd_ctx_t *ctx, long pace_exec_us, log_level_t log_level, int wfb_ctl_port);
void cmd_format(char *dest, size_t dest_size, const char *tmpl,
                int count, const char **keys, const char **values,
                log_level_t log_level);

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

/**
 * Send CMD_SET_FEC to the wfb_tx daemon on 127.0.0.1:wfb_ctl_port.
 * Waits for the reply with a ~100 ms recv timeout, verifies the echoed
 * req_id, and returns the daemon's rc (0 on success, non-zero on failure
 * or local send/recv error).
 */
int cmd_wfb_set_fec(cmd_ctx_t *ctx, uint8_t k, uint8_t n);

/**
 * Send CMD_SET_RADIO to the wfb_tx daemon. Caller passes raw wire values;
 * no string-to-bool translation inside. Returns 0 on success.
 */
int cmd_wfb_set_radio(cmd_ctx_t *ctx,
                      uint8_t stbc, bool ldpc, bool short_gi,
                      uint8_t bandwidth, uint8_t mcs_index,
                      bool vht_mode, uint8_t vht_nss);

#endif /* ALINK_COMMAND_H */