/**
 * @file keyframe.c
 * @brief Keyframe request deduplication and special command handling.
 */
#include "keyframe.h"
#include "util.h"
#include "command.h"
#include <string.h>

void keyframe_init(keyframe_state_t *ks) {
    memset(ks, 0, sizeof(*ks));
    pthread_mutex_init(&ks->mutex, NULL);
}

static bool code_exists(keyframe_state_t *ks, const char *code, struct timespec *current_time) {
    pthread_mutex_lock(&ks->mutex);
    for (int i = 0; i < ks->num_requests; i++) {
        if (strcmp(ks->codes[i].code, code) == 0) {
            long elapsed_time_ms = util_elapsed_ms_timespec(current_time, &ks->codes[i].timestamp);
            if (elapsed_time_ms < EXPIRY_TIME_MS) {
                pthread_mutex_unlock(&ks->mutex);
                return true;
            } else {
                memmove(&ks->codes[i], &ks->codes[i + 1],
                        (ks->num_requests - i - 1) * sizeof(KeyframeRequest));
                ks->num_requests--;
                i--;
                pthread_mutex_unlock(&ks->mutex);
                return false;
            }
        }
    }
    pthread_mutex_unlock(&ks->mutex);
    return false;
}

static void add_code(keyframe_state_t *ks, const char *code, struct timespec *current_time) {
    pthread_mutex_lock(&ks->mutex);
    if (ks->num_requests < MAX_CODES) {
        strncpy(ks->codes[ks->num_requests].code, code, CODE_LENGTH);
        ks->codes[ks->num_requests].timestamp = *current_time;
        ks->num_requests++;
    } else {
        ERROR_LOG_LEVEL(ks->log_level, "Max keyframe request codes reached. Consider increasing MAX_CODES.\n");
    }
    pthread_mutex_unlock(&ks->mutex);
}

static void cleanup_expired_codes(keyframe_state_t *ks, struct timespec *current_time) {
    pthread_mutex_lock(&ks->mutex);

    if (ks->num_requests <= 0) {
        pthread_mutex_unlock(&ks->mutex);
        return;
    }

    int most_recent_idx = 0;
    long min_elapsed = util_elapsed_ms_timespec(current_time, &ks->codes[0].timestamp);
    for (int i = 1; i < ks->num_requests; i++) {
        long elapsed = util_elapsed_ms_timespec(current_time, &ks->codes[i].timestamp);
        if (elapsed < min_elapsed) {
            min_elapsed = elapsed;
            most_recent_idx = i;
        }
    }

    for (int i = 0; i < ks->num_requests; ) {
        if (i == most_recent_idx) {
            i++;
            continue;
        }
        long elapsed_time_ms = util_elapsed_ms_timespec(current_time, &ks->codes[i].timestamp);
        INFO_LOG_LEVEL(ks->log_level, "Keyframe request code: %s, Elapsed time: %ld ms\n", ks->codes[i].code, elapsed_time_ms);
        if (elapsed_time_ms >= EXPIRY_TIME_MS) {
            memmove(&ks->codes[i], &ks->codes[i + 1],
                    (ks->num_requests - i - 1) * sizeof(KeyframeRequest));
            ks->num_requests--;
            if (i < most_recent_idx) {
                most_recent_idx--;
            }
        } else {
            i++;
        }
    }
    pthread_mutex_unlock(&ks->mutex);
}

void keyframe_handle_special(keyframe_state_t *ks, const char *msg,
                             const alink_config_t *cfg,
                             float prevSetGop,
                             volatile bool *paused,
                             pthread_mutex_t *pause_mutex,
                             const cmd_ctx_t *cmd) {
    const char *cleaned_msg = msg + 8;  /* Skip "special:" */
    const char *idrApiCommand = cfg->idrApiCommandTemplate;

    /* We need a mutable copy to split at ':' */
    char msg_buf[256];
    strncpy(msg_buf, cleaned_msg, sizeof(msg_buf) - 1);
    msg_buf[sizeof(msg_buf) - 1] = '\0';

    char *separator = strchr(msg_buf, ':');
    char code[CODE_LENGTH] = {0};

    if (separator) {
        *separator = '\0';
        strncpy(code, separator + 1, CODE_LENGTH - 1);
    }

    if (cfg->allow_request_keyframe && prevSetGop > 0.5f && strcmp(msg_buf, "request_keyframe") == 0 && code[0] != '\0') {
        struct timespec current_time;
        clock_gettime(CLOCK_MONOTONIC, &current_time);

        cleanup_expired_codes(ks, &current_time);

        long elapsed_ms = util_elapsed_ms_timespec(&current_time, &ks->last_request_time);

        if (elapsed_ms >= cfg->request_keyframe_interval_ms) {
            if (!code_exists(ks, code, &current_time)) {
                add_code(ks, code, &current_time);

                /* Parse the IDR API URL and use native HTTP client */
                char host[64];
                int port = 80;
                char url_path[BUFFER_SIZE];
                
                if (util_parse_url(idrApiCommand, host, sizeof(host), &port, url_path, sizeof(url_path)) != 0) {
                    ERROR_LOG(cfg, "Failed to parse IDR API URL: %s\n", idrApiCommand);
                } else {
                    if (cmd_http_get(host, port, url_path, NULL, 0, cmd) != 0) {
                        ERROR_LOG(cfg, "IDR API request failed: %s:%d%s\n", host, port, url_path);
                    }
                }
                ks->last_request_time = current_time;
                ks->total_requests++;
            } else {
                INFO_LOG(cfg, "Already requested keyframe for code: %s\n", code);
            }
        } else {
            INFO_LOG(cfg, "Keyframe request ignored. Interval  %ld not met for code: %s\n", elapsed_ms, code);
        }

    } else if (strcmp(msg_buf, "pause_adaptive") == 0) {
        pthread_mutex_lock(pause_mutex);
        *paused = true;
        pthread_mutex_unlock(pause_mutex);
        INFO_LOG(cfg, "Paused adaptive mode\n");

    } else if (strcmp(msg_buf, "resume_adaptive") == 0) {
        pthread_mutex_lock(pause_mutex);
        *paused = false;
        pthread_mutex_unlock(pause_mutex);
        INFO_LOG(cfg, "Resumed adaptive mode\n");

    } else {
        ERROR_LOG(cfg, "Unknown or disabled special command: %s\n", msg_buf);
    }
}

int keyframe_get_total(const keyframe_state_t *ks) {
    return ks->total_requests;
}

int keyframe_get_total_xtx(const keyframe_state_t *ks) {
    return ks->total_requests_xtx;
}
