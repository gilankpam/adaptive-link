/**
 * @file profile.c
 * @brief Profile application and async worker thread.
 *
 * The GS selects profiles; the drone just applies them. This module
 * handles command execution ordering (upgrade vs downgrade) and
 * delta detection to avoid redundant commands.
 * 
 * Optimized to use native HTTP client for API calls (no curl) and
 * fork/exec for shell commands (faster than system()).
 */
#include "profile.h"
#include "osd.h"
#include "util.h"
#include <string.h>

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
    ps->prevSetFecK = -1;
    ps->prevSetFecN = -1;
    ps->prevSetBitrate = -1;
    ps->prevDivideFpsBy = -1;
    ps->prevFPS = -1;

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

int profile_apply_fec(profile_state_t *ps, int new_fec_k, int new_fec_n) {
    char fecCommand[MAX_COMMAND_SIZE];
    alink_config_t *cfg = ps->cfg;

    const char *fecKeys[] = { "fecK", "fecN" };
    char strFecK[10], strFecN[10];
    snprintf(strFecK, sizeof(strFecK), "%d", new_fec_k);
    snprintf(strFecN, sizeof(strFecN), "%d", new_fec_n);
    const char *fecValues[] = { strFecK, strFecN };
    cmd_format(fecCommand, sizeof(fecCommand), cfg->fecCommandTemplate, 2, fecKeys, fecValues);
    int result = cmd_exec_with_timeout(ps->cmd, fecCommand);
    if (result != 0) {
        ERROR_LOG(ps->cfg, "FEC command failed: %s\n", fecCommand);
    }
    ps->old_fec_k = new_fec_k;
    ps->old_fec_n = new_fec_n;
    return result;
}

/**
 * Compute ROI QP from bitrate (kbps).
 * Linearly scales roiqp_base between hi and lo thresholds.
 */
int calc_roiQp_from_bitrate(int kbps, int hi, int lo, int roiqp_base) {
    const int span = hi - lo;

    if (kbps > hi) return 0;
    if (roiqp_base == 0) return 0;
    if (kbps <= lo) return roiqp_base;

    /* pct = 25 .. 100 linearly as kbps falls from hi -> lo */
    int x = hi - kbps;                             /* 0 .. span-1 */
    int pct = 25 + (x * 75 + (span / 2)) / span;   /* rounded */

    long v = (long)roiqp_base * (long)pct;
    if (v >= 0) v = (v + 50) / 100;
    else        v = (v - 50) / 100;  /* round negatives correctly */
    return (int)v;
}

/**
 * Build and execute a batched API command using native HTTP client.
 * Combines bitrate, gop, and drone-computed roiQp into a single HTTP request.
 *
 * @param cfg Configuration with apiCommandTemplate and roiQp params
 * @param bitrate Bitrate value (kbps)
 * @param gop GOP size
 * @param cmd Command context for execution
 * @return 0 on success, non-zero on failure
 */
