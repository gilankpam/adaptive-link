/**
 * @file profile.h
 * @brief Profile application and async worker thread.
 *
 * The drone receives finalized profile parameters from the GS and
 * applies them (direct UDP to the wfb_tx daemon for FEC/MCS; shell
 * and native HTTP for power/API/IDR). Profile application is dispatched
 * to an async worker thread so the main UDP receive loop is never
 * blocked.
 */
#ifndef ALINK_PROFILE_H
#define ALINK_PROFILE_H

#include "alink_types.h"
#include "config.h"
#include "hardware.h"
#include "command.h"
#include <stdint.h>

/* Forward declaration */
struct osd_state_t_tag;

/* Job struct for the async profile worker */
typedef struct {
    Profile profile;
    int currentProfile;
    int previousProfile;
    void *osd;              /* osd_state_t* */
} profile_job_t;

typedef struct {
    /* Current profile tracking */
    int currentProfile;
    int previousProfile;
    uint64_t prevTimeStamp;

    /* Previous-value tracking for apply_*_step delta detection. Mirrors the
     * Profile struct so adding a new profile field automatically gets a
     * "previous" slot. Protected by worker_mutex when accessed from
     * tx_monitor. */
    Profile prevApplied;

    /* Set by tx_monitor while the xtx-driven bitrate reduction is active;
     * cleared when it restores to prevApplied.setBitrate. */
    bool bitrate_reduced;

    /* Async worker */
    pthread_mutex_t worker_mutex;
    pthread_cond_t worker_cond;
    profile_job_t pending_job;
    bool job_pending;
    volatile bool worker_stop;

    /* Pointers to shared state (not owned) */
    alink_config_t *cfg;
    hw_state_t *hw;
    cmd_ctx_t *cmd;
} profile_state_t;

void profile_init(profile_state_t *ps, alink_config_t *cfg,
                  hw_state_t *hw, cmd_ctx_t *cmd);

/**
 * Apply a profile directly (GS has already selected it).
 * Dispatches to the async worker thread.
 */
void profile_apply_direct(profile_state_t *ps, const Profile *profile,
                          int profile_index, void *osd);

/**
 * Dispatch profile to the async worker thread.
 */
void profile_apply(profile_state_t *ps, Profile *profile, void *osd);

/**
 * Apply batched API call for bitrate, gop, and drone-computed roiQp.
 * Used by tx_monitor for bitrate reduction/restore.
 */
int profile_apply_api_batch(const alink_config_t *cfg,
                            int bitrate, float gop,
                            const cmd_ctx_t *cmd);

/* Async worker thread entry point */
void *profile_worker_func(void *arg);

#endif /* ALINK_PROFILE_H */
