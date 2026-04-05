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
    int x_res;
    int y_res;
    int global_fps;
    int total_pixels;
    bool tx_dropped_initialized;
    long global_total_tx_dropped;
} hw_state_t;

void hw_init(hw_state_t *hw);
void hw_load_vtx_info(hw_state_t *hw);
int  hw_get_camera_bin(hw_state_t *hw);
int  hw_get_resolution(hw_state_t *hw);
int  hw_get_video_fps(void);
void hw_read_wfb_status(int *k, int *n, int *stbc_val, int *ldpc, int *short_gi,
                        int *actual_bandwidth, int *mcs_index, int *vht_mode, int *vht_nss);
int  hw_get_wlan0_channel(void);
long hw_get_tx_dropped(hw_state_t *hw);

#endif /* ALINK_HARDWARE_H */
