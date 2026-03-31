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

    /* Selection tuning */
    float rssi_weight;
    float snr_weight;
    int hold_fallback_mode_s;
    int hold_modes_down_s;
    int hold_fallback_mode_ms;
    int hold_modes_down_ms;
    int min_between_changes_ms;
    int hysteresis_percent;
    int hysteresis_percent_down;
    float smoothing_factor;
    float smoothing_factor_down;
    float ema_fast_alpha;
    float ema_slow_alpha;
    float predict_multi;
    bool fast_downgrade;
    int upward_confidence_loops;
    int limit_max_score_to;
    int fallback_ms;
    int baseline_value;

    /* FEC control */
    bool allow_dynamic_fec;
    bool fec_k_adjust;
    bool spike_fix_dynamic_fec;
    int fec_reaction_delay_ms;

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

    /* Profiles */
    Profile profiles[MAX_PROFILES];
    int num_profiles;
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
 * Load transmission profiles from txprofiles.conf.
 * Calls error_to_osd() and exit() on fatal errors (matching original behavior).
 * Returns 0 on success.
 */
int config_load_profiles(alink_config_t *cfg, const char *filename);

/**
 * Update a single parameter in /etc/alink.conf using sed.
 * Returns the system() return value.
 */
int config_update_param(const char *key, const char *value);

#endif /* ALINK_CONFIG_H */
