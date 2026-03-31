/**
 * @file profile.h
 * @brief Profile lookup, selection with hysteresis/smoothing, and application.
 *
 * Manages the core adaptive link logic: matching scores to profiles,
 * applying exponential smoothing and hysteresis, enforcing time guards,
 * and executing command templates to apply profile settings.
 *
 * Thread safety: profile_apply() and profile_start_selection() are NOT
 * thread-safe. They are called from the main thread and occasionally from
 * cmd_server (for re-apply). This matches the original behavior.
 */
#ifndef ALINK_PROFILE_H
#define ALINK_PROFILE_H

#include "alink_types.h"
#include "config.h"
#include "hardware.h"
#include "command.h"

/* Forward declaration */
struct osd_state_t_tag;

typedef struct {
    /* Current selection state */
    Profile *selectedProfile;
    int currentProfile;
    int previousProfile;
    long prevTimeStamp;
    float smoothed_combined_value;
    int last_value_sent;
    bool selection_busy;

    /* Previous-value tracking for apply_profile delta detection */
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

    /* Timing */
    struct timespec last_exec_time;

    /* Pointers to shared state (not owned) */
    alink_config_t *cfg;
    hw_state_t *hw;
    cmd_ctx_t *cmd;

    /* Noise penalty for OSD (set by message processing) */
    int noise_pnlty;
    int fec_change;
} profile_state_t;

void profile_init(profile_state_t *ps, alink_config_t *cfg,
                  hw_state_t *hw, cmd_ctx_t *cmd);

void profile_start_selection(profile_state_t *ps, int rssi_score,
                             int snr_score, int recovered,
                             void *osd);  /* osd_state_t* */

void profile_apply(profile_state_t *ps, Profile *profile, void *osd);  /* osd_state_t* */

void profile_apply_fec_bitrate(profile_state_t *ps, int fec_k, int fec_n, int bitrate);

Profile *profile_get_selected(const profile_state_t *ps);

#endif /* ALINK_PROFILE_H */
