/**
 * @file cmd_server.h
 * @brief Unix domain socket command listener thread.
 *
 * Listens on /tmp/alink_cmd.sock for binary protocol commands from
 * external tools (e.g., air_man). Handles SET_POWER, ANTENNA_STATS,
 * GET, and SET commands.
 */
#ifndef ALINK_CMD_SERVER_H
#define ALINK_CMD_SERVER_H

#include "alink_types.h"
#include "config.h"
#include "profile.h"
#include "hardware.h"
#include "rssi_monitor.h"
#include "osd.h"

typedef struct {
    alink_config_t *cfg;
    profile_state_t *ps;
    hw_state_t *hw;
    rssi_state_t *rs;
    osd_state_t *osd;
    pthread_mutex_t *tx_power_mutex;
    pthread_mutex_t *pause_mutex;
    volatile bool *paused;
} cmdsrv_thread_arg_t;

void *cmdsrv_thread_func(void *arg);

#endif /* ALINK_CMD_SERVER_H */
