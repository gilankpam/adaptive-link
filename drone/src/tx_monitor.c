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

        /* If we see dropped-tx, reduce bitrate (once) and reset timer */
        if (cfg->allow_xtx_reduce_bitrate && latest_tx_dropped > 0) {
            if (!ps->bitrate_reduced) {
                int new_bitrate = (int)(ps->prevSetBitrate * cfg->xtx_reduce_bitrate_factor);
                /* Apply bitrate via batched API */
                profile_apply_api_batch(cfg, new_bitrate, ps->prevSetGop, cmd);
                ps->bitrate_reduced = true;
                if (cfg->verbose_mode)
                    printf("Reduced bitrate due to tx-drops\n");
            }
            last_xtx_time = now;
        }

        /* If we've reduced, but no new tx-drops for >= restore_interval_ms, restore */
        else if (ps->bitrate_reduced && since_xtx_ms >= restore_interval_ms) {
            /* Apply bitrate via batched API */
            profile_apply_api_batch(cfg, (int)ps->prevSetBitrate, ps->prevSetGop, cmd);
            ps->bitrate_reduced = false;
            if (cfg->verbose_mode)
                printf("Restored normal bitrate after %ld ms without tx-drops\n",
                       since_xtx_ms);
        }

        long elapsed_kf_ms = util_elapsed_ms_timespec(&now, &ks->last_request_time);

        if (latest_tx_dropped > 0 && elapsed_kf_ms >= cfg->request_keyframe_interval_ms && cfg->allow_rq_kf_by_tx_d && ps->prevSetGop > 0.5f) {
            /* Parse the IDR command URL and use native HTTP client */
            char host[64];
            int port = 80;
            char url_path[BUFFER_SIZE];
            
            if (util_parse_url(idrCommand, host, sizeof(host), &port, url_path, sizeof(url_path)) != 0) {
                printf("Failed to parse IDR URL: %s\n", idrCommand);
            } else {
                if (cmd_http_get(host, port, url_path, NULL, 0, cmd) != 0) {
                    printf("IDR request failed: %s:%d%s\n", host, port, url_path);
                }
            }
            
            ks->last_request_time = now;
            ks->total_requests_xtx++;
            if (cfg->verbose_mode)
                printf("Requesting keyframe for locally dropped tx packet\n");
        }

        usleep(cfg->check_xtx_period_ms * 1000);
    }
    return NULL;
}
