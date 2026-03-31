/**
 * @file profile.c
 * @brief Profile application and async worker thread.
 *
 * The GS selects profiles; the drone just applies them. This module
 * handles command execution ordering (upgrade vs downgrade) and
 * delta detection to avoid redundant commands.
 */
#include "profile.h"
#include "osd.h"
#include "util.h"

void profile_init(profile_state_t *ps, alink_config_t *cfg,
                  hw_state_t *hw, cmd_ctx_t *cmd) {
    memset(ps, 0, sizeof(*ps));
    ps->currentProfile = -1;
    ps->previousProfile = -2;
    ps->prevTimeStamp = 0;
    ps->hasAppliedProfile = false;

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

    ps->worker_mutex = (pthread_mutex_t)PTHREAD_MUTEX_INITIALIZER;
    ps->worker_cond = (pthread_cond_t)PTHREAD_COND_INITIALIZER;
    ps->job_pending = false;
    ps->worker_stop = false;

    ps->cfg = cfg;
    ps->hw = hw;
    ps->cmd = cmd;
}

void profile_apply_fec_bitrate(profile_state_t *ps, int new_fec_k, int new_fec_n, int new_bitrate) {
    char fecCommand[MAX_COMMAND_SIZE];
    char bitrateCommand[MAX_COMMAND_SIZE];
    alink_config_t *cfg = ps->cfg;

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

/*
 * Internal: execute profile commands on the worker thread.
 */
static void profile_apply_exec(profile_state_t *ps, const profile_job_t *job) {
    const Profile *profile = &job->profile;
    osd_state_t *os = (osd_state_t *)job->osd;
    alink_config_t *cfg = ps->cfg;
    hw_state_t *hw = ps->hw;

    char powerCommand[MAX_COMMAND_SIZE];
    char fpsCommand[MAX_COMMAND_SIZE];
    char qpDeltaCommand[MAX_COMMAND_SIZE];
    char mcsCommand[MAX_COMMAND_SIZE];
    char gopCommand[MAX_COMMAND_SIZE];
    char roiCommand[MAX_COMMAND_SIZE];
    const char *idrCommand = cfg->idrCommandTemplate;

    uint64_t now = util_now_ms();
    long timeElapsed = (long)((now - ps->prevTimeStamp) / 1000);

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

    /* --- Build commands --- */
    {
        const char *keys[] = { "qpDelta" };
        char strQpDelta[10];
        snprintf(strQpDelta, sizeof(strQpDelta), "%d", currentQpDelta);
        const char *values[] = { strQpDelta };
        cmd_format(qpDeltaCommand, sizeof(qpDeltaCommand), cfg->qpDeltaCommandTemplate, 1, keys, values);
    }
    {
        const char *keys[] = { "fps" };
        char strFPS[10];
        snprintf(strFPS, sizeof(strFPS), "%d", currentFPS);
        const char *values[] = { strFPS };
        cmd_format(fpsCommand, sizeof(fpsCommand), cfg->fpsCommandTemplate, 1, keys, values);
    }
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
    {
        const char *keys[] = { "gop" };
        char strGop[10];
        snprintf(strGop, sizeof(strGop), "%.1f", currentSetGop);
        const char *values[] = { strGop };
        cmd_format(gopCommand, sizeof(gopCommand), cfg->gopCommandTemplate, 1, keys, values);
    }
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
    {
        const char *keys[] = { "roiQp" };
        const char *values[] = { currentROIqp };
        cmd_format(roiCommand, sizeof(roiCommand), cfg->roiCommandTemplate, 1, keys, values);
    }

    /* --- Execute commands (ordering depends on direction) --- */
    if (job->currentProfile > job->previousProfile) {
        /* Upgrading: power before MCS */
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
        /* Downgrading: MCS/FEC before power */
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

/*
 * Dispatch profile application to the async worker thread.
 * Returns immediately — the worker executes commands in the background.
 */
void profile_apply(profile_state_t *ps, Profile *profile, void *osd_ptr) {
    pthread_mutex_lock(&ps->worker_mutex);
    ps->pending_job.profile = *profile;
    ps->pending_job.currentProfile = ps->currentProfile;
    ps->pending_job.previousProfile = ps->previousProfile;
    ps->pending_job.osd = osd_ptr;
    ps->job_pending = true;
    pthread_cond_signal(&ps->worker_cond);
    pthread_mutex_unlock(&ps->worker_mutex);
}

/**
 * Apply a profile directly from GS parameters.
 * Updates tracking state and dispatches to async worker.
 */
void profile_apply_direct(profile_state_t *ps, const Profile *profile,
                          int profile_index, void *osd_ptr) {
    /* Store latest profile for re-application (cmd_server power changes) */
    ps->lastAppliedProfile = *profile;
    ps->hasAppliedProfile = true;

    /* Skip dispatch if the same profile is already active — the GS sends
     * the current profile on every tick for UDP reliability, so most
     * messages are duplicates. */
    if (profile_index == ps->currentProfile)
        return;

    ps->previousProfile = ps->currentProfile;
    ps->currentProfile = profile_index;
    ps->prevTimeStamp = util_now_ms();

    profile_apply(ps, (Profile *)profile, osd_ptr);
}

/*
 * Async worker thread: waits for profile jobs and executes them.
 * Latest job wins — if a new job arrives while executing, it is
 * picked up immediately after the current one finishes.
 */
void *profile_worker_func(void *arg) {
    profile_state_t *ps = (profile_state_t *)arg;

    while (1) {
        pthread_mutex_lock(&ps->worker_mutex);
        while (!ps->job_pending && !ps->worker_stop)
            pthread_cond_wait(&ps->worker_cond, &ps->worker_mutex);

        if (ps->worker_stop) {
            pthread_mutex_unlock(&ps->worker_mutex);
            break;
        }

        profile_job_t job = ps->pending_job;
        ps->job_pending = false;
        pthread_mutex_unlock(&ps->worker_mutex);

        profile_apply_exec(ps, &job);
    }
    return NULL;
}
