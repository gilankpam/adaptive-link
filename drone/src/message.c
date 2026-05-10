/**
 * @file message.c
 * @brief UDP message parsing and processing.
 *
 * Handles "P:" profile messages from the GS. The GS sends the current
 * profile on every tick for reliability; delta detection on the drone
 * side avoids redundant command execution.
 */
#include "message.h"
#include "util.h"
#include "command.h"
#include "hardware.h"
#include <time.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <stdio.h>

void msg_init(msg_state_t *ms, profile_state_t *ps, keyframe_state_t *ks,
              osd_state_t *osd, alink_config_t *cfg,
              const cmd_ctx_t *cmd) {
    memset(ms, 0, sizeof(*ms));
    ms->ps = ps;
    ms->ks = ks;
    ms->osd = osd;
    ms->cfg = cfg;
    ms->cmd = cmd;
    ms->jitter_first_sample = true;
    ms->prev_gs_ts_ms = 0;
    ms->prev_drone_ts_ms = 0;
    ms->last_jitter_ms = 0;
    ms->avg_jitter_ms = 0;
}

int msg_handle_hello(const char *payload, size_t payload_len,
                     hw_state_t *hw, int sockfd,
                     const struct sockaddr_in *client_addr,
                     uint32_t session_id) {
    /* Stamp t2 first thing — both t2 and t3 must be on the drone clock so
     * (t3 - t2) measures drone-side processing only. The GS subtracts that
     * from its own (gs_recv - t1) to recover the on-wire RTT, regardless of
     * how far the GS and drone wall clocks differ. */
    long t2 = util_now_ms();

    /* Copy to a NUL-terminated local buffer so strtoll works cleanly. */
    char buf[64];
    if (payload_len == 0 || payload_len >= sizeof(buf)) {
        return -1;
    }
    memcpy(buf, payload, payload_len);
    buf[payload_len] = '\0';

    /* Parse T1 from the start of the payload. Reject if no digits or zero/neg. */
    char *endptr = NULL;
    long long t1 = strtoll(buf, &endptr, 10);
    if (endptr == buf || t1 <= 0) {
        return -1;
    }

    /* Camera info (fps/x_res/y_res) is refreshed on the OSD thread, not here:
     * popen("cli …") on the recv thread was blocking profile-message intake
     * every time the cache TTL expired. Reading whatever's currently cached
     * is safe — runtime camera reconfigs are rare and stale values for one
     * TTL window are acceptable. */

    /* Stamp t3 as late as possible while still embeddable in the payload.
     * Snprintf/memcpy/frame-build cost is ~µs and falls outside (t3 - t2);
     * the GS will see it as part of the on-wire RTT, which is fine — it's
     * far below the noise floor of a wireless link. */
    long t3 = util_now_ms();

    char reply_payload[128];
    int n = snprintf(reply_payload, sizeof(reply_payload),
                     "I:%lld:%ld:%ld:%d:%d:%d:%u",
                     t1, t2, t3,
                     hw->global_fps, hw->x_res, hw->y_res,
                     session_id);
    if (n <= 0 || (size_t)n >= sizeof(reply_payload)) {
        return -1;
    }

    uint8_t frame[4 + sizeof(reply_payload)];
    uint32_t plen_be = htonl((uint32_t)n);
    memcpy(frame, &plen_be, 4);
    memcpy(frame + 4, reply_payload, (size_t)n);

    ssize_t sent = sendto(sockfd, frame, 4 + (size_t)n, 0,
                          (const struct sockaddr *)client_addr,
                          sizeof(*client_addr));
    if (sent < 0) {
        return -1;
    }

    return 0;
}

/**
 * Measure inter-arrival jitter between drone and GS without requiring
 * clock synchronization.
 *
 * jitter = |(drone_now - prev_drone) - (gs_ts - prev_gs_ts)|
 *
 * This captures link variability (stalls, bursts) using only the delta
 * between consecutive timestamps, so absolute clock offset cancels out.
 */
