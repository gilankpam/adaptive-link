/**
 * @file message.h
 * @brief UDP message parsing and processing.
 *
 * Parses incoming "P:" profile messages from the ground station.
 * The GS sends the current profile on every tick (even when unchanged)
 * for reliability over lossy UDP. The drone's delta detection prevents
 * redundant command execution.
 */
#ifndef ALINK_MESSAGE_H
#define ALINK_MESSAGE_H

#include "alink_types.h"
#include "config.h"
#include "profile.h"
#include "keyframe.h"
#include "osd.h"

typedef struct {
    profile_state_t *ps;
    keyframe_state_t *ks;
    osd_state_t *osd;
    alink_config_t *cfg;
    pthread_mutex_t *pause_mutex;
    volatile bool *paused;
    const cmd_ctx_t *cmd;

    bool time_synced;
    bool gs_connected;
} msg_state_t;

void msg_init(msg_state_t *ms, profile_state_t *ps, keyframe_state_t *ks,
              osd_state_t *osd, alink_config_t *cfg,
              pthread_mutex_t *pause_mutex, volatile bool *paused,
              const cmd_ctx_t *cmd);

void msg_process(msg_state_t *ms, const char *msg);

#endif /* ALINK_MESSAGE_H */
