/**
 * @file message.h
 * @brief UDP heartbeat message parsing and processing.
 *
 * Parses incoming colon-delimited UDP messages from the ground station,
 * extracts link quality metrics, triggers profile selection, handles
 * time synchronization, and manages dynamic FEC adjustments.
 */
#ifndef ALINK_MESSAGE_H
#define ALINK_MESSAGE_H

#include "alink_types.h"
#include "config.h"
#include "profile.h"
#include "keyframe.h"
#include "osd.h"
#include "rssi_monitor.h"

typedef struct {
    profile_state_t *ps;
    keyframe_state_t *ks;
    osd_state_t *osd;
    alink_config_t *cfg;
    rssi_state_t *rs;
    pthread_mutex_t *pause_mutex;
    volatile bool *paused;

    /* State that was previously global or static-local */
    bool time_synced;
    int num_antennas;
    int noise_pnlty;
    int fec_change;
    int prev_fec_change;
    struct timeval last_fec_call_time;
    int first_time;
} msg_state_t;

void msg_init(msg_state_t *ms, profile_state_t *ps, keyframe_state_t *ks,
              osd_state_t *osd, alink_config_t *cfg, rssi_state_t *rs,
              pthread_mutex_t *pause_mutex, volatile bool *paused);

void msg_process(msg_state_t *ms, const char *msg);

#endif /* ALINK_MESSAGE_H */
