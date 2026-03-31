/**
 * @file fallback.h
 * @brief Message counting and fallback trigger thread.
 *
 * Periodically checks if ground station heartbeats have been received.
 * Triggers fallback profile selection when no messages arrive within
 * the configured timeout.
 */
#ifndef ALINK_FALLBACK_H
#define ALINK_FALLBACK_H

#include "alink_types.h"
#include "config.h"
#include "profile.h"
#include "osd.h"

typedef struct {
    profile_state_t *ps;
    osd_state_t *osd;
    alink_config_t *cfg;
    pthread_mutex_t *count_mutex;
    pthread_mutex_t *pause_mutex;
    volatile int *message_count;
    volatile bool *paused;
    volatile bool *initialized;
} fallback_thread_arg_t;

void *fallback_thread_func(void *arg);

#endif /* ALINK_FALLBACK_H */
