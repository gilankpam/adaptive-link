/**
 * @file profile.c
 * @brief Profile lookup, selection with hysteresis/smoothing, and application.
 */
#include "profile.h"
#include "osd.h"
#include "util.h"

void profile_init(profile_state_t *ps, alink_config_t *cfg,
                  hw_state_t *hw, cmd_ctx_t *cmd) {
    memset(ps, 0, sizeof(*ps));
    ps->selectedProfile = NULL;
    ps->currentProfile = -1;
    ps->previousProfile = -2;
    ps->prevTimeStamp = 0;
    ps->smoothed_combined_value = 1500;
    ps->last_value_sent = 100;
    ps->selection_busy = false;

    ps->prevWfbPower = -1;
    ps->prevSetGop = -1.0f;
    ps->prevBandwidth = -20;
    strncpy(ps->prevSetGI, "-1", sizeof(ps->prevSetGI));
    ps->prevSetMCS = -1;
    strncpy(ps->prevROIqp, "-1", sizeof(ps->prevROIqp));
    ps->prevSetFecK = -1;
    ps->prevSetFecN = -1;
    ps->prevSetBitrate = -1;
    ps->prevDivideFpsBy = -1;
    ps->prevFPS = -1;
    ps->prevQpDelta = -100;

    ps->old_bitrate = -1;
    ps->old_fec_k = -1;
    ps->old_fec_n = -1;
    ps->bitrate_reduced = false;

    ps->noise_pnlty = 0;
    ps->fec_change = 0;

    ps->cfg = cfg;
    ps->hw = hw;
    ps->cmd = cmd;
}

static Profile *profile_lookup(const alink_config_t *cfg, int input_value) {
    for (int i = 0; i < MAX_PROFILES; i++) {
        if (input_value >= cfg->profiles[i].rangeMin && input_value <= cfg->profiles[i].rangeMax) {
            return (Profile *)&cfg->profiles[i];
        }
    }
    return NULL;
}

void profile_apply_fec_bitrate(profile_state_t *ps, int new_fec_k, int new_fec_n, int new_bitrate) {
    char fecCommand[MAX_COMMAND_SIZE];
    char bitrateCommand[MAX_COMMAND_SIZE];
    alink_config_t *cfg = ps->cfg;

    if (cfg->allow_dynamic_fec && ps->fec_change > 0 && ps->fec_change <= 5) {
        if ((cfg->spike_fix_dynamic_fec && new_bitrate >= 4000) || (!cfg->spike_fix_dynamic_fec)) {
            float denominators[] = { 1, 1.11111f, 1.25f, 1.42f, 1.66667f, 2.0f };
            float denominator = denominators[ps->fec_change];
            new_bitrate = (int)(new_bitrate / denominator);
            (cfg->fec_k_adjust) ? (new_fec_k = (int)(new_fec_k / denominator)) : (new_fec_n = (int)(new_fec_n * denominator));
        }
    }

    /* Update the OSD regardless of order — we access osd_state_t indirectly
       but the profile_fec field is always written via the same pattern */
    /* Note: OSD fec string is updated here, matching original code */

    if (new_bitrate > ps->old_bitrate) {
        const char *fecKeys[] = { "fecK", "fecN" };
        char strFecK[10], strFecN[10];
        snprintf(strFecK, sizeof(strFecK), "%d", new_fec_k);
        snprintf(strFecN, sizeof(strFecN), "%d", new_fec_n);
        const char *fecValues[] = { strFecK, strFecN };
        cmd_format(fecCommand, sizeof(fecCommand), cfg->fecCommandTemplate, 2, fecKeys, fecValues);
        cmd_exec(ps->cmd, fecCommand);
        ps->old_fec_k = new_fec_k;
        ps->old_fec_n = new_fec_n;

        const char *brKeys[] = { "bitrate" };
        char strBitrate[12];
        snprintf(strBitrate, sizeof(strBitrate), "%d", new_bitrate);
        const char *brValues[] = { strBitrate };
        cmd_format(bitrateCommand, sizeof(bitrateCommand), cfg->bitrateCommandTemplate, 1, brKeys, brValues);
        cmd_exec(ps->cmd, bitrateCommand);
        ps->old_bitrate = new_bitrate;
    } else {
        const char *brKeys[] = { "bitrate" };
        char strBitrate[12];
        snprintf(strBitrate, sizeof(strBitrate), "%d", new_bitrate);
        const char *brValues[] = { strBitrate };
        cmd_format(bitrateCommand, sizeof(bitrateCommand), cfg->bitrateCommandTemplate, 1, brKeys, brValues);
        cmd_exec(ps->cmd, bitrateCommand);
        ps->old_bitrate = new_bitrate;

        const char *fecKeys[] = { "fecK", "fecN" };
        char strFecK[10], strFecN[10];
        snprintf(strFecK, sizeof(strFecK), "%d", new_fec_k);
        snprintf(strFecN, sizeof(strFecN), "%d", new_fec_n);
        const char *fecValues[] = { strFecK, strFecN };
        cmd_format(fecCommand, sizeof(fecCommand), cfg->fecCommandTemplate, 2, fecKeys, fecValues);
        cmd_exec(ps->cmd, fecCommand);
        ps->old_fec_k = new_fec_k;
        ps->old_fec_n = new_fec_n;
    }
}

