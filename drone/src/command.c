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
#include <errno.h>
#include <signal.h>
#include <fcntl.h>

#define DEFAULT_EXEC_TIMEOUT_MS 500

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

void cmd_init(cmd_ctx_t *ctx, long pace_exec_us, log_level_t log_level) {
    ctx->pace_exec_us = pace_exec_us;
    ctx->exec_timeout_ms = DEFAULT_EXEC_TIMEOUT_MS;
    ctx->log_level = log_level;
    pthread_mutex_init(&ctx->exec_mutex, NULL);
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
    if (ctx->pace_exec_us > 0) {
        usleep(ctx->pace_exec_us);
    }
    pthread_mutex_unlock(mu);
    return result;
}

int cmd_http_get(const char *host, int port, const char *path,
                 char *response, size_t resp_size, const cmd_ctx_t *ctx) {
    INFO_LOG_LEVEL(ctx->log_level, "HTTP GET %s:%d%s\n", host, port, path);

    pthread_mutex_t *mu = (pthread_mutex_t *)&ctx->exec_mutex;
    pthread_mutex_lock(mu);
    int result = http_get(host, port, path, response, resp_size, ctx->exec_timeout_ms);

    if (ctx->pace_exec_us > 0) {
        usleep(ctx->pace_exec_us);
    }
    pthread_mutex_unlock(mu);

    return result;
}