/**
 * @file command.c
 * @brief Command template substitution and execution with timeout support.
 *
 * TRUST MODEL: Template values are substituted **unescaped** into a
 * `/bin/sh -c` command line. The wfb-ng tunnel between GS and drone is
 * trusted (encrypted, authenticated link), so the GS is responsible for
 * sanitizing any value that ends up in a placeholder before sending it.
 * If this trust assumption ever changes (e.g. inputs from an untrusted
 * source), `replace_placeholder` and `cmd_exec_with_timeout` must be
 * replaced with an `execv()`-based path that takes argv arrays directly,
 * bypassing the shell entirely.
 */
#include "command.h"
#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/select.h>
#include <sys/time.h>
#include <errno.h>
#include <signal.h>
#include <fcntl.h>
#include <time.h>

#define DEFAULT_EXEC_TIMEOUT_MS 500
#define WFB_RECV_TIMEOUT_MS     100

/* wfb_tx command protocol opcodes (matches wfb-ng/src/tx_cmd.h). */
#define WFB_CMD_SET_FEC   1
#define WFB_CMD_SET_RADIO 2
#define WFB_CMD_GET_STATS 5

/* Substitutes `value` into `str` UNESCAPED — see TRUST MODEL at top of file.
 * Returns 0 on success, -1 if substitution would overflow MAX_COMMAND_SIZE
 * (in which case `str` is left unchanged and an error is logged). */
static int replace_placeholder(char *str, const char *placeholder, const char *value,
                               log_level_t log_level) {
    char *pos = strstr(str, placeholder);
    if (!pos)
        return 0;

    size_t prefix_len = (size_t)(pos - str);
    size_t value_len = strlen(value);
    size_t ph_len = strlen(placeholder);
    size_t suffix_len = strlen(pos + ph_len);
    size_t needed = prefix_len + value_len + suffix_len + 1; /* + NUL */

    if (needed > MAX_COMMAND_SIZE) {
        ERROR_LOG_LEVEL(log_level,
                        "Command template truncation: substituting '%s'='%s' "
                        "needs %zu bytes, max %d. Aborting substitution.\n",
                        placeholder, value, needed, MAX_COMMAND_SIZE);
        return -1;
    }

    char buffer[MAX_COMMAND_SIZE];
    memcpy(buffer, str, prefix_len);
    memcpy(buffer + prefix_len, value, value_len);
    memcpy(buffer + prefix_len + value_len, pos + ph_len, suffix_len);
    buffer[prefix_len + value_len + suffix_len] = '\0';
    memcpy(str, buffer, prefix_len + value_len + suffix_len + 1);
    return 0;
}

void cmd_init(cmd_ctx_t *ctx, long pace_exec_us, log_level_t log_level, int wfb_ctl_port) {
    ctx->pace_exec_us = pace_exec_us;
    ctx->exec_timeout_ms = DEFAULT_EXEC_TIMEOUT_MS;
    ctx->log_level = log_level;
    ctx->wfb_ctl_fd = -1;
    ctx->wfb_ctl_port = wfb_ctl_port;
    pthread_mutex_init(&ctx->exec_mutex, NULL);
    pthread_mutex_init(&ctx->wfb_mutex, NULL);
}

/**
 * Execute a command using fork()/exec() with timeout.
 * Uses pipe + select() for millisecond-precision timeout (unlike alarm() which is seconds-only).
 * Returns 0 on success, non-zero exit code on failure, -1 on timeout.
 */
