/**
 * @file tx_monitor.c
 * @brief TX drop monitoring thread.
 */
#include "tx_monitor.h"
#include "util.h"

void *txmon_thread_func(void *arg) {
    txmon_thread_arg_t *ta = (txmon_thread_arg_t *)arg;
    profile_state_t *ps = ta->ps;
    keyframe_state_t *ks = ta->ks;
    hw_state_t *hw = ta->hw;
    alink_config_t *cfg = ta->cfg;

    const char *idrCommand = cfg->idrCommandTemplate;
    char roiCommand[MAX_COMMAND_SIZE];
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

        /* Disable ROI to help mitigate spikes */
        if (cfg->roi_focus_mode && latest_tx_dropped > 0 && strcmp(ps->prevROIqp, "0,0,0,0") != 0) {
            const char *keys[] = { "roiQp" };
            const char *values[] = { "0,0,0,0" };
            cmd_format(roiCommand, sizeof(roiCommand), cfg->roiCommandTemplate, 1, keys, values);
            cmd_exec(ta->cmd, roiCommand);
            strcpy(ps->prevROIqp, "0,0,0,0");
        }

        /* If we see dropped-tx, reduce bitrate (once) and reset timer */
        if (cfg->allow_xtx_reduce_bitrate && latest_tx_dropped > 0) {
            if (!ps->bitrate_reduced) {
                profile_apply_fec_bitrate(ps, ps->prevSetFecK,
                                       ps->prevSetFecN,
                                       (int)(ps->prevSetBitrate * cfg->xtx_reduce_bitrate_factor));
                ps->bitrate_reduced = true;
                if (cfg->verbose_mode)
                    printf("Reduced bitrate due to tx-drops\n");
            }
            last_xtx_time = now;
        }

        /* If we've reduced, but no new tx-drops for >= restore_interval_ms, restore */
        else if (ps->bitrate_reduced && since_xtx_ms >= restore_interval_ms) {
            profile_apply_fec_bitrate(ps, ps->prevSetFecK,
                                   ps->prevSetFecN,
                                   (int)ps->prevSetBitrate);
            ps->bitrate_reduced = false;
            if (cfg->verbose_mode)
                printf("Restored normal bitrate after %ld ms without tx-drops\n",
                       since_xtx_ms);
        }

        long elapsed_kf_ms = util_elapsed_ms_timespec(&now, &ks->last_request_time);

        if (latest_tx_dropped > 0 && elapsed_kf_ms >= cfg->request_keyframe_interval_ms && cfg->allow_rq_kf_by_tx_d && ps->prevSetGop > 0.5f) {
            char quotedCommand[BUFFER_SIZE];
            snprintf(quotedCommand, sizeof(quotedCommand), "\"%s\"", idrCommand);
            if (system(quotedCommand) != 0)
                printf("Command failed: %s\n", quotedCommand);
            ks->last_request_time = now;
            ks->total_requests_xtx++;
            if (cfg->verbose_mode)
                printf("Requesting keyframe for locally dropped tx packet\n");
        }

        usleep(cfg->check_xtx_period_ms * 1000);
    }
    return NULL;
}
