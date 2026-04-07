/**
 * @file hardware.c
 * @brief Hardware detection, power tables, camera/video queries.
 */
#include "hardware.h"
#include "config.h"
#include "util.h"
#include "command.h"

static int get_resolution_inner(hw_state_t *hw) {
    char resolution[32];

    FILE *fp = popen("cli -g .video0.size", "r");
    if (fp == NULL) {
        ERROR_LOG_LEVEL(hw->log_level, "Failed to run get resolution command\n");
        return 1;
    }

    if (fgets(resolution, sizeof(resolution) - 1, fp) == NULL) {
        ERROR_LOG_LEVEL(hw->log_level, "fgets failed\n");
        pclose(fp);
        return 1;
    }

    pclose(fp);

    if (sscanf(resolution, "%dx%d", &hw->x_res, &hw->y_res) != 2) {
        ERROR_LOG_LEVEL(hw->log_level, "Failed to parse resolution\n");
        return 1;
    }

    INFO_LOG_LEVEL(hw->log_level, "Video Size: %dx%d\n", hw->x_res, hw->y_res);
    return 0;
}

void hw_init(hw_state_t *hw) {
    memset(hw, 0, sizeof(*hw));
    hw->ldpc_tx = 1;
    hw->stbc = 1;
    hw->x_res = 1920;
    hw->y_res = 1080;
    hw->global_fps = 120;
    hw->total_pixels = 1920 * 1080;
    hw->camera_bin[0] = '\0';
    hw->tx_dropped_initialized = false;
    hw->global_total_tx_dropped = 0;
}

void hw_load_vtx_info(hw_state_t *hw) {
    char command1[] = "yaml-cli-multi -i /etc/wfb.yaml -g .broadcast.ldpc";
    char command2[] = "yaml-cli-multi -i /etc/wfb.yaml -g .broadcast.stbc";

    char buffer[128];
    FILE *pipe;

    pipe = popen(command1, "r");
    if (pipe == NULL) {
        ERROR_LOG_LEVEL(hw->log_level, "Failed to run yaml reader for ldpc_tx\n");
        return;
    }
    if (fgets(buffer, sizeof(buffer), pipe) != NULL) {
        hw->ldpc_tx = atoi(buffer);
    }
    pclose(pipe);

    pipe = popen(command2, "r");
    if (pipe == NULL) {
        ERROR_LOG_LEVEL(hw->log_level, "Failed to run yaml reader for stbc\n");
        return;
    }
    if (fgets(buffer, sizeof(buffer), pipe) != NULL) {
        hw->stbc = atoi(buffer);
    }
    pclose(pipe);
}

int hw_get_camera_bin(hw_state_t *hw) {
    char sensor_config[256];

    FILE *fp = popen("cli -g .isp.sensorConfig", "r");
    if (fp == NULL) {
        ERROR_LOG_LEVEL(hw->log_level, "Failed to run sensorConfig command\n");
        return 1;
    }

    if (fgets(sensor_config, sizeof(sensor_config), fp) == NULL) {
        ERROR_LOG_LEVEL(hw->log_level, "fgets failed\n");
        pclose(fp);
        return 1;
    }

    pclose(fp);

    sensor_config[strcspn(sensor_config, "\n")] = '\0';

    const char *filename = strrchr(sensor_config, '/');
    if (filename) {
        strncpy(hw->camera_bin, filename + 1, sizeof(hw->camera_bin) - 1);
    } else {
        strncpy(hw->camera_bin, sensor_config, sizeof(hw->camera_bin) - 1);
    }

    hw->camera_bin[sizeof(hw->camera_bin) - 1] = '\0';

    INFO_LOG_LEVEL(hw->log_level, "Camera Bin: %s\n", hw->camera_bin);
    return 0;
}

int hw_get_resolution(hw_state_t *hw) {
    if (get_resolution_inner(hw) != 0) {
        ERROR_LOG_LEVEL(hw->log_level, "Failed to get resolution. Assuming 1920x1080\n");
        hw->x_res = 1920;
        hw->y_res = 1080;
    }
    hw->total_pixels = hw->x_res * hw->y_res;
    return 0;
}

