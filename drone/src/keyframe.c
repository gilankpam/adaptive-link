/**
 * @file keyframe.c
 * @brief Keyframe request rate limiting.
 */
#include "keyframe.h"
#include "hardware.h"
#include "util.h"
#include "command.h"
#include <string.h>

void keyframe_init(keyframe_state_t *ks) {
    memset(ks, 0, sizeof(*ks));
    pthread_mutex_init(&ks->mutex, NULL);
}

bool keyframe_fire_request(keyframe_state_t *ks, const alink_config_t *cfg,
                           const cmd_ctx_t *cmd) {
    if (!cfg->allow_request_keyframe) {
        return false;
    }

    struct timespec current_time;
    clock_gettime(CLOCK_MONOTONIC, &current_time);

    pthread_mutex_lock(&ks->mutex);
    long elapsed_ms = util_elapsed_ms_timespec(&current_time, &ks->last_request_time);
    bool allowed = elapsed_ms >= cfg->request_keyframe_interval_ms;
    if (allowed) {
        ks->last_request_time = current_time;
        ks->total_requests++;
    }
    pthread_mutex_unlock(&ks->mutex);

    if (!allowed) {
        INFO_LOG(cfg, "Keyframe request throttled (%ld ms since last)\n", elapsed_ms);
        return false;
    }

    if (cmd_http_get(VENC_API_HOST, VENC_API_PORT, cfg->idrApiCommandTemplate, NULL, 0, cmd) != 0) {
        ERROR_LOG(cfg, "IDR API request failed: %s\n", cfg->idrApiCommandTemplate);
        return false;
    }
    return true;
}
