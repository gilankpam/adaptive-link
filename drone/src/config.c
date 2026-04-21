/**
 * @file config.c
 * @brief Configuration loading for alink.conf.
 */
#include "config.h"
#include "util.h"
#include "osd.h"
#include <stddef.h>

#define CUSTOM_OSD_DEFAULT "&L%d0&F%d&B &C tx&Wc"

/* Field types for the table-driven config dispatch. CT_LOGLEVEL maps the
 * three string values "debug"/"info"/"error" to the log_level_t enum;
 * CT_OSDFMT runs customosd_format_is_safe() before storing. */
typedef enum {
    CT_INT,         /* atoi -> int */
    CT_BOOL,        /* atoi -> bool */
    CT_FLOAT,       /* atof -> float */
    CT_STR,         /* strncpy -> char[size] */
    CT_LOGLEVEL,    /* "debug"|"info"|"error" -> log_level_t */
    CT_OSDFMT       /* validated format string */
} cfg_kind_t;

typedef struct {
    const char *key;
    size_t      offset;     /* offsetof(alink_config_t, field) */
    size_t      size;       /* sizeof(field), for CT_STR / CT_OSDFMT */
    cfg_kind_t  kind;
} cfg_entry_t;

/* customOSD is fed to snprintf as the format string with two int args
 * (color and font_size). Reject anything that isn't a literal char, '%%',
 * or one of (%d, %i) — and cap the conversion-specifier count at 2 — so a
 * malformed/malicious config can't introduce %s/%n/%x style format-string
 * vulnerabilities at the osd.c snprintf site. */
static int customosd_format_is_safe(const char *fmt) {
    int spec_count = 0;
    for (const char *p = fmt; *p; p++) {
        if (*p != '%') continue;
        p++;
        if (*p == '%') continue;          /* literal '%' */
        if (*p == 'd' || *p == 'i') {
            if (++spec_count > 2) return 0;
            continue;
        }
        return 0;                          /* %s, %n, %x, %p, ... */
    }
    return 1;
}

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

    cfg->wfb_control_port = 8000;

    cfg->get_card_info_from_yaml = false;
    cfg->osd_level = 4;
    cfg->multiply_font_size_by = 0.5f;
    cfg->log_level = LOG_LEVEL_INFO;
    cfg->roiqp_hi = 11000;
    cfg->roiqp_lo = 2000;
    cfg->roiqp_base = 0;

    strncpy(cfg->customOSD, CUSTOM_OSD_DEFAULT, sizeof(cfg->customOSD) - 1);
    cfg->customOSD[sizeof(cfg->customOSD) - 1] = '\0';
}

/* offsetof helpers for nested fallback_profile fields. Splitting the
 * computation rather than using offsetof(alink_config_t, fallback_profile.x)
 * keeps us inside strictly portable C99. */
#define FB_OFF(field) (offsetof(alink_config_t, fallback_profile) + offsetof(Profile, field))