static int exec_with_timeout(const char *command, int timeout_ms, log_level_t log_level) {
    int pipefd[2];
    pid_t pid;
    int status;
    
    // Create pipe for child lifetime tracking (death signal)
    if (pipe(pipefd) < 0) {
        perror("pipe");
        return -1;
    }
    
    pid = fork();
    
    if (pid < 0) {
        perror("fork");
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }
    
    if (pid == 0) {
        // Child process
        close(pipefd[0]);  // Close read end
        
        // Redirect stdout safely to /dev/null to prevent log spam
        // We do NOT redirect to the pipe, so select() only triggers on process exit.
        int nullfd = open("/dev/null", O_WRONLY);
        if (nullfd >= 0) {
            dup2(nullfd, STDOUT_FILENO);
            if (log_level < LOG_LEVEL_DEBUG) {
                dup2(nullfd, STDERR_FILENO);
            }
            close(nullfd);
        }
        
        // pipefd[1] remains open. The OS will close it when exactly this process tree dies.
        execl("/bin/sh", "sh", "-c", command, NULL);
        _exit(127);  // execl failed
    }
    
    // Parent process
    close(pipefd[1]);  // Close write end
    
    // Set up select with timeout waiting for EOF on the pipe
    fd_set readfds;
    struct timeval tv;
    int select_result;
    
    FD_ZERO(&readfds);
    FD_SET(pipefd[0], &readfds);
    
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    
    // Block until the child closes the pipe (i.e. exits) or we timeout
    select_result = select(pipefd[0] + 1, &readfds, NULL, NULL, &tv);
    close(pipefd[0]);
    
    if (select_result <= 0) {
        // select_result == 0 is Timeout, < 0 is error/interrupted
        if (select_result == 0) {
            ERROR_LOG_LEVEL(log_level, "Command timed out after %d ms: %s\n", timeout_ms, command);
        } else if (select_result < 0) {
            ERROR_LOG_LEVEL(log_level, "select interrupted: %s\n", strerror(errno));
        }
        kill(pid, SIGKILL);
        waitpid(pid, &status, 0);
        return -1;
    }
    
    // Command completed - wait for child to reap
    pid_t waited = waitpid(pid, &status, 0);
    
    if (waited < 0) {
        perror("waitpid");
        return -1;
    }
    
    if (WIFEXITED(status)) {
        int exit_code = WEXITSTATUS(status);
        if (exit_code != 0) {
            ERROR_LOG_LEVEL(log_level, "Command failed with status %d: %s\n", exit_code, command);
        }
        return exit_code;
    }
    
    if (WIFSIGNALED(status)) {
        ERROR_LOG_LEVEL(log_level, "Command killed by signal %d: %s\n", WTERMSIG(status), command);
        return -1;
    }
    
    return -1;
}

void cmd_format(char *dest, size_t dest_size, const char *tmpl,
                int count, const char **keys, const char **values,
                log_level_t log_level) {
    char temp[MAX_COMMAND_SIZE];
    strncpy(temp, tmpl, sizeof(temp));
    temp[sizeof(temp)-1] = '\0';
    char placeholder[64];
    for (int i = 0; i < count; i++) {
        snprintf(placeholder, sizeof(placeholder), "{%s}", keys[i]);
        replace_placeholder(temp, placeholder, values[i], log_level);
    }
    strncpy(dest, temp, dest_size);
    dest[dest_size-1] = '\0';
}

int cmd_exec_with_timeout(const cmd_ctx_t *ctx, const char *command) {
    /* exec_mutex is logically mutable even for a "const" ctx — it protects
     * shared execution resources, not the ctx's configuration fields. */
    pthread_mutex_t *mu = (pthread_mutex_t *)&ctx->exec_mutex;
    pthread_mutex_lock(mu);
    int result = exec_with_timeout(command, ctx->exec_timeout_ms, ctx->log_level);
    pthread_mutex_unlock(mu);

    /* Pace outside the mutex: `iw`/majestic drivers may need settle time
     * between successive netlink ops, but other threads (OSD wfb stats,
     * tx_monitor) have no reason to wait for our sleep to finish. */
    if (ctx->pace_exec_us > 0) {
        usleep(ctx->pace_exec_us);
    }
    return result;
}

int cmd_http_get(const char *host, int port, const char *path,
                 char *response, size_t resp_size, const cmd_ctx_t *ctx) {
    INFO_LOG_LEVEL(ctx->log_level, "HTTP GET %s:%d%s\n", host, port, path);

    pthread_mutex_t *mu = (pthread_mutex_t *)&ctx->exec_mutex;
    pthread_mutex_lock(mu);
    int result = http_get(host, port, path, response, resp_size, ctx->exec_timeout_ms);
    pthread_mutex_unlock(mu);

    return result;
}

static void wfb_seed_rand(void) {
    srand((unsigned)(time(NULL) ^ getpid()));
}

