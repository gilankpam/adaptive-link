/**
 * @file profile.h
 * @brief Profile application and async worker thread.
 *
 * The drone receives finalized profile parameters from the GS and
 * applies them via command templates. Profile application is dispatched
 * to an async worker thread so the main UDP receive loop is never
 * blocked by system() calls.
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

    /* Last applied profile (for re-application via cmd_server) */
    Profile lastAppliedProfile;
    bool hasAppliedProfile;

    /* Previous-value tracking for apply_profile delta detection.
     * Protected by worker_mutex when accessed from tx_monitor. */
    int prevWfbPower;
    float prevSetGop;
    int prevBandwidth;
    char prevSetGI[10];
    int prevSetMCS;
    char prevROIqp[20];
    int prevSetFecK;
    int prevSetFecN;
    int prevSetBitrate;
    int prevDivideFpsBy;
    int prevFPS;
    int prevQpDelta;

    /* FEC/bitrate restore tracking */
    int old_bitrate;
    int old_fec_k;
    int old_fec_n;
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

void profile_apply_fec_bitrate(profile_state_t *ps, int fec_k, int fec_n, int bitrate);

/* Async worker thread entry point */
void *profile_worker_func(void *arg);

#endif /* ALINK_PROFILE_H */
