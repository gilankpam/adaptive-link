/**
 * @file osd.h
 * @brief On-screen display string assembly, font sizing, and output.
 *
 * Manages OSD string buffers shared across threads, provides the OSD
 * update thread, error display, and font size calculation.
 */
#ifndef ALINK_OSD_H
#define ALINK_OSD_H

#include "alink_types.h"

/* Forward declarations to avoid circular includes */
struct alink_config_t_tag;
struct hw_state_t_tag;
struct profile_state_t_tag;
struct keyframe_state_t_tag;
struct rssi_state_t_tag;

typedef struct {
    char profile[48];
    char profile_fec[16];
    char regular[64];
    char gs_stats[64];
    char extra_stats[256];
    char score_related[64];
    uint32_t jitter_ms;           /* Jitter in milliseconds */
    int32_t rtt_ms;               /* GS-computed handshake RTT in ms (-1 = no sample yet) */
    int set_osd_font_size;
    int set_osd_colour;
    
    /* Optimization fields */
    int cached_channel;           /* Cached WiFi channel number */
    uint64_t channel_cache_time;  /* Cache timestamp in milliseconds */
    char last_osd_string[600];    /* Last written OSD string for change detection */
    bool last_string_valid;       /* Track if last_osd_string has valid content */

    /* Debug-OSD delta tracking. tx_bytes / tx_dropped come from the kernel
     * (wlan0 statistics) and are read every OSD tick, so we cache the
     * previous sample + its wall-clock to derive per-second rates. wfb_tx
     * stats are queried every ~1 s from the OSD thread (gated by
     * cfg->debug_osd); stats_ok tracks whether the last query succeeded so
     * the render path can degrade gracefully on an old wfb_tx. */
    long prev_tx_bytes;
    long prev_tx_dropped;
    uint64_t prev_sample_time_ms;
    bool debug_samples_valid;

    uint64_t prev_wfb_stats_query_ms;
    struct {
        uint64_t p_fec_timeouts;
        uint64_t p_truncated;
    } wfb_stats_cached;
    bool wfb_stats_ok;
} osd_state_t;

typedef struct {
    osd_udp_config_t *udp_config;
    osd_state_t *osd;
    void *cfg;     /* alink_config_t* */
    void *hw;      /* hw_state_t* */
    void *ps;      /* profile_state_t* */
    void *ks;      /* keyframe_state_t* */
    void *rs;      /* rssi_state_t* */
    void *ms;      /* msg_state_t* */
    void *cmd;     /* cmd_ctx_t* — used by debug OSD to query wfb_tx stats */
    volatile bool *initialized;
} osd_thread_arg_t;

void osd_init(osd_state_t *os);
void osd_error(const char *message);
void osd_adjust_font_size(osd_state_t *os, int x_res, float multiply_by);
void *osd_thread_func(void *arg);

#endif /* ALINK_OSD_H */