/* Build, send, and await ack for a wfb_tx control-socket request. Returns 0
 * on success (daemon rc == 0), non-zero on local error or daemon rc != 0.
 * Wire format matches wfb-ng/src/tx_cmd.h (packed): req_id(net u32) +
 * cmd_id(u8) + payload. Reply: req_id(echo) + rc(net u32) + optional extra.
 *
 * If resp_extra != NULL, up to resp_extra_max bytes of the reply payload
 * AFTER the 8-byte header are copied into it, and *resp_extra_len is set
 * to the actual number received (may be 0 if the daemon returned a bare
 * error reply). Pass NULL/0/NULL when no extra reply is expected. */
static int wfb_send_request(cmd_ctx_t *ctx, uint8_t cmd_id,
                            const uint8_t *payload, size_t payload_len,
                            uint8_t *resp_extra, size_t resp_extra_max,
                            size_t *resp_extra_len) {
    static pthread_once_t rand_once = PTHREAD_ONCE_INIT;
    pthread_once(&rand_once, wfb_seed_rand);

    if (resp_extra_len) *resp_extra_len = 0;

    pthread_mutex_lock(&ctx->wfb_mutex);

    if (ctx->wfb_ctl_fd < 0) {
        int fd = socket(AF_INET, SOCK_DGRAM, 0);
        if (fd < 0) {
            ERROR_LOG_LEVEL(ctx->log_level, "wfb: socket() failed: %s\n", strerror(errno));
            pthread_mutex_unlock(&ctx->wfb_mutex);
            return -1;
        }
        struct timeval tv = { .tv_sec = 0, .tv_usec = WFB_RECV_TIMEOUT_MS * 1000L };
        if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0) {
            ERROR_LOG_LEVEL(ctx->log_level, "wfb: SO_RCVTIMEO failed: %s\n", strerror(errno));
            close(fd);
            pthread_mutex_unlock(&ctx->wfb_mutex);
            return -1;
        }
        ctx->wfb_ctl_fd = fd;
    }

    /* Packet: 5-byte header + up to 7 bytes payload (set_radio is largest). */
    uint8_t buf[5 + 7];
    if (payload_len > sizeof(buf) - 5) {
        ERROR_LOG_LEVEL(ctx->log_level, "wfb: payload_len %zu exceeds buffer\n", payload_len);
        pthread_mutex_unlock(&ctx->wfb_mutex);
        return -1;
    }
    uint32_t req_id_net = htonl((uint32_t)rand());
    memcpy(buf, &req_id_net, 4);
    buf[4] = cmd_id;
    if (payload_len > 0) memcpy(buf + 5, payload, payload_len);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)ctx->wfb_ctl_port);
    addr.sin_addr.s_addr = htonl(0x7f000001); /* 127.0.0.1 */

    /* Drain any stale replies left in the socket buffer before sending our
     * own request. A previous request may have timed out (SO_RCVTIMEO) while
     * the daemon was still processing it; its reply arrives after we gave
     * up and sits here poisoning the next recv(). This loop costs one
     * syscall when the queue is empty (EAGAIN) and a few when it isn't. */
    uint8_t junk[8 + 48];
    int drained = 0;
    while (recv(ctx->wfb_ctl_fd, junk, sizeof(junk), MSG_DONTWAIT) >= 0) {
        drained += 1;
        if (drained > 32) break;  /* defensive cap, should never fire */
    }
    if (drained > 0) {
        DEBUG_LOG_LEVEL(ctx->log_level, "wfb: drained %d stale replies before cmd_id=%u\n",
                        drained, cmd_id);
    }

    ssize_t sent = sendto(ctx->wfb_ctl_fd, buf, 5 + payload_len, 0,
                          (struct sockaddr *)&addr, sizeof(addr));
    if (sent < 0 || (size_t)sent != 5 + payload_len) {
        ERROR_LOG_LEVEL(ctx->log_level, "wfb: sendto failed: %s\n", strerror(errno));
        pthread_mutex_unlock(&ctx->wfb_mutex);
        return -1;
    }

    /* Reply buffer sized for the largest known reply: 8-byte header plus a
     * cmd_get_stats payload (6 × uint64 = 48 bytes). Future variants must
     * fit here or bump the size. */
    uint8_t resp[8 + 48];
    ssize_t nrecv;
    int mismatch = 0;
    for (;;) {
        nrecv = recv(ctx->wfb_ctl_fd, resp, sizeof(resp), 0);
        if (nrecv < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                ERROR_LOG_LEVEL(ctx->log_level, "wfb: reply timeout (cmd_id=%u)\n", cmd_id);
            } else {
                ERROR_LOG_LEVEL(ctx->log_level, "wfb: recv failed: %s\n", strerror(errno));
            }
            pthread_mutex_unlock(&ctx->wfb_mutex);
            return -1;
        }
        if (nrecv >= 8 && memcmp(resp, &req_id_net, 4) == 0) {
            /* Matched our req_id, proceed. */
            break;
        }
        /* Stale or corrupt reply. Drop it and keep waiting — our own reply
         * may still be coming. Cap retries so we eventually time out
         * instead of spinning on a misbehaving daemon. */
        mismatch += 1;
        DEBUG_LOG_LEVEL(ctx->log_level,
                        "wfb: skipping unmatched reply (nrecv=%zd, cmd_id=%u)\n",
                        nrecv, cmd_id);
        if (mismatch > 8) {
            ERROR_LOG_LEVEL(ctx->log_level,
                            "wfb: too many unmatched replies (cmd_id=%u)\n", cmd_id);
            pthread_mutex_unlock(&ctx->wfb_mutex);
            return -1;
        }
    }

    uint32_t rc_net;
    memcpy(&rc_net, resp + 4, 4);
    int rc = (int)ntohl(rc_net);

    if (rc == 0 && resp_extra && resp_extra_max > 0) {
        size_t extra = (size_t)nrecv - 8;
        if (extra > resp_extra_max) extra = resp_extra_max;
        if (extra > 0) memcpy(resp_extra, resp + 8, extra);
        if (resp_extra_len) *resp_extra_len = extra;
    }

    pthread_mutex_unlock(&ctx->wfb_mutex);

    if (rc != 0) {
        ERROR_LOG_LEVEL(ctx->log_level, "wfb: daemon rc=%d (%s) for cmd_id=%u\n",
                        rc, strerror(rc), cmd_id);
    }
    return rc;
}

