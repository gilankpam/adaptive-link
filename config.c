/**
 * @file config.c
 * @brief Configuration loading for alink.conf and txprofiles.conf.
 */
#include "config.h"
#include "util.h"
#include "osd.h"

void config_set_defaults(alink_config_t *cfg) {
    memset(cfg, 0, sizeof(*cfg));

    cfg->allow_set_power = 1;
    cfg->use_0_to_4_txpower = 0;
    cfg->power_level_0_to_4 = 0;

    cfg->rssi_weight = 0.5f;
    cfg->snr_weight = 0.5f;
    cfg->hold_fallback_mode_s = 2;
    cfg->hold_modes_down_s = 2;
    cfg->min_between_changes_ms = 100;
    cfg->hysteresis_percent = 15;
    cfg->hysteresis_percent_down = 5;
    cfg->smoothing_factor = 0.5f;
    cfg->smoothing_factor_down = 0.8f;
    cfg->limit_max_score_to = 2000;
    cfg->fallback_ms = 1000;
    cfg->baseline_value = 100;

    cfg->allow_dynamic_fec = 1;
    cfg->fec_k_adjust = 0;
    cfg->spike_fix_dynamic_fec = 1;

    cfg->allow_xtx_reduce_bitrate = 1;
    cfg->xtx_reduce_bitrate_factor = 0.5f;
    cfg->check_xtx_period_ms = 500;

    cfg->allow_request_keyframe = 1;
    cfg->allow_rq_kf_by_tx_d = 1;
    cfg->request_keyframe_interval_ms = 50;
    cfg->idr_every_change = false;

    cfg->limitFPS = 1;
    cfg->roi_focus_mode = false;
    cfg->get_card_info_from_yaml = false;
    cfg->osd_level = 4;
    cfg->multiply_font_size_by = 0.5f;
    cfg->verbose_mode = false;

    strncpy(cfg->customOSD, "&L%d0&F%d&B &C tx&Wc", sizeof(cfg->customOSD));

    cfg->num_profiles = 0;
}

