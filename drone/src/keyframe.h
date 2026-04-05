/**
 * @file keyframe.h
 * @brief Keyframe request deduplication and special command handling.
 *
 * Manages keyframe IDR request codes with deduplication and expiry.
 * Also handles pause/resume special commands. Owns its own mutex
 * (fully encapsulated).
 */
#ifndef ALINK_KEYFRAME_H
#define ALINK_KEYFRAME_H

#include "alink_types.h"
#include "config.h"
#include "command.h"

typedef struct {
    KeyframeRequest codes[MAX_CODES];
    int num_requests;
    int total_requests;
    int total_requests_xtx;
    struct timespec last_request_time;
    pthread_mutex_t mutex;
    log_level_t log_level;
} keyframe_state_t;

void keyframe_init(keyframe_state_t *ks);

/**
 * Process a special command message (keyframe request, pause, resume).
 * The msg parameter should include the "special:" prefix.
 */
void keyframe_handle_special(keyframe_state_t *ks, const char *msg,
                             const alink_config_t *cfg,
                             float prevSetGop,
                             volatile bool *paused,
                             pthread_mutex_t *pause_mutex,
                             const cmd_ctx_t *cmd);

int keyframe_get_total(const keyframe_state_t *ks);
int keyframe_get_total_xtx(const keyframe_state_t *ks);

#endif /* ALINK_KEYFRAME_H */