int cmd_wfb_set_fec(cmd_ctx_t *ctx, uint8_t k, uint8_t n) {
    uint8_t payload[2] = { k, n };
    return wfb_send_request(ctx, WFB_CMD_SET_FEC, payload, sizeof(payload),
                            NULL, 0, NULL);
}

int cmd_wfb_set_radio(cmd_ctx_t *ctx,
                      uint8_t stbc, bool ldpc, bool short_gi,
                      uint8_t bandwidth, uint8_t mcs_index,
                      bool vht_mode, uint8_t vht_nss) {
    uint8_t payload[7];
    payload[0] = stbc;
    payload[1] = ldpc ? 1 : 0;
    payload[2] = short_gi ? 1 : 0;
    payload[3] = bandwidth;
    payload[4] = mcs_index;
    payload[5] = vht_mode ? 1 : 0;
    payload[6] = vht_nss;
    return wfb_send_request(ctx, WFB_CMD_SET_RADIO, payload, sizeof(payload),
                            NULL, 0, NULL);
}

int cmd_wfb_get_stats(cmd_ctx_t *ctx, wfb_stats_t *out) {
    if (!out) return -1;
    /* Wire format: 6 × uint64_t in host byte order (loopback, same machine). */
    uint8_t buf[48];
    size_t got = 0;
    int rc = wfb_send_request(ctx, WFB_CMD_GET_STATS, NULL, 0,
                              buf, sizeof(buf), &got);
    if (rc != 0) return rc;
    if (got != sizeof(buf)) {
        ERROR_LOG_LEVEL(ctx->log_level, "wfb: short stats reply (%zu bytes)\n", got);
        return -1;
    }
    memcpy(&out->p_fec_timeouts, buf + 0,  8);
    memcpy(&out->p_incoming,     buf + 8,  8);
    memcpy(&out->p_injected,     buf + 16, 8);
    memcpy(&out->b_injected,     buf + 24, 8);
    memcpy(&out->p_dropped,      buf + 32, 8);
    memcpy(&out->p_truncated,    buf + 40, 8);
    return 0;
}