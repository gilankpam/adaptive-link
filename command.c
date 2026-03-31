/**
 * @file command.c
 * @brief Command template substitution and system() execution.
 */
#include "command.h"

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
    ctx->verbose = verbose;
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

void cmd_exec_noquote(const cmd_ctx_t *ctx, const char *command) {
    if (ctx->verbose) {
        puts(command);
    }

    if (system(command) != 0) { printf("Command failed: %s\n", command); }
    usleep(ctx->pace_exec_us);
}

void cmd_exec(const cmd_ctx_t *ctx, const char *command) {
    char quotedCommand[BUFFER_SIZE];
    snprintf(quotedCommand, sizeof(quotedCommand), "\"%s\"", command);
    if (ctx->verbose) {
        puts(quotedCommand);
    }
    if (system(quotedCommand) != 0) { printf("Command failed: %s\n", quotedCommand); }
    if (ctx->verbose) {
        printf("Waiting %ldms\n", ctx->pace_exec_us / 1000);
    }
    usleep(ctx->pace_exec_us);
}
