/**
 * @file util.c
 * @brief Pure utility functions: string manipulation and time calculations.
 */
#include "util.h"

void util_strip_newline(char *s) {
    size_t l = strlen(s);
    if (l > 0 && s[l-1] == '\n') s[l-1] = '\0';
}

void util_trim_whitespace(char *str) {
    char *end;

    /* Trim leading spaces */
    while (isspace((unsigned char)*str)) str++;

    if (*str == 0) return; /* Empty string */

    /* Trim trailing spaces */
    end = str + strlen(str) - 1;
    while (end > str && isspace((unsigned char)*end)) end--;

    /* Null-terminate the trimmed string */
    *(end + 1) = '\0';
}

void util_normalize_whitespace(char *str) {
    char *src = str, *dst = str;
    int in_space = 0;

    while (*src) {
        if (isspace((unsigned char)*src)) {
            if (!in_space) {
                *dst++ = ' ';
                in_space = 1;
            }
        } else {
            *dst++ = *src;
            in_space = 0;
        }
        src++;
    }
    *dst = '\0';
}

long util_get_monotonic_time(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec;
}

uint64_t util_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

long util_elapsed_ms_timespec(const struct timespec *current, const struct timespec *past) {
    long long sec_diff = (long long)current->tv_sec - past->tv_sec;
    long long nsec_diff = (long long)current->tv_nsec - past->tv_nsec;

    if (sec_diff > LONG_MAX / 1000) {
        return LONG_MAX;
    }

    long long elapsed_ms = sec_diff * 1000 + nsec_diff / 1000000;

    if (elapsed_ms < 0) {
        return 0;
    }

    return (elapsed_ms > LONG_MAX) ? LONG_MAX : (long)elapsed_ms;
}

long util_elapsed_ms_timeval(const struct timeval *current, const struct timeval *past) {
    long long sec_diff = (long long)current->tv_sec - past->tv_sec;
    long long usec_diff = (long long)current->tv_usec - past->tv_usec;

    if (sec_diff > LONG_MAX / 1000) {
        return LONG_MAX;
    }

    long long elapsed_ms = sec_diff * 1000 + usec_diff / 1000;

    if (elapsed_ms < 0) {
        return 0;
    }

    return (elapsed_ms > LONG_MAX) ? LONG_MAX : (long)elapsed_ms;
}