int config_load(alink_config_t *cfg, const char *filename) {
    FILE *file = fopen(filename, "r");
    if (!file) {
        fprintf(stderr, "Error: Could not open configuration file: %s\n", filename);
        perror("");
        osd_error("Adaptive-Link: Check/update /etc/alink.conf");
        exit(EXIT_FAILURE);
    }

    char line[BUFFER_SIZE];
    while (fgets(line, sizeof(line), file)) {
        if (line[0] == '#')
            continue;

        char *key = strtok(line, "=");
        char *value = strtok(NULL, "\n");

        if (key && value) {
            if (strcmp(key, "allow_set_power") == 0) {
                cfg->allow_set_power = atoi(value);
            } else if (strcmp(key, "use_0_to_4_txpower") == 0) {
                cfg->use_0_to_4_txpower = atoi(value);
            } else if (strcmp(key, "power_level_0_to_4") == 0) {
                cfg->power_level_0_to_4 = atoi(value);
            } else if (strcmp(key, "rssi_weight") == 0) {
                cfg->rssi_weight = atof(value);
            } else if (strcmp(key, "snr_weight") == 0) {
                cfg->snr_weight = atof(value);
            } else if (strcmp(key, "hold_fallback_mode_s") == 0) {
                cfg->hold_fallback_mode_s = atoi(value);
            } else if (strcmp(key, "hold_modes_down_s") == 0) {
                cfg->hold_modes_down_s = atoi(value);
            } else if (strcmp(key, "min_between_changes_ms") == 0) {
                cfg->min_between_changes_ms = atoi(value);
            } else if (strcmp(key, "request_keyframe_interval_ms") == 0) {
                cfg->request_keyframe_interval_ms = atoi(value);
            } else if (strcmp(key, "fallback_ms") == 0) {
                cfg->fallback_ms = atoi(value);
            } else if (strcmp(key, "idr_every_change") == 0) {
                cfg->idr_every_change = atoi(value);
            } else if (strcmp(key, "allow_request_keyframe") == 0) {
                cfg->allow_request_keyframe = atoi(value);
            } else if (strcmp(key, "get_card_info_from_yaml") == 0) {
                cfg->get_card_info_from_yaml = atoi(value);
            } else if (strcmp(key, "allow_dynamic_fec") == 0) {
                cfg->allow_dynamic_fec = atoi(value);
            } else if (strcmp(key, "fec_k_adjust") == 0) {
                cfg->fec_k_adjust = atoi(value);
            } else if (strcmp(key, "spike_fix_dynamic_fec") == 0) {
                cfg->spike_fix_dynamic_fec = atoi(value);
            } else if (strcmp(key, "allow_rq_kf_by_tx_d") == 0) {
                cfg->allow_rq_kf_by_tx_d = atoi(value);
            } else if (strcmp(key, "hysteresis_percent") == 0) {
                cfg->hysteresis_percent = atoi(value);
            } else if (strcmp(key, "hysteresis_percent_down") == 0) {
                cfg->hysteresis_percent_down = atoi(value);
            } else if (strcmp(key, "exp_smoothing_factor") == 0) {
                cfg->smoothing_factor = atof(value);
            } else if (strcmp(key, "exp_smoothing_factor_down") == 0) {
                cfg->smoothing_factor_down = atof(value);
            } else if (strcmp(key, "roi_focus_mode") == 0) {
                cfg->roi_focus_mode = atoi(value);
            } else if (strcmp(key, "allow_spike_fix_fps") == 0) {
                cfg->limitFPS = atoi(value);
            } else if (strcmp(key, "allow_xtx_reduce_bitrate") == 0) {
                cfg->allow_xtx_reduce_bitrate = atoi(value);
            } else if (strcmp(key, "xtx_reduce_bitrate_factor") == 0) {
                cfg->xtx_reduce_bitrate_factor = atof(value);
            } else if (strcmp(key, "osd_level") == 0) {
                cfg->osd_level = atoi(value);
            } else if (strcmp(key, "multiply_font_size_by") == 0) {
                cfg->multiply_font_size_by = atof(value);
            } else if (strcmp(key, "check_xtx_period_ms") == 0) {
                cfg->check_xtx_period_ms = atoi(value);
            }
            /* Command templates */
            else if (strcmp(key, "powerCommandTemplate") == 0) {
                strncpy(cfg->powerCommandTemplate, value, sizeof(cfg->powerCommandTemplate));
            } else if (strcmp(key, "fpsCommandTemplate") == 0) {
                strncpy(cfg->fpsCommandTemplate, value, sizeof(cfg->fpsCommandTemplate));
            } else if (strcmp(key, "qpDeltaCommandTemplate") == 0) {
                strncpy(cfg->qpDeltaCommandTemplate, value, sizeof(cfg->qpDeltaCommandTemplate));
            } else if (strcmp(key, "mcsCommandTemplate") == 0) {
                strncpy(cfg->mcsCommandTemplate, value, sizeof(cfg->mcsCommandTemplate));
            } else if (strcmp(key, "bitrateCommandTemplate") == 0) {
                strncpy(cfg->bitrateCommandTemplate, value, sizeof(cfg->bitrateCommandTemplate));
            } else if (strcmp(key, "gopCommandTemplate") == 0) {
                strncpy(cfg->gopCommandTemplate, value, sizeof(cfg->gopCommandTemplate));
            } else if (strcmp(key, "fecCommandTemplate") == 0) {
                strncpy(cfg->fecCommandTemplate, value, sizeof(cfg->fecCommandTemplate));
            } else if (strcmp(key, "roiCommandTemplate") == 0) {
                strncpy(cfg->roiCommandTemplate, value, sizeof(cfg->roiCommandTemplate));
            } else if (strcmp(key, "idrCommandTemplate") == 0) {
                strncpy(cfg->idrCommandTemplate, value, sizeof(cfg->idrCommandTemplate));
            } else if (strcmp(key, "customOSD") == 0) {
                strncpy(cfg->customOSD, value, sizeof(cfg->customOSD));
            } else {
                fprintf(stderr, "Warning: Unrecognized configuration key: %s\n", key);
                osd_error("Adaptive-Link: Check/update /etc/alink.conf");
                exit(EXIT_FAILURE);
            }
        } else if (strlen(line) > 1 && line[0] != '\n') {
            fprintf(stderr, "Error: Invalid configuration format: %s\n", line);
            osd_error("Adaptive-Link: Check/update /etc/alink.conf");
            exit(EXIT_FAILURE);
        }
    }

    fclose(file);
    return 0;
}

int config_load_profiles(alink_config_t *cfg, const char *filename) {
    FILE *file = fopen(filename, "r");
    if (!file) {
        fprintf(stderr, "Problem loading %s: ", filename);
        osd_error("Adaptive-Link: Check /etc/txprofiles.conf");
        perror("");
        exit(1);
    }

    char line[256];
    int i = 0;

    while (fgets(line, sizeof(line), file) && i < MAX_PROFILES) {
        char *comment = strchr(line, '#');
        if (comment) *comment = '\0';

        util_trim_whitespace(line);
        util_normalize_whitespace(line);

        if (*line == '\0') continue;

        if (sscanf(line, "%d - %d %15s %d %d %d %d %f %d %15s %d %d",
                   &cfg->profiles[i].rangeMin, &cfg->profiles[i].rangeMax, cfg->profiles[i].setGI,
                   &cfg->profiles[i].setMCS, &cfg->profiles[i].setFecK, &cfg->profiles[i].setFecN,
                   &cfg->profiles[i].setBitrate, &cfg->profiles[i].setGop, &cfg->profiles[i].wfbPower,
                   cfg->profiles[i].ROIqp, &cfg->profiles[i].bandwidth, &cfg->profiles[i].setQpDelta) == 12) {
            i++;
        } else {
            fprintf(stderr, "Malformed line ignored: %s\n", line);
        }
    }

    cfg->num_profiles = i;
    fclose(file);
    return 0;
}

int config_update_param(const char *key, const char *value) {
    char cmd[256];
    snprintf(cmd, sizeof(cmd),
             "sed -i 's/^%s=.*/%s=%s/' %s",
             key, key, value, CONFIG_FILE);
    return system(cmd);
}
