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
    char *start = str;

    /* Trim leading spaces */
    while (isspace((unsigned char)*str)) str++;

    if (*str == 0) {
        *start = '\0'; /* Empty string */
        return;
    }

    /* Trim trailing spaces */
    end = str + strlen(str) - 1;
    while (end > str && isspace((unsigned char)*end)) end--;

    /* Null-terminate the trimmed string */
    *(end + 1) = '\0';

    /* Shift the trimmed string back to the original position */
    if (str != start) {
        memmove(start, str, (size_t)(end - str + 2));
    }
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

    /* Check for overflow: sec_diff * 1000 could overflow long long */
    /* LONG_MAX is about 9.2e18, so sec_diff > 9.2e15 would overflow */
    if (sec_diff > 9223372036854775LL) {
        return LONG_MAX;
    }
    if (sec_diff < -9223372036854775LL) {
        return 0;
    }

    long long elapsed_ms = sec_diff * 1000 + nsec_diff / 1000000;

    if (elapsed_ms < 0) {
        return 0;
    }

    if (elapsed_ms > LONG_MAX) {
        return LONG_MAX;
    }

    return (long)elapsed_ms;
}

long util_elapsed_ms_timeval(const struct timeval *current, const struct timeval *past) {
    long long sec_diff = (long long)current->tv_sec - past->tv_sec;
    long long usec_diff = (long long)current->tv_usec - past->tv_usec;

    /* Check for overflow: sec_diff * 1000 could overflow long long */
    /* LONG_MAX is about 9.2e18, so sec_diff > 9.2e15 would overflow */
    if (sec_diff > 9223372036854775LL) {
        return LONG_MAX;
    }
    if (sec_diff < -9223372036854775LL) {
        return 0;
    }

    long long elapsed_ms = sec_diff * 1000 + usec_diff / 1000;

    if (elapsed_ms < 0) {
        return 0;
    }

    if (elapsed_ms > LONG_MAX) {
        return LONG_MAX;
    }

    return (long)elapsed_ms;
}

int util_parse_url(const char *url, char *host, size_t host_size,
                   int *port, char *path, size_t path_size) {
    if (!url || !host || !port || !path) {
        return -1;
    }

    /* Set defaults */
    strncpy(host, "localhost", host_size - 1);
    host[host_size - 1] = '\0';
    *port = 80;
    path[0] = '\0';

    char *p = (char *)url;

    /* Check for https:// prefix */
    if (strncmp(p, "https://", 8) == 0) {
        *port = 443;
        p += 8;
    } else if (strncmp(p, "http://", 7) == 0) {
        *port = 80;
        p += 7;
    }

    /* Handle path-only URLs (e.g., "/path" without host) */
    if (*p == '/') {
        strncpy(path, p, path_size - 1);
        path[path_size - 1] = '\0';
        return 0;
    }

    /* Find end of host (colon, slash, or end of string) */
    const char *colon = strchr(p, ':');
    const char *slash = strchr(p, '/');

    /* Determine where the host ends */
    const char *host_end = NULL;
    if (colon && (!slash || colon < slash)) {
        host_end = colon;
    } else if (slash) {
        host_end = slash;
    }

    /* Extract host */
    if (host_end) {
        size_t host_len = host_end - p;
        if (host_len >= host_size) host_len = host_size - 1;
        strncpy(host, p, host_len);
        host[host_len] = '\0';
    } else {
        strncpy(host, p, host_size - 1);
        host[host_size - 1] = '\0';
        return 0; /* No path */
    }

    /* Extract port if specified */
    if (colon && (!slash || colon < slash)) {
        const char *port_end = slash ? slash : p + strlen(p);
        int port_len = (int)(port_end - (colon + 1));
        if (port_len > 0 && port_len <= 5) {
            char port_str[6] = {0};
            strncpy(port_str, (char *)(colon + 1), port_len);
            *port = atoi(port_str);
            if (*port <= 0 || *port > 65535) {
                *port = 80; /* Invalid port, use default */
            }
        }
        p = (char *)(colon + 1 + port_len);
    } else {
        p = (char *)host_end;
    }

    /* Extract path */
    if (*p == '/') {
        strncpy(path, p, path_size - 1);
        path[path_size - 1] = '\0';
    } else if (*p != '\0') {
        /* No leading slash, prepend it */
        if (strlen(p) + 1 < path_size) {
            path[0] = '/';
            strncpy(path + 1, p, path_size - 2);
            path[path_size - 1] = '\0';
        }
    }

    return 0;
}

int util_venc_parse_int_value(const char *json, int *out) {
    const char *p = strstr(json, "\"value\":");
    if (!p) return -1;
    p += 8; /* skip past "value": */
    while (*p == ' ') p++;
    char *endptr;
    long v = strtol(p, &endptr, 10);
    if (endptr == p) return -1;
    *out = (int)v;
    return 0;
}

int util_venc_parse_str_value(const char *json, char *out, size_t out_size) {
    const char *p = strstr(json, "\"value\":");
    if (!p) return -1;
    p += 8; /* skip past "value": */
    while (*p == ' ') p++;
    if (*p != '"') return -1;
    p++; /* skip opening quote */
    const char *end = strchr(p, '"');
    if (!end) return -1;
    size_t len = (size_t)(end - p);
    if (len >= out_size) len = out_size - 1;
    memcpy(out, p, len);
    out[len] = '\0';
    return 0;
}
