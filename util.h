/**
 * @file util.h
 * @brief Pure utility functions: string manipulation and time calculations.
 *
 * Stateless helper functions used across multiple modules. Thread-safe
 * (no shared state).
 */
#ifndef ALINK_UTIL_H
#define ALINK_UTIL_H

#include "alink_types.h"
#include <stdint.h>

void util_trim_whitespace(char *str);
void util_normalize_whitespace(char *str);
void util_strip_newline(char *s);
long util_get_monotonic_time(void);
uint64_t util_now_ms(void);
long util_elapsed_ms_timespec(const struct timespec *current, const struct timespec *past);
long util_elapsed_ms_timeval(const struct timeval *current, const struct timeval *past);

#endif /* ALINK_UTIL_H */
