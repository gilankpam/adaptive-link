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
#include <time.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <stdio.h>

void msg_init(msg_state_t *ms, profile_state_t *ps, keyframe_state_t *ks,
              osd_state_t *osd, alink_config_t *cfg,
              pthread_mutex_t *pause_mutex, volatile bool *paused,
              const cmd_ctx_t *cmd) {
    memset(ms, 0, sizeof(*ms));
    ms->ps = ps;
    ms->ks = ks;
    ms->osd = osd;
    ms->cfg = cfg;
    ms->pause_mutex = pause_mutex;
    ms->paused = paused;
    ms->cmd = cmd;
    ms->jitter_first_sample = true;
    ms->prev_gs_ts_ms = 0;
    ms->prev_drone_ts_ms = 0;
    ms->last_jitter_ms = 0;
    ms->avg_jitter_ms = 0;
}

int msg_handle_hello(const char *payload, size_t payload_len,
                     const hw_state_t *hw, int sockfd,
                     const struct sockaddr_in *client_addr) {
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

    long t2 = util_now_ms();

    /* Build the reply. T3 is stamped just before sendto. */
    char reply_payload[128];
    long t3 = util_now_ms();
    int n = snprintf(reply_payload, sizeof(reply_payload),
                     "I:%lld:%ld:%ld:%d:%d:%d",
                     t1, t2, t3,
                     hw->global_fps, hw->x_res, hw->y_res);
    if (n <= 0 || (size_t)n >= sizeof(reply_payload)) {
        return -1;
    }

    /* Frame with 4-byte BE length prefix. */
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

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    uint64_t drone_time_ms = (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;

    if (ms->jitter_first_sample) {
        /* Seed the delta pair; no jitter measurement possible yet. */
        ms->prev_gs_ts_ms = gs_send_time_ms;
        ms->prev_drone_ts_ms = drone_time_ms;
        ms->jitter_first_sample = false;
        snprintf(ms->osd->jitter, sizeof(ms->osd->jitter), "Jit: 0ms");
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

    snprintf(ms->osd->jitter, sizeof(ms->osd->jitter), "Jit: %ums", ms->avg_jitter_ms);
}

/**
 * Handle an optional IDR code from the message.
 */
static void msg_handle_idr(msg_state_t *ms, const char *idr_code) {
    if (idr_code[0] != '\0') {
        char keyframe_request[64];
        snprintf(keyframe_request, sizeof(keyframe_request),
                 "special:request_keyframe:%s", idr_code);
        keyframe_handle_special(ms->ks, keyframe_request, ms->cfg,
                                ms->ps->prevSetGop,
                                ms->paused, ms->pause_mutex,
                                ms->cmd);
    }
}

/**
 * Process a profile message: P:<index>:<GI>:<MCS>:<FecK>:<FecN>:<Bitrate>:<GOP>:<Power>:<Bandwidth>:<timestamp>[:<idr_code>]
 */
static void msg_process_profile(msg_state_t *ms, const char *msg) {
    Profile profile;
    memset(&profile, 0, sizeof(profile));

    int profile_index = 0;
    uint64_t gs_timestamp_ms = 0;
    char idr_code[16] = "";

    char *msgCopy = strdup(msg);
    if (msgCopy == NULL) {
        ERROR_LOG(ms->cfg, "Failed to allocate memory\n");
        return;
    }

    char *token = strtok(msgCopy, ":");
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
                strncpy(idr_code, token, sizeof(idr_code) - 1);
                idr_code[sizeof(idr_code) - 1] = '\0';
                break;
            default:
                break;
        }
        token = strtok(NULL, ":");
        index++;
    }

    free(msgCopy);

    msg_handle_idr(ms, idr_code);

    /* Update inter-arrival jitter display from GS/drone timestamps */
    msg_update_jitter(ms, gs_timestamp_ms);

    /* Clear init placeholder text on first profile message */
    if (!ms->gs_connected) {
        ms->gs_connected = true;
        ms->osd->gs_stats[0] = '\0';
        ms->osd->score_related[0] = '\0';
    }

    /* Apply profile if not paused */
    pthread_mutex_lock(ms->pause_mutex);
    if (!*(ms->paused)) {
        profile_apply_direct(ms->ps, &profile, profile_index, ms->osd);
    } else {
        INFO_LOG(ms->cfg, "Adaptive mode paused, waiting for resume command...\n");
    }
    pthread_mutex_unlock(ms->pause_mutex);
}

void msg_process(msg_state_t *ms, const char *msg) {
    if (msg[0] == 'P' && msg[1] == ':') {
        msg_process_profile(ms, msg + 2);
    } else {
        ERROR_LOG(ms->cfg, "Unknown message format: %.20s...\n", msg);
    }
}

