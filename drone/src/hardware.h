/**
 * @file hardware.h
 * @brief Hardware detection, power tables, camera/video queries.
 *
 * Manages WiFi adapter detection, TX power table loading from YAML,
 * LDPC/STBC configuration, camera sensor info, video resolution/FPS,
 * ROI setup, and wfb-ng radio status queries.
 */
#ifndef ALINK_HARDWARE_H
#define ALINK_HARDWARE_H

#include "alink_types.h"
#include "command.h"

typedef struct {
    int ldpc_tx;
    int stbc;
    char camera_bin[64];
    /* Written by the OSD thread (hw_refresh_camera_info) and read lockless by
     * the recv thread (hello reply). `volatile` forces a fresh load on each
     * read; word-aligned int reads are atomic on every target we ship on, so
     * there is no tearing concern. Stale-by-one-TTL is acceptable. */
    volatile int x_res;
    volatile int y_res;
    volatile int global_fps;
    bool tx_dropped_initialized;
    /* Written by tx_monitor thread, read lockless by the OSD thread —
     * volatile forces a fresh load each tick. Single-writer, so no RMW race. */
    volatile long global_total_tx_dropped;
    uint64_t camera_info_cache_time;  /* Last camera FPS/res refresh (ms) */
    log_level_t log_level;
} hw_state_t;

void hw_init(hw_state_t *hw);
void hw_load_vtx_info(hw_state_t *hw);
int  hw_get_camera_bin(hw_state_t *hw);
int  hw_get_resolution(hw_state_t *hw);
int  hw_get_video_fps(hw_state_t *hw);
void hw_refresh_camera_info(hw_state_t *hw);
int  hw_get_wlan0_channel(void);
long hw_get_tx_dropped(hw_state_t *hw);

/**
 * Read cumulative tx_bytes and tx_packets counters for wlan0 from
 * /sys/class/net/wlan0/statistics/. On read failure the out parameters
 * are set to 0. Unlike hw_get_tx_dropped(), this does NOT compute a
 * delta — callers take their own differences between OSD ticks.
 */
void hw_get_tx_netstats(long *tx_bytes, long *tx_packets);

int  hw_setup_roi(hw_state_t *hw);

#endif /* ALINK_HARDWARE_H */
