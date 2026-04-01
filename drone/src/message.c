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
    ms->time_synced = false;
}

/**
 * Handle time synchronization from a GS timestamp.
 */
static void msg_handle_time_sync(msg_state_t *ms, int transmitted_time) {
    if (!ms->time_synced && transmitted_time > 0) {
        struct timeval tv;
        tv.tv_sec = transmitted_time;
        tv.tv_usec = 0;
        if (settimeofday(&tv, NULL) == 0) {
            printf("System time synchronized with transmitted time: %ld\n", (long)transmitted_time);
            ms->time_synced = true;
        } else {
            perror("Failed to set system time");
        }
    }
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
 * Process a profile message: P:<index>:<GI>:<MCS>:<FecK>:<FecN>:<Bitrate>:<GOP>:<Power>:<ROIqp>:<Bandwidth>:<QpDelta>:<timestamp>[:<idr_code>]
 */
static void msg_process_profile(msg_state_t *ms, const char *msg) {
    Profile profile;
    memset(&profile, 0, sizeof(profile));

    int profile_index = 0;
    int transmitted_time = 0;
    char idr_code[16] = "";

    char *msgCopy = strdup(msg);
    if (msgCopy == NULL) {
        perror("Failed to allocate memory");
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
                strncpy(profile.ROIqp, token, sizeof(profile.ROIqp) - 1);
                profile.ROIqp[sizeof(profile.ROIqp) - 1] = '\0';
                break;
            case 9:
                profile.bandwidth = atoi(token);
                break;
            case 10:
                profile.setQpDelta = atoi(token);
                break;
            case 11:
                transmitted_time = atoi(token);
                break;
            case 12:
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
    msg_handle_time_sync(ms, transmitted_time);

    /* Apply profile if not paused */
    pthread_mutex_lock(ms->pause_mutex);
    if (!*(ms->paused)) {
        profile_apply_direct(ms->ps, &profile, profile_index, ms->osd);
    } else {
        printf("Adaptive mode paused, waiting for resume command...\n");
    }
    pthread_mutex_unlock(ms->pause_mutex);
}

void msg_process(msg_state_t *ms, const char *msg) {
    if (msg[0] == 'P' && msg[1] == ':') {
        msg_process_profile(ms, msg + 2);
    } else {
        printf("Unknown message format: %.20s...\n", msg);
    }
}
