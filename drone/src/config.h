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
    int roiqp_hi;
    int roiqp_lo;
    int roiqp_base;
    bool get_card_info_from_yaml;
    int osd_level;
    float multiply_font_size_by;
    log_level_t log_level;

    /* Command templates */
    char fpsCommandTemplate[150];
    char powerCommandTemplate[100];
    char mcsCommandTemplate[100];
    char fecCommandTemplate[100];
    /* IDR API command template - uses native HTTP client (no curl) */
    char idrApiCommandTemplate[100];

    /* Batched API command template - combines bitrate, gop, and drone-computed roiQp */
    /* Placeholders: {bitrate}, {gop}, {roiQp} */
    char apiCommandTemplate[256];

    /* Custom OSD format string */
    char customOSD[64];
} alink_config_t;

/**
 * Print logs based on severity levels.
 */
#define DEBUG_LOG_LEVEL(level, fmt, ...) do { \
    if ((level) >= LOG_LEVEL_DEBUG) { \
        struct timespec _ts; \
        clock_gettime(CLOCK_REALTIME, &_ts); \
        struct tm _tm; \
        localtime_r(&_ts.tv_sec, &_tm); \
        printf("[DEBUG %02d:%02d:%02d.%03ld] " fmt, \
               _tm.tm_hour, _tm.tm_min, _tm.tm_sec, \
               _ts.tv_nsec / 1000000, ##__VA_ARGS__); \
    } \
} while (0)

#define INFO_LOG_LEVEL(level, fmt, ...) do { \
    if ((level) >= LOG_LEVEL_INFO) { \
        printf("[INFO] " fmt, ##__VA_ARGS__); \
    } \
} while (0)

#define ERROR_LOG_LEVEL(level, fmt, ...) do { \
    if ((level) >= LOG_LEVEL_ERROR) { \
        fprintf(stderr, "[ERROR] " fmt, ##__VA_ARGS__); \
    } \
} while (0)

#define DEBUG_LOG(cfg, fmt, ...) DEBUG_LOG_LEVEL((cfg)->log_level, fmt, ##__VA_ARGS__)
#define INFO_LOG(cfg, fmt, ...)  INFO_LOG_LEVEL((cfg)->log_level, fmt, ##__VA_ARGS__)
#define ERROR_LOG(cfg, fmt, ...) ERROR_LOG_LEVEL((cfg)->log_level, fmt, ##__VA_ARGS__)
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

#endif /* ALINK_CONFIG_H */
