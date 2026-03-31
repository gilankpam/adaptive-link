/**
 * @file hardware.c
 * @brief Hardware detection, power tables, camera/video queries.
 */
#include "hardware.h"
#include "util.h"

static int check_module_loaded(const char *module_name) {
    FILE *fp = fopen("/proc/modules", "r");
    if (!fp) {
        perror("Failed to open /proc/modules");
        return 0;
    }

    char line[256];
    while (fgets(line, sizeof(line), fp)) {
        if (strncmp(line, module_name, strlen(module_name)) == 0) {
            fclose(fp);
            return 1;
        }
    }

    fclose(fp);
    return 0;
}

static int get_resolution_inner(hw_state_t *hw) {
    char resolution[32];

    FILE *fp = popen("cli -g .video0.size", "r");
    if (fp == NULL) {
        printf("Failed to run get resolution command\n");
        return 1;
    }

    if (fgets(resolution, sizeof(resolution) - 1, fp) == NULL) {
        printf("fgets failed\n");
    }

    pclose(fp);

    if (sscanf(resolution, "%dx%d", &hw->x_res, &hw->y_res) != 2) {
        printf("Failed to parse resolution\n");
        return 1;
    }

    printf("Video Size: %dx%d\n", hw->x_res, hw->y_res);
    return 0;
}

void hw_init(hw_state_t *hw) {
    memset(hw, 0, sizeof(*hw));
    hw->tx_factor = 50;
    hw->ldpc_tx = 1;
    hw->stbc = 1;
    hw->x_res = 1920;
    hw->y_res = 1080;
    hw->global_fps = 120;
    hw->total_pixels = 1920 * 1080;
    hw->camera_bin[0] = '\0';
    hw->global_total_tx_dropped = 0;
}

void hw_load_tx_power_table(hw_state_t *hw) {
    char adapter[MAX_OUTPUT];
    char cmd[MAX_CMD];
    char raw[RAW_BUF];
    char tmp[MAX_OUTPUT];
    FILE *fp;

    snprintf(cmd, sizeof(cmd),
             "yaml-cli-multi -i %s -g .wireless.wlan_adapter",
             WFB_YAML);
    fp = popen(cmd, "r");
    if (!fp || !fgets(adapter, sizeof(adapter), fp)) {
        fprintf(stderr, "Error: Could not detect WiFi adapter.\n");
        if (fp) pclose(fp);
        return;
    }
    pclose(fp);
    util_strip_newline(adapter);
    printf("\n\nUsing wlan adapter: %s\n\n", adapter);

    for (int mcs = 0; mcs < MCS_COUNT; ++mcs) {
        memset(hw->tx_power_table[mcs], 0,
               sizeof(hw->tx_power_table[mcs]));

        snprintf(cmd, sizeof(cmd),
                 "yaml-cli-multi -i %s -g \".profiles.%s.tx_power.mcs%d\" | sed 's/[][]//g'",
                 WIFI_ADAPTERS_YAML, adapter, mcs);

        fp = popen(cmd, "r");
        if (!fp) {
            fprintf(stderr, "Failed to run yaml-cli-multi for MCS%d\n", mcs);
            continue;
        }

        raw[0] = '\0';
        while (fgets(tmp, sizeof(tmp), fp)) {
            util_strip_newline(tmp);
            strncat(raw, tmp, sizeof(raw) - strlen(raw) - 1);
        }
        pclose(fp);

        for (char *p = raw; *p; ++p) {
            if (*p == '"') *p = ' ';
        }

        int idx = 0;
        int last_value = 0;
        char *tok = strtok(raw, ", \t");
        while (tok && idx < POWER_LEVELS) {
            while (*tok == ' ') tok++;
            last_value = atoi(tok);
            hw->tx_power_table[mcs][idx++] = last_value;
            tok = strtok(NULL, ", \t");
        }

        for (; idx < POWER_LEVELS; idx++) {
            hw->tx_power_table[mcs][idx] = last_value;
        }
    }
}

void hw_print_tx_power_table(const hw_state_t *hw) {
    printf("TX Power Table (MCS x Power Index):\n");

    printf("        ");
    for (int i = 0; i < POWER_LEVELS; i++) {
        char hdr[5];
        snprintf(hdr, sizeof(hdr), "P%02d", i);
        printf("%5s ", hdr);
    }
    printf("\n");

    for (int m = 0; m < MCS_COUNT; m++) {
        printf("MCS%-3d: ", m);
        for (int p = 0; p < POWER_LEVELS; p++) {
            printf("%5d ", hw->tx_power_table[m][p]);
        }
        printf("\n");
    }
}

void hw_load_vtx_info(hw_state_t *hw) {
    char command1[] = "yaml-cli-multi -i /etc/wfb.yaml -g .broadcast.ldpc";
    char command2[] = "yaml-cli-multi -i /etc/wfb.yaml -g .broadcast.stbc";

    char buffer[128];
    FILE *pipe;

    pipe = popen(command1, "r");
    if (pipe == NULL) {
        fprintf(stderr, "Failed to run yaml reader for ldpc_tx\n");
        return;
    }
    if (fgets(buffer, sizeof(buffer), pipe) != NULL) {
        hw->ldpc_tx = atoi(buffer);
    }
    pclose(pipe);

    pipe = popen(command2, "r");
    if (pipe == NULL) {
        fprintf(stderr, "Failed to run yaml reader for stbc\n");
        return;
    }
    if (fgets(buffer, sizeof(buffer), pipe) != NULL) {
        hw->stbc = atoi(buffer);
    }
    pclose(pipe);
}