void profile_apply(profile_state_t *ps, Profile *profile, void *osd_ptr) {
    osd_state_t *os = (osd_state_t *)osd_ptr;
    alink_config_t *cfg = ps->cfg;
    hw_state_t *hw = ps->hw;

    char powerCommand[MAX_COMMAND_SIZE];
    char fpsCommand[MAX_COMMAND_SIZE];
    char qpDeltaCommand[MAX_COMMAND_SIZE];
    char mcsCommand[MAX_COMMAND_SIZE];
    char gopCommand[MAX_COMMAND_SIZE];
    char roiCommand[MAX_COMMAND_SIZE];
    const char *idrCommand = cfg->idrCommandTemplate;

    long currentTime = util_get_monotonic_time();
    long timeElapsed = currentTime - ps->prevTimeStamp;

    int currentWfbPower = profile->wfbPower;
    float currentSetGop = profile->setGop;
    char currentSetGI[10];
    strcpy(currentSetGI, profile->setGI);
    int currentSetMCS = profile->setMCS;
    int currentSetFecK = profile->setFecK;
    int currentSetFecN = profile->setFecN;
    int currentSetBitrate = profile->setBitrate;
    char currentROIqp[20];
    strcpy(currentROIqp, profile->ROIqp);
    int currentBandwidth = profile->bandwidth;
    int currentQpDelta = profile->setQpDelta;

    int currentFPS = hw->global_fps;
    int finalPower;

    if (cfg->limitFPS && currentSetBitrate < 4000 && hw->global_fps > 30 && hw->total_pixels > HIGH_RES_PIXEL_THRESHOLD) {
        currentFPS = 30;
    } else if (cfg->limitFPS && currentSetBitrate < 8000 && hw->total_pixels > HIGH_RES_PIXEL_THRESHOLD && hw->global_fps > 60) {
        currentFPS = 60;
    }

    /* --- qpDeltaCommand --- */
    {
        const char *keys[] = { "qpDelta" };
        char strQpDelta[10];
        snprintf(strQpDelta, sizeof(strQpDelta), "%d", currentQpDelta);
        const char *values[] = { strQpDelta };
        cmd_format(qpDeltaCommand, sizeof(qpDeltaCommand), cfg->qpDeltaCommandTemplate, 1, keys, values);
    }
    /* --- fpsCommand --- */
    {
        const char *keys[] = { "fps" };
        char strFPS[10];
        snprintf(strFPS, sizeof(strFPS), "%d", currentFPS);
        const char *values[] = { strFPS };
        cmd_format(fpsCommand, sizeof(fpsCommand), cfg->fpsCommandTemplate, 1, keys, values);
    }
    /* --- powerCommand --- */
    {
        const char *keys[] = { "power" };
        char strPower[10];

        if (cfg->use_0_to_4_txpower) {
            finalPower = hw->tx_power_table[currentSetMCS][cfg->power_level_0_to_4];
        } else {
            finalPower = currentWfbPower * hw->tx_factor;
        }

        snprintf(strPower, sizeof(strPower), "%d", finalPower);
        const char *values[] = { strPower };
        cmd_format(powerCommand, sizeof(powerCommand), cfg->powerCommandTemplate, 1, keys, values);
    }
    /* --- gopCommand --- */
    {
        const char *keys[] = { "gop" };
        char strGop[10];
        snprintf(strGop, sizeof(strGop), "%.1f", currentSetGop);
        const char *values[] = { strGop };
        cmd_format(gopCommand, sizeof(gopCommand), cfg->gopCommandTemplate, 1, keys, values);
    }
    /* --- mcsCommand --- */
    {
        const char *keys[] = { "bandwidth", "gi", "stbc", "ldpc", "mcs" };
        char strBandwidth[10], strGI[10], strStbc[10], strLdpc[10], strMcs[10];
        snprintf(strBandwidth, sizeof(strBandwidth), "%d", currentBandwidth);
        snprintf(strGI, sizeof(strGI), "%s", currentSetGI);
        snprintf(strStbc, sizeof(strStbc), "%d", hw->stbc);
        snprintf(strLdpc, sizeof(strLdpc), "%d", hw->ldpc_tx);
        snprintf(strMcs, sizeof(strMcs), "%d", currentSetMCS);
        const char *values[] = { strBandwidth, strGI, strStbc, strLdpc, strMcs };
        cmd_format(mcsCommand, sizeof(mcsCommand), cfg->mcsCommandTemplate, 5, keys, values);
    }
    /* --- roiCommand --- */
    {
        const char *keys[] = { "roiQp" };
        const char *values[] = { currentROIqp };
        cmd_format(roiCommand, sizeof(roiCommand), cfg->roiCommandTemplate, 1, keys, values);
    }

    /* --- Execution Logic --- */
    if (ps->currentProfile > ps->previousProfile) {
        if (currentQpDelta != ps->prevQpDelta) {
            cmd_exec(ps->cmd, qpDeltaCommand);
            ps->prevQpDelta = currentQpDelta;
        }
        if (currentFPS != ps->prevFPS) {
            cmd_exec(ps->cmd, fpsCommand);
            ps->prevFPS = currentFPS;
        }
        if (cfg->allow_set_power && finalPower != ps->prevWfbPower) {
            cmd_exec(ps->cmd, powerCommand);
            ps->prevWfbPower = finalPower;
        }
        if (currentSetGop != ps->prevSetGop) {
            cmd_exec(ps->cmd, gopCommand);
            ps->prevSetGop = currentSetGop;
        }
        if (strcmp(currentSetGI, ps->prevSetGI) != 0 ||
            currentSetMCS != ps->prevSetMCS ||
            currentBandwidth != ps->prevBandwidth) {
            cmd_exec(ps->cmd, mcsCommand);
            ps->prevBandwidth = currentBandwidth;
            strcpy(ps->prevSetGI, currentSetGI);
            ps->prevSetMCS = currentSetMCS;
        }

        if (currentSetFecK != ps->prevSetFecK || currentSetFecN != ps->prevSetFecN || currentSetBitrate != ps->prevSetBitrate) {
            profile_apply_fec_bitrate(ps, currentSetFecK, currentSetFecN, currentSetBitrate);
            ps->prevSetBitrate = currentSetBitrate;
            ps->prevSetFecK = currentSetFecK;
            ps->prevSetFecN = currentSetFecN;
        }

        if (cfg->roi_focus_mode && strcmp(currentROIqp, ps->prevROIqp) != 0) {
            cmd_exec(ps->cmd, roiCommand);
            strcpy(ps->prevROIqp, currentROIqp);
        }
        if (cfg->idr_every_change) {
            cmd_exec(ps->cmd, idrCommand);
        }
    } else {
        if (currentQpDelta != ps->prevQpDelta) {
            cmd_exec(ps->cmd, qpDeltaCommand);
            ps->prevQpDelta = currentQpDelta;
        }
        if (currentFPS != ps->prevFPS) {
            cmd_exec(ps->cmd, fpsCommand);
            ps->prevFPS = currentFPS;
        }

        if (currentSetFecK != ps->prevSetFecK || currentSetFecN != ps->prevSetFecN || currentSetBitrate != ps->prevSetBitrate) {
            profile_apply_fec_bitrate(ps, currentSetFecK, currentSetFecN, currentSetBitrate);
            ps->prevSetBitrate = currentSetBitrate;
            ps->prevSetFecK = currentSetFecK;
            ps->prevSetFecN = currentSetFecN;
        }

        if (currentSetGop != ps->prevSetGop) {
            cmd_exec(ps->cmd, gopCommand);
            ps->prevSetGop = currentSetGop;
        }
        if (strcmp(currentSetGI, ps->prevSetGI) != 0 ||
            currentSetMCS != ps->prevSetMCS ||
            currentBandwidth != ps->prevBandwidth) {
            cmd_exec(ps->cmd, mcsCommand);
            ps->prevBandwidth = currentBandwidth;
            strcpy(ps->prevSetGI, currentSetGI);
            ps->prevSetMCS = currentSetMCS;
        }
        if (cfg->allow_set_power && finalPower != ps->prevWfbPower) {
            cmd_exec(ps->cmd, powerCommand);
            ps->prevWfbPower = finalPower;
        }
        if (cfg->roi_focus_mode && strcmp(currentROIqp, ps->prevROIqp) != 0) {
            cmd_exec(ps->cmd, roiCommand);
            strcpy(ps->prevROIqp, currentROIqp);
        }
        if (cfg->idr_every_change) {
            cmd_exec(ps->cmd, idrCommand);
        }
    }

    /* Update OSD with actual values from wfb_tx_cmd output */
    int k, n, stbc_val, ldpc_val, short_gi, actual_bandwidth, mcs_index, vht_mode, vht_nss;
    hw_read_wfb_status(&k, &n, &stbc_val, &ldpc_val, &short_gi, &actual_bandwidth, &mcs_index, &vht_mode, &vht_nss);
    const char *gi_string = short_gi ? "short" : "long";
    int pwr = cfg->allow_set_power ? finalPower : 0;

    sprintf(os->profile, "%lds %d %d%s%d Pw(%d)%d g%.1f",
            timeElapsed,
            profile->setBitrate,
            actual_bandwidth,
            gi_string,
            mcs_index,
            cfg->power_level_0_to_4,
            pwr,
            profile->setGop);

    snprintf(os->profile_fec, sizeof(os->profile_fec), "%d/%d", k, n);
}

