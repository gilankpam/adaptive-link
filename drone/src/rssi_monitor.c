/**
 * @file rssi_monitor.c
 * @brief Drone antenna RSSI circular queue and monitoring thread.
 */
#include "rssi_monitor.h"

void rssi_init(rssi_state_t *rs, bool verbose) {
    memset(rs, 0, sizeof(*rs));
    pthread_mutex_init(&rs->lock, NULL);
    rs->verbose = verbose;
}

int rssi_enqueue(rssi_state_t *rs, const char *line) {
    pthread_mutex_lock(&rs->lock);
    int next_tail = (rs->tail + 1) % MAX_RSSI_QUEUE;
    if (next_tail == rs->head) {
        pthread_mutex_unlock(&rs->lock);
        return -1; /* Queue full */
    }
    strncpy(rs->queue[rs->tail], line, MAX_RSSI_LINE - 1);
    rs->queue[rs->tail][MAX_RSSI_LINE - 1] = '\0';
    rs->tail = next_tail;
    pthread_mutex_unlock(&rs->lock);
    return 0;
}

static int dequeue_rssi_line(rssi_state_t *rs, char *line_out) {
    pthread_mutex_lock(&rs->lock);
    if (rs->head == rs->tail) {
        pthread_mutex_unlock(&rs->lock);
        return 0; /* Queue empty */
    }
    strncpy(line_out, rs->queue[rs->head], MAX_RSSI_LINE);
    rs->head = (rs->head + 1) % MAX_RSSI_QUEUE;
    pthread_mutex_unlock(&rs->lock);
    return 1;
}

void *rssi_thread_func(void *arg) {
    rssi_state_t *rs = (rssi_state_t *)arg;

    const int MAX_LINE = 512;

    int rssi_history[NUM_ANTENNA_SLOTS][RSSI_HISTORY_SIZE] = {{0}};
    int rssi_index[NUM_ANTENNA_SLOTS] = {0};
    int rssi_avg[NUM_ANTENNA_SLOTS] = {0};
    int rssi_count[NUM_ANTENNA_SLOTS] = {0};
    /* Running sum per antenna so the rolling average is O(1) per sample —
     * subtract the slot we're about to overwrite (only once the ring is
     * full) and add the new value, instead of re-summing the whole ring. */
    int rssi_sum[NUM_ANTENNA_SLOTS] = {0};

    char line[MAX_LINE];
    while (1) {
        if (!dequeue_rssi_line(rs, line)) {
            usleep(10000);
            continue;
        }

        if (rs->verbose && strstr(line, "RX_ANT")) {
            printf("RX_ANT received: %s\n", line);
        }

        if (strstr(line, "RX_ANT")) {
            char freq_mcs_band[64], colon_values[128];
            int antenna, timestamp;

            if (sscanf(line, "%d RX_ANT %63s %d %127[^\n]", &timestamp, freq_mcs_band, &antenna, colon_values) == 4) {
                if (antenna < 0 || antenna >= NUM_ANTENNA_SLOTS) continue;
                if (antenna >= rs->num_antennas_drone) {
                    rs->num_antennas_drone = antenna + 1;
                }

                char *saveptr = NULL;
                char *token;
                int token_count = 0, rssi = 0;
                token = strtok_r(colon_values, ":", &saveptr);
                while (token) {
                    if (++token_count == 3) {
                        rssi = atoi(token);
                        break;
                    }
                    token = strtok_r(NULL, ":", &saveptr);
                }

                int slot = rssi_index[antenna] % RSSI_HISTORY_SIZE;
                if (rssi_count[antenna] >= RSSI_HISTORY_SIZE) {
                    /* Ring is full — evict the slot we're about to overwrite. */
                    rssi_sum[antenna] -= rssi_history[antenna][slot];
                } else {
                    rssi_count[antenna]++;
                }
                rssi_history[antenna][slot] = rssi;
                rssi_sum[antenna] += rssi;
                rssi_index[antenna]++;
                rssi_avg[antenna] = rssi_sum[antenna] / rssi_count[antenna];

                int min_rssi = INT_MAX, max_rssi = INT_MIN;
                for (int i = 0; i < NUM_ANTENNA_SLOTS; i++) {
                    if (rssi_count[i] > 0) {
                        if (rssi_avg[i] < min_rssi) min_rssi = rssi_avg[i];
                        if (rssi_avg[i] > max_rssi) max_rssi = rssi_avg[i];
                    }
                }
                rs->weak_antenna_detected = (max_rssi - min_rssi >= ANTENNA_SPREAD_THRESHOLD_DB) ? 1 : 0;
            }
        }
    }

    pthread_exit(NULL);
}

int rssi_get_weak_antenna(const rssi_state_t *rs) {
    return rs->weak_antenna_detected;
}

int rssi_get_num_antennas_drone(const rssi_state_t *rs) {
    return rs->num_antennas_drone;
}