void hw_determine_tx_factor(hw_state_t *hw) {
    if (check_module_loaded("88XXau")) {
        hw->tx_factor = -100;
        printf("Found 88XXau card\n");
    } else {
        hw->tx_factor = 50;
        printf("Did not find 88XXau\n");
    }
}

int hw_get_camera_bin(hw_state_t *hw) {
    char sensor_config[256];

    FILE *fp = popen("cli -g .isp.sensorConfig", "r");
    if (fp == NULL) {
        printf("Failed to run sensorConfig command\n");
        return 1;
    }

    if (fgets(sensor_config, sizeof(sensor_config) - 1, fp) == NULL) {
        printf("fgets failed\n");
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

    printf("Camera Bin: %s\n", hw->camera_bin);
    return 0;
}

int hw_get_resolution(hw_state_t *hw) {
    if (get_resolution_inner(hw) != 0) {
        printf("Failed to get resolution. Assuming 1920x1080\n");
        hw->x_res = 1920;
        hw->y_res = 1080;
    }
    hw->total_pixels = hw->x_res * hw->y_res;
    return 0;
}

int hw_get_video_fps(void) {
    char command[] = "cli -g .video0.fps";
    char buffer[128];
    FILE *pipe;
    int fps = 0;

    pipe = popen(command, "r");
    if (pipe == NULL) {
        fprintf(stderr, "Failed to run cli -g .video0.fps\n");
        return -1;
    }

    if (fgets(buffer, sizeof(buffer), pipe) != NULL) {
        fps = atoi(buffer);
    }

    pclose(pipe);
    return fps;
}

int hw_setup_roi(const hw_state_t *hw) {
    FILE *fp;

    int rounded_x_res = (int)floor(hw->x_res / 32) * 32;
    int rounded_y_res = (int)floor(hw->y_res / 32) * 32;

    int roi_height, start_roi_y;
    if (rounded_y_res != hw->y_res) {
        roi_height = rounded_y_res - 32;
        start_roi_y = 32;
    } else {
        roi_height = rounded_y_res;
        start_roi_y = hw->y_res - rounded_y_res;
    }

    roi_height = roi_height - 32;
    start_roi_y = start_roi_y + 32;

    int edge_roi_width = (int)floor(rounded_x_res / 8 / 32) * 32;
    int next_roi_width = ((int)floor(rounded_x_res / 8 / 32) * 32) + 32;

    int coord0 = 0;
    int coord1 = edge_roi_width;
    int coord2 = hw->x_res - edge_roi_width - next_roi_width;
    int coord3 = hw->x_res - edge_roi_width;

    char roi_define[256];
    snprintf(roi_define, sizeof(roi_define), "%dx%dx%dx%d,%dx%dx%dx%d,%dx%dx%dx%d,%dx%dx%dx%d",
             coord0, start_roi_y, edge_roi_width, roi_height,
             coord1, start_roi_y, next_roi_width, roi_height,
             coord2, start_roi_y, next_roi_width, roi_height,
             coord3, start_roi_y, edge_roi_width, roi_height);

    char command[512];
    snprintf(command, sizeof(command), "cli -s .fpv.roiRect %s", roi_define);

    char enabled_status[16];
    fp = popen("cli -g .fpv.enabled", "r");
    if (fp == NULL) {
        printf("Failed to run command\n");
        return 1;
    }

    if (fgets(enabled_status, sizeof(enabled_status) - 1, fp) == NULL) {
        printf("fgets failed\n");
    }

    enabled_status[strcspn(enabled_status, "\n")] = 0;

    if (strcmp(enabled_status, "true") != 0 && strcmp(enabled_status, "false") != 0) {
        if (system("cli -s .fpv.enabled true") != 0) { printf("problem with reading fpv.enabled status\n"); }
    }

    if (system(command) != 0) { printf("set ROI command failed\n"); }

    char roi_qp_status[32];
    fp = popen("cli -g .fpv.roiQp", "r");
    if (fp == NULL) {
        printf("Failed to run command\n");
        return 1;
    }

    if (fgets(roi_qp_status, sizeof(roi_qp_status) - 1, fp) == NULL) { printf("fgets failed\n"); }
    pclose(fp);

    roi_qp_status[strcspn(roi_qp_status, "\n")] = 0;

    int num_count = 0;
    char *token = strtok(roi_qp_status, ",");
    while (token != NULL) {
        num_count++;
        token = strtok(NULL, ",");
    }

    if (num_count != 4) {
        if (system("cli -s .fpv.roiQp 0,0,0,0") != 0) { printf("Command failed\n"); }
    }

    return 0;
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
    return delta;
}
