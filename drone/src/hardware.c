/**
 * @file hardware.c
 * @brief Hardware detection, power tables, camera/video queries.
 */
#include "hardware.h"
#include "config.h"
#include "util.h"
#include "command.h"

/* TTL for cached camera FPS/resolution (milliseconds).
 * Coalesces handshake bursts so the camera API isn't hit per probe,
 * while still picking up runtime camera reconfigs within ~2s. */
#define CAMERA_INFO_CACHE_INTERVAL_MS 2000



static int get_resolution_inner(hw_state_t *hw) {
    char response[256];

    if (http_get(VENC_API_HOST, VENC_API_PORT, "/api/v1/get?video0.size",
                 response, sizeof(response), VENC_QUERY_TIMEOUT_MS) != 0) {
        ERROR_LOG_LEVEL(hw->log_level, "Failed to query video0.size from waybeam_venc\n");
        return 1;
    }

    char size_str[32];
    if (util_venc_parse_str_value(response, size_str, sizeof(size_str)) != 0) {
        ERROR_LOG_LEVEL(hw->log_level, "Failed to parse video0.size response\n");
        return 1;
    }

    if (sscanf(size_str, "%dx%d", &hw->x_res, &hw->y_res) != 2) {
        ERROR_LOG_LEVEL(hw->log_level, "Failed to parse resolution from '%s'\n", size_str);
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
    hw->camera_info_cache_time = 0;
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
    char response[256];

    if (http_get(VENC_API_HOST, VENC_API_PORT, "/api/v1/get?isp.sensorBin",
                 response, sizeof(response), VENC_QUERY_TIMEOUT_MS) != 0) {
        ERROR_LOG_LEVEL(hw->log_level, "Failed to query isp.sensorBin from waybeam_venc\n");
        return 1;
    }

    char sensor_path[256];
    if (util_venc_parse_str_value(response, sensor_path, sizeof(sensor_path)) != 0) {
        ERROR_LOG_LEVEL(hw->log_level, "Failed to parse isp.sensorBin response\n");
        return 1;
    }

    const char *filename = strrchr(sensor_path, '/');
    if (filename) {
        strncpy(hw->camera_bin, filename + 1, sizeof(hw->camera_bin) - 1);
    } else {
        strncpy(hw->camera_bin, sensor_path, sizeof(hw->camera_bin) - 1);
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
    char response[256];

    if (http_get(VENC_API_HOST, VENC_API_PORT, "/api/v1/get?video0.fps",
                 response, sizeof(response), VENC_QUERY_TIMEOUT_MS) != 0) {
        ERROR_LOG_LEVEL(hw->log_level, "Failed to query video0.fps from waybeam_venc\n");
        return -1;
    }

    int fps = 0;
    if (util_venc_parse_int_value(response, &fps) != 0) {
        ERROR_LOG_LEVEL(hw->log_level, "Failed to parse video0.fps response\n");
        return -1;
    }

    return fps;
}

void hw_refresh_camera_info(hw_state_t *hw) {
    uint64_t now = util_now_ms();
    if (hw->camera_info_cache_time != 0 &&
        (now - hw->camera_info_cache_time) < CAMERA_INFO_CACHE_INTERVAL_MS) {
        return;
    }

    int fps = hw_get_video_fps(hw);
    if (fps >= 0) {
        hw->global_fps = fps;
    }
    hw_get_resolution(hw);

    hw->camera_info_cache_time = now;
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
