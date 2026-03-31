/**
 * @file fallback.c
 * @brief Message counting and fallback trigger thread.
 */
#include "fallback.h"

void *fallback_thread_func(void *arg) {
    fallback_thread_arg_t *ta = (fallback_thread_arg_t *)arg;
    int local_count;

    while (1) {
        usleep(ta->cfg->fallback_ms * 1000);

        pthread_mutex_lock(ta->count_mutex);
        local_count = *(ta->message_count);
        *(ta->message_count) = 0;
        pthread_mutex_unlock(ta->count_mutex);

        pthread_mutex_lock(ta->pause_mutex);
        if (*(ta->initialized) && local_count == 0 && !*(ta->paused)) {
            printf("No messages received in %dms, sending 999\n", ta->cfg->fallback_ms);
            profile_start_selection(ta->ps, FALLBACK_SCORE, SCORE_RANGE_BASE, 0, ta->osd);
        } else {
            if (ta->cfg->verbose_mode) {
                printf("Messages per %dms: %d\n", ta->cfg->fallback_ms, local_count);
            }
        }
        pthread_mutex_unlock(ta->pause_mutex);
    }
    return NULL;
}
