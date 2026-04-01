/**
 * @file rssi_monitor.h
 * @brief Drone antenna RSSI circular queue and monitoring thread.
 *
 * Manages a thread-safe circular queue for antenna stats and a background
 * thread that parses RSSI data and detects weak antennas. Owns its own
 * mutex (fully encapsulated).
 */
#ifndef ALINK_RSSI_MONITOR_H
#define ALINK_RSSI_MONITOR_H

#include "alink_types.h"

typedef struct {
    char queue[MAX_RSSI_QUEUE][MAX_RSSI_LINE];
    int head;
    int tail;
    pthread_mutex_t lock;
    volatile int weak_antenna_detected;
    int num_antennas_drone;
    bool verbose;
} rssi_state_t;

void rssi_init(rssi_state_t *rs, bool verbose);
int  rssi_enqueue(rssi_state_t *rs, const char *line);
void *rssi_thread_func(void *arg);  /* arg = rssi_state_t* */
int  rssi_get_weak_antenna(const rssi_state_t *rs);
int  rssi_get_num_antennas_drone(const rssi_state_t *rs);

#endif /* ALINK_RSSI_MONITOR_H */
