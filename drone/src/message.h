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

#include <stdint.h>
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

    bool gs_connected;

    /* Inter-arrival jitter measurement (no clock sync required) */
    bool jitter_first_sample;     /* true until first delta pair established */
    uint64_t prev_gs_ts_ms;       /* Last message's GS send timestamp */
    uint64_t prev_drone_ts_ms;    /* Last message's drone receive timestamp */
    uint32_t last_jitter_ms;      /* Last measured jitter */
    uint32_t avg_jitter_ms;       /* Rolling average (EMA) */
} msg_state_t;

void msg_init(msg_state_t *ms, profile_state_t *ps, keyframe_state_t *ks,
              osd_state_t *osd, alink_config_t *cfg,
              pthread_mutex_t *pause_mutex, volatile bool *paused,
              const cmd_ctx_t *cmd);

void msg_process(msg_state_t *ms, const char *msg);

int msg_handle_hello(const char *payload, size_t payload_len,
                     const hw_state_t *hw, int sockfd,
                     const struct sockaddr_in *client_addr);

#endif /* ALINK_MESSAGE_H */