static const cfg_entry_t CONFIG_KEYS[] = {
    /* Top-level fields */
    { "allow_set_power",             offsetof(alink_config_t, allow_set_power),             0,                                                                CT_BOOL  },
    { "fallback_ms",                 offsetof(alink_config_t, fallback_ms),                 0,                                                                CT_INT   },
    { "request_keyframe_interval_ms",offsetof(alink_config_t, request_keyframe_interval_ms),0,                                                                CT_INT   },
    { "idr_every_change",            offsetof(alink_config_t, idr_every_change),            0,                                                                CT_BOOL  },
    { "allow_request_keyframe",      offsetof(alink_config_t, allow_request_keyframe),      0,                                                                CT_BOOL  },
    { "get_card_info_from_yaml",     offsetof(alink_config_t, get_card_info_from_yaml),     0,                                                                CT_BOOL  },
    { "allow_rq_kf_by_tx_d",         offsetof(alink_config_t, allow_rq_kf_by_tx_d),         0,                                                                CT_BOOL  },
    { "allow_xtx_reduce_bitrate",    offsetof(alink_config_t, allow_xtx_reduce_bitrate),    0,                                                                CT_BOOL  },
    { "xtx_reduce_bitrate_factor",   offsetof(alink_config_t, xtx_reduce_bitrate_factor),   0,                                                                CT_FLOAT },
    { "osd_level",                   offsetof(alink_config_t, osd_level),                   0,                                                                CT_INT   },
    { "multiply_font_size_by",       offsetof(alink_config_t, multiply_font_size_by),       0,                                                                CT_FLOAT },
    { "check_xtx_period_ms",         offsetof(alink_config_t, check_xtx_period_ms),         0,                                                                CT_INT   },
    { "log_level",                   offsetof(alink_config_t, log_level),                   0,                                                                CT_LOGLEVEL },
    { "roiqp_hi",                    offsetof(alink_config_t, roiqp_hi),                    0,                                                                CT_INT   },
    { "roiqp_lo",                    offsetof(alink_config_t, roiqp_lo),                    0,                                                                CT_INT   },
    { "roiqp_base",                  offsetof(alink_config_t, roiqp_base),                  0,                                                                CT_INT   },
    { "wfbControlPort",              offsetof(alink_config_t, wfb_control_port),            0,                                                                CT_INT   },

    /* Fallback profile fields */
    { "fallback_gi",                 FB_OFF(setGI),     sizeof(((Profile*)0)->setGI),                                                                          CT_STR   },
    { "fallback_mcs",                FB_OFF(setMCS),    0,                                                                                                     CT_INT   },
    { "fallback_feck",               FB_OFF(setFecK),   0,                                                                                                     CT_INT   },
    { "fallback_fecn",               FB_OFF(setFecN),   0,                                                                                                     CT_INT   },
    { "fallback_bitrate",            FB_OFF(setBitrate),0,                                                                                                     CT_INT   },
    { "fallback_gop",                FB_OFF(setGop),    0,                                                                                                     CT_FLOAT },
    { "fallback_power",              FB_OFF(wfbPower),  0,                                                                                                     CT_INT   },
    { "fallback_bandwidth",          FB_OFF(bandwidth), 0,                                                                                                     CT_INT   },

    /* Command templates */
    { "powerCommandTemplate",        offsetof(alink_config_t, powerCommandTemplate),        sizeof(((alink_config_t*)0)->powerCommandTemplate),               CT_STR   },
    { "idrApiCommandTemplate",       offsetof(alink_config_t, idrApiCommandTemplate),       sizeof(((alink_config_t*)0)->idrApiCommandTemplate),              CT_STR   },
    { "apiCommandTemplate",          offsetof(alink_config_t, apiCommandTemplate),          sizeof(((alink_config_t*)0)->apiCommandTemplate),                 CT_STR   },
    { "customOSD",                   offsetof(alink_config_t, customOSD),                   sizeof(((alink_config_t*)0)->customOSD),                          CT_OSDFMT },
};
#define CONFIG_KEYS_COUNT (sizeof(CONFIG_KEYS) / sizeof(CONFIG_KEYS[0]))

static void apply_config_entry(alink_config_t *cfg, const cfg_entry_t *e, const char *value) {
    void *field = (char *)cfg + e->offset;
    switch (e->kind) {
        case CT_INT:
            *(int *)field = atoi(value);
            break;
        case CT_BOOL:
            *(bool *)field = atoi(value) ? true : false;
            break;
        case CT_FLOAT:
            *(float *)field = (float)atof(value);
            break;
        case CT_STR:
            strncpy((char *)field, value, e->size - 1);
            ((char *)field)[e->size - 1] = '\0';
            break;
        case CT_LOGLEVEL:
            if      (strcmp(value, "debug") == 0) *(log_level_t *)field = LOG_LEVEL_DEBUG;
            else if (strcmp(value, "info")  == 0) *(log_level_t *)field = LOG_LEVEL_INFO;
            else if (strcmp(value, "error") == 0) *(log_level_t *)field = LOG_LEVEL_ERROR;
            break;
        case CT_OSDFMT:
            if (customosd_format_is_safe(value)) {
                strncpy((char *)field, value, e->size - 1);
                ((char *)field)[e->size - 1] = '\0';
            } else {
                fprintf(stderr, "Error: customOSD contains unsafe format "
                        "specifier; falling back to default. Value: %s\n", value);
                strncpy((char *)field, CUSTOM_OSD_DEFAULT, e->size - 1);
                ((char *)field)[e->size - 1] = '\0';
            }
            break;
    }
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

        char *saveptr = NULL;
        char *key = strtok_r(line, "=", &saveptr);
        char *value = strtok_r(NULL, "\n", &saveptr);

        if (key && value) {
            /* Strip surrounding quotes from value */
            size_t vlen = strlen(value);
            if (vlen >= 2 && value[0] == '"' && value[vlen - 1] == '"') {
                value[vlen - 1] = '\0';
                value++;
            }

            const cfg_entry_t *match = NULL;
            for (size_t i = 0; i < CONFIG_KEYS_COUNT; i++) {
                if (strcmp(key, CONFIG_KEYS[i].key) == 0) {
                    match = &CONFIG_KEYS[i];
                    break;
                }
            }
            if (match) {
                apply_config_entry(cfg, match, value);
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
