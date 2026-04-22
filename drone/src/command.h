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

    /* Two mutexes, decoupled: HTTP/shell is slow (hundreds of ms) and
     * serialises majestic API + `iw` (neither is concurrency-safe); wfb is
     * fast (<1 ms local UDP) and serialises the wfb_tx control socket's
     * stateful send/recv+req_id matching. Splitting them prevents OSD's
     * per-second wfb_get_stats from queuing behind the profile worker's
     * HTTP call, and vice versa. */
    pthread_mutex_t exec_mutex;  /* covers cmd_exec_with_timeout + cmd_http_get */
    pthread_mutex_t wfb_mutex;   /* covers cmd_wfb_* control-socket ops */

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

/* Cumulative counters returned by CMD_GET_STATS. See wfb-ng/src/tx_cmd.h
 * and wfb-ng/src/tx.cpp data_source() for semantics. All values are
 * monotonic uint64 since wfb_tx startup; caller takes deltas. */
typedef struct {
    uint64_t p_fec_timeouts;  /* FEC-only pad packets sent (only >0 when wfb_tx -T > 0) */
    uint64_t p_incoming;      /* UDP packets received from encoder (inc. RXQ-dropped) */
    uint64_t p_injected;      /* Successfully injected packets (includes parity) */
    uint64_t b_injected;      /* Successfully injected bytes */
    uint64_t p_dropped;       /* Dropped due to RXQ overflow or injection timeout */
    uint64_t p_truncated;     /* Packets truncated because payload exceeds MAX_FEC_PAYLOAD */
} wfb_stats_t;

/**
 * Send CMD_GET_STATS to the wfb_tx daemon and populate *out on success.
 * Returns 0 on success, non-zero if wfb_tx is old (lacks CMD_GET_STATS,
 * replies with ENOTSUP) or on any transport error. Caller is expected to
 * degrade gracefully on failure.
 */
int cmd_wfb_get_stats(cmd_ctx_t *ctx, wfb_stats_t *out);

#endif /* ALINK_COMMAND_H */