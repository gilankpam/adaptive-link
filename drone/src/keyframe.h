/**
 * @file keyframe.h
 * @brief Keyframe request rate limiting.
 *
 * Gates keyframe IDR HTTP requests to the camera API behind a single
 * rate limit (request_keyframe_interval_ms) shared by GS-initiated K
 * messages and drone-initiated tx-drop triggers. Owns its own mutex
 * (fully encapsulated).
 */
#ifndef ALINK_KEYFRAME_H
#define ALINK_KEYFRAME_H

#include "alink_types.h"
#include "config.h"
#include "command.h"

typedef struct {
    int total_requests;
    struct timespec last_request_time;
    pthread_mutex_t mutex;
    log_level_t log_level;
} keyframe_state_t;

void keyframe_init(keyframe_state_t *ks);

/**
 * Request a keyframe from the camera API, rate-limited by
 * cfg->request_keyframe_interval_ms against ks->last_request_time.
 * Returns true if the HTTP call was issued, false if throttled.
 */
bool keyframe_fire_request(keyframe_state_t *ks, const alink_config_t *cfg,
                           const cmd_ctx_t *cmd);

#endif /* ALINK_KEYFRAME_H */