int profile_apply_api_batch(const alink_config_t *cfg,
                                   int bitrate, float gop,
                                   const cmd_ctx_t *cmd) {
    char path[MAX_COMMAND_SIZE];
    char bitrateStr[16], gopStr[16], roiQpStr[32];

    int roiQp = calc_roiQp_from_bitrate(bitrate, cfg->roiqp_hi, cfg->roiqp_lo, cfg->roiqp_base);

    snprintf(bitrateStr, sizeof(bitrateStr), "%d", bitrate);
    snprintf(gopStr, sizeof(gopStr), "%.1f", gop);
    snprintf(roiQpStr, sizeof(roiQpStr), "0,%d,0", roiQp);

    const char *keys[] = { "bitrate", "gop", "roiQp" };
    const char *values[] = { bitrateStr, gopStr, roiQpStr };

    cmd_format(path, sizeof(path), cfg->apiCommandTemplate, 3, keys, values);

    /* Parse the URL to extract host, port, and path */
    char host[64];
    int port = 80;
    char url_path[MAX_COMMAND_SIZE];

    if (util_parse_url(path, host, sizeof(host), &port, url_path, sizeof(url_path)) != 0) {
        return -1;
    }

    return cmd_http_get(host, port, url_path, NULL, 0, cmd);
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
    char mcsCommand[MAX_COMMAND_SIZE];
    const char *idrApiCommand = cfg->idrApiCommandTemplate;

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
    int currentBandwidth = profile->bandwidth;

    int currentFPS = hw->global_fps;
    int finalPower;

    if (cfg->limitFPS && currentSetBitrate < 4000 && hw->global_fps > 30 && hw->total_pixels > HIGH_RES_PIXEL_THRESHOLD) {
        currentFPS = 30;
    } else if (cfg->limitFPS && currentSetBitrate < 8000 && hw->total_pixels > HIGH_RES_PIXEL_THRESHOLD && hw->global_fps > 60) {
        currentFPS = 60;
    }

    /* --- Build commands --- */
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

        finalPower = currentWfbPower;

        snprintf(strPower, sizeof(strPower), "%d", finalPower);
        const char *values[] = { strPower };
        cmd_format(powerCommand, sizeof(powerCommand), cfg->powerCommandTemplate, 1, keys, values);
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

    /* --- Execute commands (ordering depends on direction) --- */
    if (job->currentProfile > job->previousProfile) {
        /* Upgrading: power before MCS */
        if (currentFPS != ps->prevFPS) {
            DEBUG_LOG(cfg, "FPS: %d -> %d\n", ps->prevFPS, currentFPS);
            if (cmd_exec_with_timeout(ps->cmd, fpsCommand) != 0) {
                ERROR_LOG(ps->cfg, "FPS command failed: %s\n", fpsCommand);
            }
            ps->prevFPS = currentFPS;
        }
        if (cfg->allow_set_power && finalPower != ps->prevWfbPower) {
            DEBUG_LOG(cfg, "Power: %d -> %d\n", ps->prevWfbPower, finalPower);
            if (cmd_exec_with_timeout(ps->cmd, powerCommand) != 0) {
                ERROR_LOG(ps->cfg, "Power command failed: %s\n", powerCommand);
            }
            ps->prevWfbPower = finalPower;
        }
        if (strcmp(currentSetGI, ps->prevSetGI) != 0 ||
            currentSetMCS != ps->prevSetMCS ||
            currentBandwidth != ps->prevBandwidth) {
            DEBUG_LOG(cfg, "MCS: %d -> %d, GI: %s -> %s, BW: %d -> %d\n",
                       ps->prevSetMCS, currentSetMCS, ps->prevSetGI, currentSetGI,
                       ps->prevBandwidth, currentBandwidth);
            if (cmd_exec_with_timeout(ps->cmd, mcsCommand) != 0) {
                ERROR_LOG(ps->cfg, "MCS command failed: %s\n", mcsCommand);
            }
            ps->prevBandwidth = currentBandwidth;
            strcpy(ps->prevSetGI, currentSetGI);
            ps->prevSetMCS = currentSetMCS;
        }
        /* Apply FEC changes (uses wfb_tx_cmd) */
        if (currentSetFecK != ps->prevSetFecK || currentSetFecN != ps->prevSetFecN) {
            DEBUG_LOG(cfg, "FEC: %d/%d -> %d/%d\n",
                       ps->prevSetFecK, ps->prevSetFecN, currentSetFecK, currentSetFecN);
            if (profile_apply_fec(ps, currentSetFecK, currentSetFecN) != 0) {
                ERROR_LOG(ps->cfg, "Failed to apply FEC settings\n");
            }
            ps->prevSetFecK = currentSetFecK;
            ps->prevSetFecN = currentSetFecN;
        }
        /* Batch API call (bitrate, gop, drone-computed roiQp) */
        if (currentSetBitrate != ps->prevSetBitrate ||
            currentSetGop != ps->prevSetGop) {
            DEBUG_LOG(cfg, "Bitrate: %d -> %d, GOP: %.1f -> %.1f\n",
                       ps->prevSetBitrate, currentSetBitrate, ps->prevSetGop, currentSetGop);
            if (profile_apply_api_batch(cfg, currentSetBitrate, currentSetGop, ps->cmd) == 0) {
                ps->prevSetBitrate = currentSetBitrate;
                ps->prevSetGop = currentSetGop;
            } else {
                ERROR_LOG(ps->cfg, "API batch command failed\n");
            }
        }
        if (cfg->idr_every_change) {
            /* Parse the IDR API URL and use native HTTP client */
            char host[64];
            int port = 80;
            char url_path[MAX_COMMAND_SIZE];
            
            if (util_parse_url(idrApiCommand, host, sizeof(host), &port, url_path, sizeof(url_path)) == 0) {
                if (cmd_http_get(host, port, url_path, NULL, 0, ps->cmd) != 0) {
                    ERROR_LOG(ps->cfg, "IDR command failed: %s\n", idrApiCommand);
                }
            }
        }
    } else {
        /* Downgrading: MCS/FEC before power */
        if (currentFPS != ps->prevFPS) {
            DEBUG_LOG(cfg, "FPS: %d -> %d\n", ps->prevFPS, currentFPS);
            if (cmd_exec_with_timeout(ps->cmd, fpsCommand) != 0) {
                ERROR_LOG(ps->cfg, "FPS command failed: %s\n", fpsCommand);
            }
            ps->prevFPS = currentFPS;
        }
        /* Apply FEC changes (uses wfb_tx_cmd) */
        if (currentSetFecK != ps->prevSetFecK || currentSetFecN != ps->prevSetFecN) {
            DEBUG_LOG(cfg, "FEC: %d/%d -> %d/%d\n",
                       ps->prevSetFecK, ps->prevSetFecN, currentSetFecK, currentSetFecN);
            if (profile_apply_fec(ps, currentSetFecK, currentSetFecN) != 0) {
                ERROR_LOG(ps->cfg, "Failed to apply FEC settings\n");
            }
            ps->prevSetFecK = currentSetFecK;
            ps->prevSetFecN = currentSetFecN;
        }
        /* Batch API call (bitrate, gop, drone-computed roiQp) */
        if (currentSetBitrate != ps->prevSetBitrate ||
            currentSetGop != ps->prevSetGop) {
            DEBUG_LOG(cfg, "Bitrate: %d -> %d, GOP: %.1f -> %.1f\n",
                       ps->prevSetBitrate, currentSetBitrate, ps->prevSetGop, currentSetGop);
            if (profile_apply_api_batch(cfg, currentSetBitrate, currentSetGop, ps->cmd) == 0) {
                ps->prevSetBitrate = currentSetBitrate;
                ps->prevSetGop = currentSetGop;
            } else {
                ERROR_LOG(ps->cfg, "API batch command failed\n");
            }
        }
        if (strcmp(currentSetGI, ps->prevSetGI) != 0 ||
            currentSetMCS != ps->prevSetMCS ||
            currentBandwidth != ps->prevBandwidth) {
            DEBUG_LOG(cfg, "MCS: %d -> %d, GI: %s -> %s, BW: %d -> %d\n",
                       ps->prevSetMCS, currentSetMCS, ps->prevSetGI, currentSetGI,
                       ps->prevBandwidth, currentBandwidth);
            if (cmd_exec_with_timeout(ps->cmd, mcsCommand) != 0) {
                ERROR_LOG(ps->cfg, "MCS command failed: %s\n", mcsCommand);
            }
            ps->prevBandwidth = currentBandwidth;
            strcpy(ps->prevSetGI, currentSetGI);
            ps->prevSetMCS = currentSetMCS;
        }
        if (cfg->allow_set_power && finalPower != ps->prevWfbPower) {
            DEBUG_LOG(cfg, "Power: %d -> %d\n", ps->prevWfbPower, finalPower);
            if (cmd_exec_with_timeout(ps->cmd, powerCommand) != 0) {
                ERROR_LOG(ps->cfg, "Power command failed: %s\n", powerCommand);
            }
            ps->prevWfbPower = finalPower;
        }
        if (cfg->idr_every_change) {
            /* Parse the IDR API URL and use native HTTP client */
            char host[64];
            int port = 80;
            char url_path[MAX_COMMAND_SIZE];
            
            if (util_parse_url(idrApiCommand, host, sizeof(host), &port, url_path, sizeof(url_path)) == 0) {
                if (cmd_http_get(host, port, url_path, NULL, 0, ps->cmd) != 0) {
                    ERROR_LOG(ps->cfg, "IDR command failed: %s\n", idrApiCommand);
                }
            }
        }
    }

    /* Update OSD with actual values from wfb_tx_cmd output */
    int k, n, stbc_val, ldpc_val, short_gi, actual_bandwidth, mcs_index, vht_mode, vht_nss;
    hw_read_wfb_status(&k, &n, &stbc_val, &ldpc_val, &short_gi, &actual_bandwidth, &mcs_index, &vht_mode, &vht_nss);
    const char *gi_string = short_gi ? "short" : "long";
    int pwr = cfg->allow_set_power ? finalPower : 0;

    sprintf(os->profile, "%lds %d %d%s%d Pw%d g%.1f",
            timeElapsed,
            profile->setBitrate,
            actual_bandwidth,
            gi_string,
            mcs_index,
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

    {
        const char *direction = (profile_index > ps->previousProfile) ? "upgrade" : "downgrade";
        DEBUG_LOG(ps->cfg, "Profile change: %d -> %d (%s)\n", ps->previousProfile, profile_index, direction);
    }

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