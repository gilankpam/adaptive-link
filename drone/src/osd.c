/**
 * @file osd.c
 * @brief On-screen display string assembly, font sizing, and output.
 */
#include "osd.h"
#include "config.h"
#include "hardware.h"
#include "profile.h"
#include "keyframe.h"
#include "rssi_monitor.h"
#include "util.h"
#include "message.h"

/* Channel cache refresh interval (milliseconds) */
#define CHANNEL_CACHE_INTERVAL_MS 5000

/* OSD refresh interval (microseconds) - 10 Hz = 100ms */
#define OSD_REFRESH_INTERVAL_US 200000

void osd_init(osd_state_t *os) {
    strncpy(os->profile, "initializing...", sizeof(os->profile));
    strncpy(os->profile_fec, "0/0", sizeof(os->profile_fec));
    strncpy(os->regular, "&F%d&B &C tx&Wc", sizeof(os->regular));
    strncpy(os->gs_stats, "waiting for gs.", sizeof(os->gs_stats));
    strncpy(os->extra_stats, "initializing...", sizeof(os->extra_stats));
    strncpy(os->score_related, "initializing...", sizeof(os->score_related));
    os->jitter_ms = 0;
    os->rtt_ms = -1;
    os->set_osd_font_size = 20;
    os->set_osd_colour = 7;

    /* Initialize optimization fields */
    os->cached_channel = -1;
    os->channel_cache_time = 0;
    os->last_osd_string[0] = '\0';
    os->last_string_valid = false;
}

void osd_error(const char *message) {
    const char *prefix = "&L50&F30 ";
    char full_message[128];

    snprintf(full_message, sizeof(full_message), "%s%s", prefix, message);

    FILE *file = fopen("/tmp/MSPOSD.msg", "w");
    if (file == NULL) {
        perror("Error opening /tmp/MSPOSD.msg");
        return;
    }

    if (fwrite(full_message, sizeof(char), strlen(full_message), file) != strlen(full_message)) {
        perror("Error writing to /tmp/MSPOSD.msg");
    }

    fclose(file);
}

void osd_adjust_font_size(osd_state_t *os, int x_res, float multiply_by) {
    os->set_osd_font_size = (x_res < 1280) ? ((int)(20 * multiply_by)) :
                    (x_res < 1700) ? ((int)(25 * multiply_by)) :
                    (x_res < 2000) ? ((int)(35 * multiply_by)) :
                    (x_res < 2560) ? ((int)(45 * multiply_by)) :
                                    ((int)(50 * multiply_by));
}

/* Helper function to get WiFi channel with caching */
static int get_cached_channel(osd_state_t *os) {
    uint64_t now = util_now_ms();
    
    /* Refresh cache if expired or not initialized */
    if (os->cached_channel == -1 || (now - os->channel_cache_time) > CHANNEL_CACHE_INTERVAL_MS) {
        os->cached_channel = hw_get_wlan0_channel();
        os->channel_cache_time = now;
    }
    
    return os->cached_channel;
}

