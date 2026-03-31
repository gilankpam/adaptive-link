/**
 * @file config.h
 * @brief Configuration loading for alink.conf and txprofiles.conf.
 *
 * Parses configuration files into the alink_config_t struct. All 41+
 * configuration parameters, command templates, and profile definitions
 * are centralized here.
 */
#ifndef ALINK_CONFIG_H
#define ALINK_CONFIG_H

#include "alink_types.h"

typedef struct {
    /* Power config */
    bool allow_set_power;
    bool use_0_to_4_txpower;
    int power_level_0_to_4;

    /* Fallback */
    int fallback_ms;
    Profile fallback_profile;

    /* Bitrate control */
    bool allow_xtx_reduce_bitrate;
    float xtx_reduce_bitrate_factor;
    int check_xtx_period_ms;

    /* Keyframe control */
    bool allow_request_keyframe;
    bool allow_rq_kf_by_tx_d;
    int request_keyframe_interval_ms;
    bool idr_every_change;

    /* Camera/display */
    bool limitFPS;
    bool roi_focus_mode;
    bool get_card_info_from_yaml;
    int osd_level;
    float multiply_font_size_by;
    bool verbose_mode;

    /* Command templates */
    char fpsCommandTemplate[150];
    char powerCommandTemplate[100];
    char qpDeltaCommandTemplate[150];
    char mcsCommandTemplate[100];
    char bitrateCommandTemplate[150];
    char gopCommandTemplate[100];
    char fecCommandTemplate[100];
    char roiCommandTemplate[150];
    char idrCommandTemplate[100];

    /* Custom OSD format string */
    char customOSD[64];
} alink_config_t;

/**
 * Initialize config struct with default values.
 */
void config_set_defaults(alink_config_t *cfg);

/**
 * Load configuration from alink.conf file.
 * Calls error_to_osd() and exit() on fatal errors (matching original behavior).
 * Returns 0 on success.
 */
int config_load(alink_config_t *cfg, const char *filename);

/**
 * Update a single parameter in /etc/alink.conf using sed.
 * Returns the system() return value.
 */
int config_update_param(const char *key, const char *value);

#endif /* ALINK_CONFIG_H */
