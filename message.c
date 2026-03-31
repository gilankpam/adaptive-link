/**
 * @file message.c
 * @brief UDP heartbeat message parsing and processing.
 */
#include "message.h"
#include "util.h"
#include "rssi_monitor.h"

void msg_init(msg_state_t *ms, profile_state_t *ps, keyframe_state_t *ks,
              osd_state_t *osd, alink_config_t *cfg, rssi_state_t *rs,
              pthread_mutex_t *pause_mutex, volatile bool *paused) {
    memset(ms, 0, sizeof(*ms));
    ms->ps = ps;
    ms->ks = ks;
    ms->osd = osd;
    ms->cfg = cfg;
    ms->rs = rs;
    ms->pause_mutex = pause_mutex;
    ms->paused = paused;
    ms->time_synced = false;
    ms->num_antennas = 0;
    ms->noise_pnlty = 0;
    ms->fec_change = 0;
    ms->prev_fec_change = 0;
    ms->first_time = 1;
    memset(&ms->last_fec_call_time, 0, sizeof(ms->last_fec_call_time));
}

void msg_process(msg_state_t *ms, const char *msg) {
    profile_state_t *ps = ms->ps;
    alink_config_t *cfg = ms->cfg;

    int transmitted_time = 0;
    int link_value_rssi = FALLBACK_SCORE;
    int link_value_snr = FALLBACK_SCORE;
    int recovered = 0;
    int lost_packets = 0;
    int rssi1 = -105;
    int snr1 = 0;
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
                transmitted_time = atoi(token);
                break;
            case 1:
                link_value_rssi = atoi(token);
                break;
            case 2:
                link_value_snr = atoi(token);
                break;
            case 3:
                recovered = atoi(token);
                break;
            case 4:
                lost_packets = atoi(token);
                break;
            case 5:
                rssi1 = atoi(token);
                break;
            case 6:
                snr1 = atoi(token);
                break;
            case 7:
                ms->num_antennas = atoi(token);
                break;
            case 8:
                ms->noise_pnlty = atoi(token);
                ps->noise_pnlty = ms->noise_pnlty;
                break;
            case 9:
                ms->fec_change = atoi(token);
                ps->fec_change = ms->fec_change;
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

    /* Request a keyframe if an idr_code is provided */
    if (idr_code[0] != '\0') {
        char keyframe_request[64];
        snprintf(keyframe_request, sizeof(keyframe_request), "special:request_keyframe:%s", idr_code);
        keyframe_handle_special(ms->ks, keyframe_request, cfg, ps->prevSetGop,
                                ms->paused, ms->pause_mutex);
    }

    struct timeval current_time;
    gettimeofday(&current_time, NULL);

    if (ms->first_time) {
        ms->last_fec_call_time = current_time;
        ms->first_time = 0;
    }

    long elapsed_ms = util_elapsed_ms_timeval(&current_time, &ms->last_fec_call_time);

    if (cfg->allow_dynamic_fec && ms->fec_change != ms->prev_fec_change && elapsed_ms >= 1000) {
        profile_apply_fec_bitrate(ps, ps->prevSetFecK, ps->prevSetFecN, ps->prevSetBitrate);
        ms->last_fec_call_time = current_time;
        ms->prev_fec_change = ms->fec_change;
    }

    /* Create OSD string with ground station stats */
    int num_antennas_drone = rssi_get_num_antennas_drone(ms->rs);
    if (num_antennas_drone > 0) {
        sprintf(ms->osd->gs_stats, "rssi%d snr%d fec%d lost%d ants:vrx%d,vtx%d",
                rssi1, snr1, recovered, lost_packets, ms->num_antennas, num_antennas_drone);
    } else {
        sprintf(ms->osd->gs_stats, "rssi%d snr%d fec%d lost%d ants:vrx%d",
                rssi1, snr1, recovered, lost_packets, ms->num_antennas);
    }

    /* Time synchronization */
    if (!ms->time_synced) {
        if (transmitted_time > 0) {
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

    /* Start selection if not paused */
    pthread_mutex_lock(ms->pause_mutex);
    if (!*(ms->paused)) {
        profile_start_selection(ps, link_value_rssi, link_value_snr, recovered, ms->osd);
    } else {
        printf("Adaptive mode paused, waiting for resume command...\n");
    }
    pthread_mutex_unlock(ms->pause_mutex);
}