void *osd_thread_func(void *arg) {
    osd_thread_arg_t *ta = (osd_thread_arg_t *)arg;
    osd_state_t *os = ta->osd;
    osd_udp_config_t *osd_config = ta->udp_config;
    alink_config_t *cfg = (alink_config_t *)ta->cfg;
    hw_state_t *hw = (hw_state_t *)ta->hw;
    profile_state_t *ps = (profile_state_t *)ta->ps;
    keyframe_state_t *ks = (keyframe_state_t *)ta->ks;
    rssi_state_t *rs = (rssi_state_t *)ta->rs;

    struct sockaddr_in udp_out_addr;
    if (osd_config->udp_out_sock != -1) {
        memset(&udp_out_addr, 0, sizeof(udp_out_addr));
        udp_out_addr.sin_family = AF_INET;
        udp_out_addr.sin_port = htons(osd_config->udp_out_port);
        if (inet_pton(AF_INET, osd_config->udp_out_ip, &udp_out_addr.sin_addr) <= 0) {
            perror("Invalid IP address for OSD UDP output");
            pthread_exit(NULL);
        }
    }

    /* Edge-triggered weak-antenna logging: the on-screen warning fires every
     * tick (correct), but the stdout line only fires when the boolean flips,
     * so the log isn't flooded with 5 lines/sec while the condition holds. */
    bool prev_weak_antenna = false;

    while (true) {
        usleep(OSD_REFRESH_INTERVAL_US);

        /* Get cached WiFi channel (refreshes every 5 seconds) */
        int wfb_ch = get_cached_channel(os);

        snprintf(os->extra_stats, sizeof(os->extra_stats),
             "xtx:%ld idr:%d ch:%d",
             hw->global_total_tx_dropped,
             ks->total_requests,
             wfb_ch);

        bool weak_antenna = rssi_get_weak_antenna(rs);
        if (weak_antenna) {
            strncat(os->extra_stats,
                    "\nPersistent VTX antenna mismatch >= 20dB detected! Check antennas...",
                    sizeof(os->extra_stats) - strlen(os->extra_stats) - 1);
        }
        if (weak_antenna && !prev_weak_antenna) {
            INFO_LOG(cfg, "Weak drone antenna detected!\n");
        } else if (!weak_antenna && prev_weak_antenna) {
            INFO_LOG(cfg, "Drone antenna recovered.\n");
        }
        prev_weak_antenna = weak_antenna;

        os->set_osd_colour = (ps->currentProfile == -1) ? 2 : 3;

        char local_regular_osd[64];
        snprintf(local_regular_osd, sizeof(local_regular_osd), cfg->customOSD, os->set_osd_colour, os->set_osd_font_size);

        char full_osd_string[600];
        int osd_off = 0;

        /* Position prefix for top-center: &Lx1 where x=color, 1=top-center position */
        char pos_prefix[8];
        snprintf(pos_prefix, sizeof(pos_prefix), "&L%d1 ", os->set_osd_colour);
        
        if (cfg->osd_level >= 5) {
            /* Line 1: profile info */
            osd_off = snprintf(full_osd_string, sizeof(full_osd_string), "%s%s %s",
                    pos_prefix, os->profile, os->profile_fec);
            /* Line 2: regular OSD */
            osd_off += snprintf(full_osd_string + osd_off, sizeof(full_osd_string) - osd_off, "\n%s%s",
                    pos_prefix, local_regular_osd);
            /* Line 3: score related (conditional) */
            if (os->score_related[0] != '\0')
                osd_off += snprintf(full_osd_string + osd_off, sizeof(full_osd_string) - osd_off, "\n%s%s",
                        pos_prefix, os->score_related);
            /* Line 4: GS stats (conditional) */
            if (os->gs_stats[0] != '\0')
                osd_off += snprintf(full_osd_string + osd_off, sizeof(full_osd_string) - osd_off, "\n%s%s",
                        pos_prefix, os->gs_stats);
            /* Line 5: extra stats */
            osd_off += snprintf(full_osd_string + osd_off, sizeof(full_osd_string) - osd_off, "\n%s%s",
                    pos_prefix, os->extra_stats);
            /* Line 6: jitter + rtt (if available) + resolution */
            if (os->rtt_ms >= 0) {
                osd_off += snprintf(full_osd_string + osd_off, sizeof(full_osd_string) - osd_off,
                        "\n%sjit:%ums rtt:%dms res:%dp",
                        pos_prefix, os->jitter_ms, os->rtt_ms, hw->y_res);
            } else {
                osd_off += snprintf(full_osd_string + osd_off, sizeof(full_osd_string) - osd_off,
                        "\n%sjit:%ums res:%dp",
                        pos_prefix, os->jitter_ms, hw->y_res);
            }
        } else if (cfg->osd_level == 4) {
            snprintf(full_osd_string, sizeof(full_osd_string), "%s%s %s | %s | %s | %s | %s",
                    pos_prefix, os->profile, os->profile_fec, local_regular_osd, os->score_related, os->gs_stats, os->extra_stats);
        } else if (cfg->osd_level == 3) {
            snprintf(full_osd_string, sizeof(full_osd_string), "%s%s %s %s\n%s%s",
                    pos_prefix, os->profile, os->profile_fec, local_regular_osd, pos_prefix, os->gs_stats);
        } else if (cfg->osd_level == 2) {
            snprintf(full_osd_string, sizeof(full_osd_string), "%s%s %s %s",
                    pos_prefix, os->profile, os->profile_fec, local_regular_osd);
        } else if (cfg->osd_level == 1) {
            snprintf(full_osd_string, sizeof(full_osd_string), "%s%s",
                    pos_prefix, local_regular_osd);
        }

        /* Conditional update: only write if content changed */
        bool content_changed = !os->last_string_valid ||
                               strcmp(full_osd_string, os->last_osd_string) != 0;

        if (cfg->osd_level != 0 && content_changed) {
            if (osd_config->udp_out_sock != -1) {
                ssize_t sent_bytes = sendto(osd_config->udp_out_sock, full_osd_string, strlen(full_osd_string), 0,
                                        (struct sockaddr *)&udp_out_addr, sizeof(udp_out_addr));
                if (sent_bytes < 0) {
                    perror("Error sending OSD string over UDP");
                }
            } else {
                FILE *file = fopen("/tmp/MSPOSD.msg", "w");
                if (file == NULL) {
                    perror("Error opening /tmp/MSPOSD.msg");
                    continue;
                }

                if (fwrite(full_osd_string, sizeof(char), strlen(full_osd_string), file) != strlen(full_osd_string)) {
                    perror("Error writing to /tmp/MSPOSD.msg");
                }

                fclose(file);
            }

            /* Cache the written string for next-cycle change detection */
            strncpy(os->last_osd_string, full_osd_string, sizeof(os->last_osd_string) - 1);
            os->last_osd_string[sizeof(os->last_osd_string) - 1] = '\0';
            os->last_string_valid = true;
        }

        while (!*(ta->initialized)) {
            usleep(OSD_REFRESH_INTERVAL_US);
        }
    }
    return NULL;
}