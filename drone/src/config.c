/**
 * @file config.c
 * @brief Configuration loading for alink.conf.
 */
#include "config.h"
#include "util.h"
#include "osd.h"

void config_set_defaults(alink_config_t *cfg) {
    memset(cfg, 0, sizeof(*cfg));

    cfg->allow_set_power = 1;

    cfg->fallback_ms = 1000;

    /* Fallback profile defaults */
    strncpy(cfg->fallback_profile.setGI, "long", sizeof(cfg->fallback_profile.setGI));
    cfg->fallback_profile.setMCS = 1;
    cfg->fallback_profile.setFecK = 8;
    cfg->fallback_profile.setFecN = 12;
    cfg->fallback_profile.setBitrate = 4096;
    cfg->fallback_profile.setGop = 1.0f;
    cfg->fallback_profile.wfbPower = 2500;
    cfg->fallback_profile.bandwidth = 20;

    cfg->allow_xtx_reduce_bitrate = 1;
    cfg->xtx_reduce_bitrate_factor = 0.5f;
    cfg->check_xtx_period_ms = 200;

    cfg->allow_request_keyframe = 1;
    cfg->allow_rq_kf_by_tx_d = 1;
    cfg->request_keyframe_interval_ms = 50;
    cfg->idr_every_change = false;

    cfg->limitFPS = 1;
    cfg->get_card_info_from_yaml = false;
    cfg->osd_level = 4;
    cfg->multiply_font_size_by = 0.5f;
    cfg->log_level = LOG_LEVEL_INFO;
    cfg->roiqp_hi = 11000;
    cfg->roiqp_lo = 2000;
    cfg->roiqp_base = 0;

    strncpy(cfg->customOSD, "&L%d0&F%d&B &C tx&Wc", sizeof(cfg->customOSD));
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
            /* Strip surrounding quotes from value */
            size_t vlen = strlen(value);
            if (vlen >= 2 && value[0] == '"' && value[vlen - 1] == '"') {
                value[vlen - 1] = '\0';
                value++;
            }
            if (strcmp(key, "allow_set_power") == 0) {
                cfg->allow_set_power = atoi(value);
            } else if (strcmp(key, "fallback_ms") == 0) {
                cfg->fallback_ms = atoi(value);
            } else if (strcmp(key, "fallback_gi") == 0) {
                strncpy(cfg->fallback_profile.setGI, value, sizeof(cfg->fallback_profile.setGI) - 1);
                cfg->fallback_profile.setGI[sizeof(cfg->fallback_profile.setGI) - 1] = '\0';
            } else if (strcmp(key, "fallback_mcs") == 0) {
                cfg->fallback_profile.setMCS = atoi(value);
            } else if (strcmp(key, "fallback_feck") == 0) {
                cfg->fallback_profile.setFecK = atoi(value);
            } else if (strcmp(key, "fallback_fecn") == 0) {
                cfg->fallback_profile.setFecN = atoi(value);
            } else if (strcmp(key, "fallback_bitrate") == 0) {
                cfg->fallback_profile.setBitrate = atoi(value);
            } else if (strcmp(key, "fallback_gop") == 0) {
                cfg->fallback_profile.setGop = atof(value);
            } else if (strcmp(key, "fallback_power") == 0) {
                cfg->fallback_profile.wfbPower = atoi(value);
            } else if (strcmp(key, "fallback_bandwidth") == 0) {
                cfg->fallback_profile.bandwidth = atoi(value);
            } else if (strcmp(key, "request_keyframe_interval_ms") == 0) {
                cfg->request_keyframe_interval_ms = atoi(value);
            } else if (strcmp(key, "idr_every_change") == 0) {
                cfg->idr_every_change = atoi(value);
            } else if (strcmp(key, "allow_request_keyframe") == 0) {
                cfg->allow_request_keyframe = atoi(value);
            } else if (strcmp(key, "get_card_info_from_yaml") == 0) {
                cfg->get_card_info_from_yaml = atoi(value);
            } else if (strcmp(key, "allow_rq_kf_by_tx_d") == 0) {
                cfg->allow_rq_kf_by_tx_d = atoi(value);
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
            } else if (strcmp(key, "log_level") == 0) {
                if (strcmp(value, "debug") == 0) cfg->log_level = LOG_LEVEL_DEBUG;
                else if (strcmp(value, "info") == 0) cfg->log_level = LOG_LEVEL_INFO;
                else if (strcmp(value, "error") == 0) cfg->log_level = LOG_LEVEL_ERROR;
            } else if (strcmp(key, "roiqp_hi") == 0) {
                cfg->roiqp_hi = atoi(value);
            } else if (strcmp(key, "roiqp_lo") == 0) {
                cfg->roiqp_lo = atoi(value);
            } else if (strcmp(key, "roiqp_base") == 0) {
                cfg->roiqp_base = atoi(value);
            }
            /* Command templates */
            else if (strcmp(key, "powerCommandTemplate") == 0) {
                strncpy(cfg->powerCommandTemplate, value, sizeof(cfg->powerCommandTemplate));
            } else if (strcmp(key, "fpsCommandTemplate") == 0) {
                strncpy(cfg->fpsCommandTemplate, value, sizeof(cfg->fpsCommandTemplate));
            } else if (strcmp(key, "mcsCommandTemplate") == 0) {
                strncpy(cfg->mcsCommandTemplate, value, sizeof(cfg->mcsCommandTemplate));
            } else if (strcmp(key, "fecCommandTemplate") == 0) {
                strncpy(cfg->fecCommandTemplate, value, sizeof(cfg->fecCommandTemplate));
            } else if (strcmp(key, "idrApiCommandTemplate") == 0) {
                strncpy(cfg->idrApiCommandTemplate, value, sizeof(cfg->idrApiCommandTemplate));
            } else if (strcmp(key, "apiCommandTemplate") == 0) {
                strncpy(cfg->apiCommandTemplate, value, sizeof(cfg->apiCommandTemplate));
            } else if (strcmp(key, "customOSD") == 0) {
                strncpy(cfg->customOSD, value, sizeof(cfg->customOSD));
            } else {
                fprintf(stderr, "Warning: Unrecognized configuration key: %s\n", key);
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
