/**
 * @file fallback.c
 * @brief Message counting and fallback trigger thread.
 *
 * When no GS heartbeat is received within fallback_ms, applies the
 * fallback profile defined in alink.conf.
 */
#include "fallback.h"
#include "util.h"

void *fallback_thread_func(void *arg) {
    fallback_thread_arg_t *ta = (fallback_thread_arg_t *)arg;
    int local_count;

    while (1) {
        usleep(ta->cfg->fallback_ms * 1000);

        pthread_mutex_lock(ta->count_mutex);
        local_count = *(ta->message_count);
        *(ta->message_count) = 0;
        pthread_mutex_unlock(ta->count_mutex);

        if (*(ta->initialized) && local_count == 0) {
            INFO_LOG(ta->cfg, "No messages received in %dms, applying fallback profile\n", ta->cfg->fallback_ms);
            /* Bypass profile_apply_direct's duplicate check — fallback must
             * always dispatch so that commands which failed on a previous
             * attempt (e.g. API batch timeout) get retried. Delta detection
             * in profile_apply_exec still skips already-applied params.
             *
             * The three ps fields are guarded by worker_mutex; take it
             * here so we don't race profile_apply's read of the same
             * fields into pending_job. */
            pthread_mutex_lock(&ta->ps->worker_mutex);
            ta->ps->previousProfile = ta->ps->currentProfile;
            ta->ps->currentProfile = -1;
            ta->ps->prevTimeStamp = util_now_ms();
            pthread_mutex_unlock(&ta->ps->worker_mutex);
            profile_apply(ta->ps, &ta->cfg->fallback_profile, ta->osd);
        } else {
            INFO_LOG(ta->cfg, "Messages per %dms: %d\n", ta->cfg->fallback_ms, local_count);
        }
    }
    return NULL;
}