static void msg_update_jitter(msg_state_t *ms, uint64_t gs_send_time_ms) {
    if (gs_send_time_ms == 0) {
        return;
    }

    /* Monotonic clock: REALTIME can jump (NTP, manual set), which would
     * underflow the uint64_t delta below and poison the EMA for ~10 ticks.
     * Constant offset between drone and GS clocks cancels in the delta math
     * regardless of which clock the drone uses, so monotonic is safe. */
    uint64_t drone_time_ms = util_now_ms();

    if (ms->jitter_first_sample) {
        /* Seed the delta pair; no jitter measurement possible yet. */
        ms->prev_gs_ts_ms = gs_send_time_ms;
        ms->prev_drone_ts_ms = drone_time_ms;
        ms->jitter_first_sample = false;
        ms->osd->jitter_ms = 0;
        return;
    }

    int64_t drone_delta = (int64_t)(drone_time_ms - ms->prev_drone_ts_ms);
    int64_t gs_delta = (int64_t)(gs_send_time_ms - ms->prev_gs_ts_ms);
    int64_t diff = drone_delta - gs_delta;
    if (diff < 0) {
        diff = -diff;
    }

    ms->prev_gs_ts_ms = gs_send_time_ms;
    ms->prev_drone_ts_ms = drone_time_ms;
    ms->last_jitter_ms = (uint32_t)diff;

    /* EMA with alpha = 0.1, seeded by the first measurable sample. */
    ms->avg_jitter_ms = (uint32_t)((ms->last_jitter_ms + 9ULL * ms->avg_jitter_ms) / 10);

    ms->osd->jitter_ms = ms->avg_jitter_ms;
}

/**
 * Process a profile message: P:<index>:<GI>:<MCS>:<FecK>:<FecN>:<Bitrate>:<GOP>:<Power>:<Bandwidth>:<timestamp>
 */
static void msg_process_profile(msg_state_t *ms, const char *msg, size_t msg_len) {
    Profile profile;
    memset(&profile, 0, sizeof(profile));

    int profile_index = 0;
    uint64_t gs_timestamp_ms = 0;
    int32_t rtt_ms = -1;

    /* strndup bounds the copy to the declared payload, so we never read past
     * the frame even if the buffer wasn't NUL-terminated upstream. */
    char *msgCopy = strndup(msg, msg_len);
    if (msgCopy == NULL) {
        ERROR_LOG(ms->cfg, "Failed to allocate memory\n");
        return;
    }

    char *saveptr = NULL;
    char *token = strtok_r(msgCopy, ":", &saveptr);
    int index = 0;

    while (token != NULL) {
        switch (index) {
            case 0:
                profile_index = atoi(token);
                break;
            case 1:
                strncpy(profile.setGI, token, sizeof(profile.setGI) - 1);
                profile.setGI[sizeof(profile.setGI) - 1] = '\0';
                break;
            case 2:
                profile.setMCS = atoi(token);
                break;
            case 3:
                profile.setFecK = atoi(token);
                break;
            case 4:
                profile.setFecN = atoi(token);
                break;
            case 5:
                profile.setBitrate = atoi(token);
                break;
            case 6:
                profile.setGop = atof(token);
                break;
            case 7:
                profile.wfbPower = atoi(token);
                break;
            case 8:
                profile.bandwidth = atoi(token);
                break;
            case 9:
                gs_timestamp_ms = strtoull(token, NULL, 10);
                break;
            case 10:
                rtt_ms = (int32_t)atoi(token);
                break;
            default:
                break;
        }
        token = strtok_r(NULL, ":", &saveptr);
        index++;
    }

    free(msgCopy);

    /* Update inter-arrival jitter display from GS/drone timestamps */
    msg_update_jitter(ms, gs_timestamp_ms);

    /* RTT is computed on the GS (from H:/I: handshake t2/t3 deltas) and
     * echoed back here purely for display. -1 is the "no sample yet"
     * sentinel; leave the OSD field untouched in that case so the render
     * path can suppress the suffix. */
    if (rtt_ms >= 0) {
        ms->osd->rtt_ms = rtt_ms;
    }

    /* Clear init placeholder text on first profile message */
    if (!ms->gs_connected) {
        ms->gs_connected = true;
        ms->osd->gs_stats[0] = '\0';
        ms->osd->score_related[0] = '\0';
    }

    profile_apply_direct(ms->ps, &profile, profile_index, ms->osd);
}

void msg_process(msg_state_t *ms, const char *msg, size_t msg_len) {
    if (msg_len >= 2 && msg[0] == 'P' && msg[1] == ':') {
        msg_process_profile(ms, msg + 2, msg_len - 2);
    } else if (msg_len == 1 && msg[0] == 'K') {
        keyframe_fire_request(ms->ks, ms->cfg, ms->cmd);
    } else {
        ERROR_LOG(ms->cfg, "Unknown message format: %.20s...\n", msg);
    }
}

