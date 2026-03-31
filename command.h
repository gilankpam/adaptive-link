/**
 * @file command.h
 * @brief Command template substitution and system() execution.
 *
 * Provides functions to format command strings by replacing {placeholder}
 * tokens and execute them via system() with configurable pacing.
 */
#ifndef ALINK_COMMAND_H
#define ALINK_COMMAND_H

#include "alink_types.h"

typedef struct {
    long pace_exec_us;    /* microseconds between commands */
    bool verbose;
} cmd_ctx_t;

void cmd_init(cmd_ctx_t *ctx, long pace_exec_us, bool verbose);
void cmd_format(char *dest, size_t dest_size, const char *tmpl,
                int count, const char **keys, const char **values);
void cmd_exec(const cmd_ctx_t *ctx, const char *command);
void cmd_exec_noquote(const cmd_ctx_t *ctx, const char *command);

#endif /* ALINK_COMMAND_H */
