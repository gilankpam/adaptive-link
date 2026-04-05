/**
 * @file command.c
 * @brief Command template substitution and execution with timeout support.
 */
#include "command.h"
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

static void replace_placeholder(char *str, const char *placeholder, const char *value) {
    char buffer[MAX_COMMAND_SIZE];
    char *pos = strstr(str, placeholder);
    if (!pos)
        return;
    size_t prefix_len = pos - str;
    buffer[0] = '\0';
    strncat(buffer, str, prefix_len);
    strncat(buffer, value, sizeof(buffer) - strlen(buffer) - 1);
    strncat(buffer, pos + strlen(placeholder), sizeof(buffer) - strlen(buffer) - 1);
    strncpy(str, buffer, MAX_COMMAND_SIZE);
    str[MAX_COMMAND_SIZE-1] = '\0';
}

void cmd_init(cmd_ctx_t *ctx, long pace_exec_us, bool verbose) {
    ctx->pace_exec_us = pace_exec_us;
    ctx->exec_timeout_ms = DEFAULT_EXEC_TIMEOUT_MS;
    ctx->verbose = verbose;
}

/**
 * Execute a command using fork()/exec() with timeout.
 * Uses pipe + select() for millisecond-precision timeout (unlike alarm() which is seconds-only).
 * Returns 0 on success, non-zero exit code on failure, -1 on timeout.
 */
static int exec_with_timeout(const char *command, int timeout_ms, bool verbose) {
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
            if (!verbose) {
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
        if (select_result == 0 && verbose) {
            fprintf(stderr, "Command timed out after %d ms: %s\n", timeout_ms, command);
        } else if (select_result < 0 && verbose) {
            perror("select interrupted");
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
        if (verbose && exit_code != 0) {
            fprintf(stderr, "Command failed with status %d: %s\n", exit_code, command);
        }
        return exit_code;
    }
    
    if (WIFSIGNALED(status)) {
        if (verbose) {
            fprintf(stderr, "Command killed by signal %d: %s\n", WTERMSIG(status), command);
        }
        return -1;
    }
    
    return -1;
}

void cmd_format(char *dest, size_t dest_size, const char *tmpl,
                int count, const char **keys, const char **values) {
    char temp[MAX_COMMAND_SIZE];
    strncpy(temp, tmpl, sizeof(temp));
    temp[sizeof(temp)-1] = '\0';
    char placeholder[64];
    for (int i = 0; i < count; i++) {
        snprintf(placeholder, sizeof(placeholder), "{%s}", keys[i]);
        replace_placeholder(temp, placeholder, values[i]);
    }
    strncpy(dest, temp, dest_size);
    dest[dest_size-1] = '\0';
}

int cmd_exec_with_timeout(const cmd_ctx_t *ctx, const char *command) {
    int result = exec_with_timeout(command, ctx->exec_timeout_ms, ctx->verbose);
    if (ctx->pace_exec_us > 0) {
        usleep(ctx->pace_exec_us);
    }
    return result;
}

int cmd_http_get(const char *host, int port, const char *path,
                 char *response, size_t resp_size, const cmd_ctx_t *ctx) {
    if (ctx->verbose) {
        printf("HTTP GET %s:%d%s\n", host, port, path);
    }
    
    int result = http_get(host, port, path, response, resp_size, ctx->exec_timeout_ms);
    
    if (ctx->pace_exec_us > 0) {
        usleep(ctx->pace_exec_us);
    }
    
    return result;
}