static bool value_chooses_profile(profile_state_t *ps, int input_value, void *osd_ptr) {
    alink_config_t *cfg = ps->cfg;

    ps->selectedProfile = profile_lookup(cfg, input_value);
    if (ps->selectedProfile == NULL) {
        printf("No matching profile found for input: %d\n", input_value);
        return false;
    }

    for (int i = 0; i < MAX_PROFILES; i++) {
        if (ps->selectedProfile == &cfg->profiles[i]) {
            ps->currentProfile = i;
            break;
        }
    }

    if (ps->previousProfile == ps->currentProfile) {
        if (cfg->verbose_mode) {
            printf("No change: Link value is within same profile.\n");
        }
        return false;
    }

    long currentTime = util_get_monotonic_time();
    long timeElapsed = currentTime - ps->prevTimeStamp;

    if (ps->previousProfile == 0) {
        if (timeElapsed <= cfg->hold_fallback_mode_s) {
            if (cfg->verbose_mode) {
                puts("Holding fallback...");
            }
            return false;
        }
    }
    else if (ps->previousProfile < ps->currentProfile && timeElapsed <= cfg->hold_modes_down_s) {
        if (cfg->verbose_mode) {
            puts("Too soon to increase link...");
        }
        return false;
    }

    profile_apply(ps, ps->selectedProfile, osd_ptr);
    ps->previousProfile = ps->currentProfile;
    ps->prevTimeStamp = currentTime;
    return true;
}

