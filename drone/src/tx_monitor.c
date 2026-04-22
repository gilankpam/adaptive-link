/**
 * @file tx_monitor.c
 * @brief TX drop monitoring thread.
 */
#include "tx_monitor.h"
#include "util.h"
#include "command.h"
#include "profile.h"
#include <string.h>

void *txmon_thread_func(void *arg) {
    txmon_thread_arg_t *ta = (txmon_thread_arg_t *)arg;
    profile_state_t *ps = ta->ps;
    keyframe_state_t *ks = ta->ks;
    hw_state_t *hw = ta->hw;
    alink_config_t *cfg = ta->cfg;
    cmd_ctx_t *cmd = ta->cmd;

    const char *idrCommand = cfg->idrApiCommandTemplate;
    const long restore_interval_ms = 1000;

    struct timespec last_xtx_time = {0, 0};

    /* Wait until initialized */
    while (!*(ta->initialized)) {
        sleep(1);
    }

    while (1) {
        long latest_tx_dropped = hw_get_tx_dropped(hw);

        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);

        long since_xtx_ms = util_elapsed_ms_timespec(&now, &last_xtx_time);

        /* Snapshot ps fields under worker_mutex so we don't race the profile
         * worker updating prevApplied/bitrate_reduced. The snapshot is also
         * written back under the same lock. The actual HTTP call is released
         * from this lock — cmd->exec_mutex serialises HTTP/shell ops across
         * threads; cmd->wfb_mutex is independent. */
        pthread_mutex_lock(&ps->worker_mutex);
        int snap_prev_bitrate = ps->prevApplied.setBitrate;
        float snap_prev_gop = ps->prevApplied.setGop;
        bool snap_bitrate_reduced = ps->bitrate_reduced;
        pthread_mutex_unlock(&ps->worker_mutex);

        /* If we see dropped-tx, reduce bitrate (once) and reset timer */
        if (cfg->allow_xtx_reduce_bitrate && latest_tx_dropped > 0) {
            if (!snap_bitrate_reduced) {
                int new_bitrate = (int)(snap_prev_bitrate * cfg->xtx_reduce_bitrate_factor);
                /* Apply bitrate via batched API */
                profile_apply_api_batch(cfg, new_bitrate, snap_prev_gop, cmd);
                pthread_mutex_lock(&ps->worker_mutex);
                ps->bitrate_reduced = true;
                pthread_mutex_unlock(&ps->worker_mutex);
                INFO_LOG(cfg, "Reduced bitrate due to tx-drops\n");
            }
            last_xtx_time = now;
        }

        /* If we've reduced, but no new tx-drops for >= restore_interval_ms, restore */
        else if (snap_bitrate_reduced && since_xtx_ms >= restore_interval_ms) {
            /* Apply bitrate via batched API */
            profile_apply_api_batch(cfg, snap_prev_bitrate, snap_prev_gop, cmd);
            pthread_mutex_lock(&ps->worker_mutex);
            ps->bitrate_reduced = false;
            pthread_mutex_unlock(&ps->worker_mutex);
            INFO_LOG(cfg, "Restored normal bitrate after %ld ms without tx-drops\n",
                   since_xtx_ms);
        }

        /* Check and claim the keyframe rate-limit slot under ks->mutex so
         * GS-initiated and tx-drop-initiated paths can't race. */
        pthread_mutex_lock(&ks->mutex);
        long elapsed_kf_ms = util_elapsed_ms_timespec(&now, &ks->last_request_time);
        bool fire_kf = latest_tx_dropped > 0
                       && elapsed_kf_ms >= cfg->request_keyframe_interval_ms
                       && cfg->allow_rq_kf_by_tx_d
                       && snap_prev_gop > 0.5f;
        if (fire_kf) {
            ks->last_request_time = now;
        }
        pthread_mutex_unlock(&ks->mutex);

        if (fire_kf) {
            /* Templates are path-only; majestic is always on localhost:80.
             * Accept legacy http://host/... prefix for backward compat. */
            const char *url_path = idrCommand;
            if (strncmp(url_path, "http://", 7) == 0) {
                const char *slash = strchr(url_path + 7, '/');
                url_path = slash ? slash : "/";
            }

            if (cmd_http_get("localhost", 80, url_path, NULL, 0, cmd) != 0) {
                ERROR_LOG(cfg, "IDR request failed: %s\n", url_path);
            }

            INFO_LOG(cfg, "Requesting keyframe for locally dropped tx packet\n");
        }

        usleep(cfg->check_xtx_period_ms * 1000);
    }
    return NULL;
}
