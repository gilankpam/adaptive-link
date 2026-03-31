/**
 * @file tx_monitor.h
 * @brief TX drop monitoring thread.
 *
 * Periodically reads the wlan0 TX dropped counter and reduces bitrate
 * when drops are detected. Restores normal bitrate after a quiet period.
 * Also requests keyframes on TX drops.
 */
#ifndef ALINK_TX_MONITOR_H
#define ALINK_TX_MONITOR_H

#include "alink_types.h"
#include "config.h"
#include "hardware.h"
#include "command.h"
#include "profile.h"
#include "keyframe.h"

typedef struct {
    profile_state_t *ps;
    keyframe_state_t *ks;
    hw_state_t *hw;
    alink_config_t *cfg;
    cmd_ctx_t *cmd;
    volatile bool *initialized;
} txmon_thread_arg_t;

void *txmon_thread_func(void *arg);

#endif /* ALINK_TX_MONITOR_H */