void profile_start_selection(profile_state_t *ps, int rssi_score,
                             int snr_score, int recovered,
                             void *osd_ptr) {
    osd_state_t *os = (osd_state_t *)osd_ptr;
    alink_config_t *cfg = ps->cfg;
    (void)recovered;

    struct timespec current_time;
    clock_gettime(CLOCK_MONOTONIC, &current_time);

    if (rssi_score == FALLBACK_SCORE) {
        if (value_chooses_profile(ps, FALLBACK_SCORE, osd_ptr)) {
            printf("Applied.\n");
            ps->last_value_sent = FALLBACK_SCORE;
            ps->smoothed_combined_value = FALLBACK_SCORE;
            ps->last_exec_time = current_time;
        } else {
            printf("Not applied.\n");
        }
        return;
    }

    if (ps->selection_busy) {
        if (cfg->verbose_mode) {
            puts("Selection process busy...");
        }
        return;
    }
    ps->selection_busy = true;

    float combined_value_float = rssi_score * cfg->rssi_weight + snr_score * cfg->snr_weight;
    int osd_raw_score = (int)combined_value_float;

    if (cfg->limit_max_score_to < SCORE_RANGE_MAX && cfg->limit_max_score_to < combined_value_float) {
        combined_value_float = (float)cfg->limit_max_score_to;
    }

    float chosen_smoothing_factor = (combined_value_float >= ps->last_value_sent) ? cfg->smoothing_factor : cfg->smoothing_factor_down;

    ps->smoothed_combined_value = (chosen_smoothing_factor * combined_value_float + (1 - chosen_smoothing_factor) * ps->smoothed_combined_value);

    int osd_smoothed_score = (int)ps->smoothed_combined_value;
    sprintf(os->score_related, "linkQ %d, smthdQ %d", osd_raw_score, osd_smoothed_score);

    long time_diff_ms = util_elapsed_ms_timespec(&current_time, &ps->last_exec_time);
    if (time_diff_ms < cfg->min_between_changes_ms) {
        printf("Skipping profile load: time_diff_ms=%ldms - too soon (min %dms required)\n", time_diff_ms, cfg->min_between_changes_ms);
        ps->selection_busy = false;
        return;
    }

    int combined_value = (int)floor(ps->smoothed_combined_value);
    combined_value = (combined_value < SCORE_RANGE_BASE) ? SCORE_RANGE_BASE : (combined_value > SCORE_RANGE_MAX) ? SCORE_RANGE_MAX : combined_value;

    float percent_change = fabs((float)(combined_value - ps->last_value_sent) / ps->last_value_sent) * 100;

    float hysteresis_threshold = (combined_value >= ps->last_value_sent) ? cfg->hysteresis_percent : cfg->hysteresis_percent_down;

    if (percent_change >= hysteresis_threshold) {
        if (cfg->verbose_mode) {
            printf("Qualified to request profile: %d is > %.2f%% different (%.2f%%)\n", combined_value, hysteresis_threshold, percent_change);
        }
        if (value_chooses_profile(ps, combined_value, osd_ptr)) {
            printf("Profile %d applied.\n", combined_value);
            ps->last_value_sent = combined_value;
            ps->last_exec_time = current_time;
        }
    }
    ps->selection_busy = false;
}

Profile *profile_get_selected(const profile_state_t *ps) {
    return ps->selectedProfile;
}