int hw_get_video_fps(hw_state_t *hw) {
    char command[] = "cli -g .video0.fps";
    char buffer[128];
    FILE *pipe;
    int fps = 0;

    pipe = popen(command, "r");
    if (pipe == NULL) {
        ERROR_LOG_LEVEL(hw->log_level, "Failed to run cli -g .video0.fps\n");
        return -1;
    }

    if (fgets(buffer, sizeof(buffer), pipe) != NULL) {
        fps = atoi(buffer);
    }

    pclose(pipe);
    return fps;
}

void hw_read_wfb_status(int *k, int *n, int *stbc_val, int *ldpc, int *short_gi,
                        int *actual_bandwidth, int *mcs_index, int *vht_mode, int *vht_nss) {
    char buffer[256];
    FILE *fp;

    fp = popen("wfb_tx_cmd 8000 get_fec", "r");
    if (fp == NULL) {
        perror("Failed to run wfb_tx_cmd command");
        return;
    }
    while (fgets(buffer, sizeof(buffer), fp) != NULL) {
        if (sscanf(buffer, "k=%d", k) == 1) continue;
        if (sscanf(buffer, "n=%d", n) == 1) continue;
    }
    pclose(fp);

    fp = popen("wfb_tx_cmd 8000 get_radio", "r");
    if (fp == NULL) {
        perror("Failed to run wfb_tx_cmd command");
        return;
    }
    while (fgets(buffer, sizeof(buffer), fp) != NULL) {
        if (sscanf(buffer, "stbc=%d", stbc_val) == 1) continue;
        if (sscanf(buffer, "ldpc=%d", ldpc) == 1) continue;
        if (sscanf(buffer, "short_gi=%d", short_gi) == 1) continue;
        if (sscanf(buffer, "bandwidth=%d", actual_bandwidth) == 1) continue;
        if (sscanf(buffer, "mcs_index=%d", mcs_index) == 1) continue;
        if (sscanf(buffer, "vht_mode=%d", vht_mode) == 1) continue;
        if (sscanf(buffer, "vht_nss=%d", vht_nss) == 1) continue;
    }
    pclose(fp);
}

int hw_get_wlan0_channel(void) {
    FILE *fp;
    char line[256];
    int channel = -1;

    fp = popen("iw dev wlan0 info", "r");
    if (!fp) {
        perror("popen");
        return -1;
    }

    while (fgets(line, sizeof(line), fp)) {
        char *p = strstr(line, "channel ");
        if (p) {
            p += strlen("channel ");
            channel = atoi(p);
            break;
        }
    }

    pclose(fp);
    return channel;
}

int hw_setup_roi(hw_state_t *hw) {
    /* Round resolution to multiples of 32 (macroblock alignment) */
    int rx = (hw->x_res / 32) * 32;
    int ry = (hw->y_res / 32) * 32;

    /* 3 zones: left 25%, center 50%, right = remainder */
    int left_w   = (rx / 4 / 32) * 32;
    int center_w = (rx / 2 / 32) * 32;
    int right_w  = ((rx - left_w - center_w) / 32) * 32;

    int coord0 = 0;
    int coord1 = left_w;
    int coord2 = left_w + center_w;

    char roi_rect[256];
    snprintf(roi_rect, sizeof(roi_rect),
             "%dx%dx%dx%d,%dx%dx%dx%d,%dx%dx%dx%d",
             coord0, 0, left_w, ry,
             coord1, 0, center_w, ry,
             coord2, 0, right_w, ry);

    char command[512];
    snprintf(command, sizeof(command), "cli -s .fpv.roiRect %s", roi_rect);

    INFO_LOG_LEVEL(hw->log_level, "Setting ROI rect: %s\n", roi_rect);

    FILE *fp = popen(command, "r");
    if (fp == NULL) {
        ERROR_LOG_LEVEL(hw->log_level, "Failed to set fpv.roiRect\n");
        return 1;
    }
    pclose(fp);
    return 0;
}

long hw_get_tx_dropped(hw_state_t *hw) {
    const char *path = "/sys/class/net/wlan0/statistics/tx_dropped";
    FILE *fp = fopen(path, "r");
    if (!fp) {
        return 0;
    }

    long tx_dropped;
    if (fscanf(fp, "%ld", &tx_dropped) != 1) {
        fclose(fp);
        return 0;
    }
    fclose(fp);

    long delta = tx_dropped - hw->global_total_tx_dropped;
    hw->global_total_tx_dropped = tx_dropped;
    
    if (!hw->tx_dropped_initialized) {
        hw->tx_dropped_initialized = true;
        return 0;
    }
    
    return delta;
}
