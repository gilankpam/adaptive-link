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

/**
 * Parse an HTTP/HTTPS URL and extract host, port, and path.
 * Expected format: http://host:port/path or https://host:port/path
 * Defaults: port 80 for http, 443 for https
 *
 * @param url       Input URL string
 * @param host      Output buffer for host (must be at least 64 bytes)
 * @param host_size Size of host buffer
 * @param port      Output pointer for port number
 * @param path      Output buffer for path (including query string)
 * @param path_size Size of path buffer
 * @return 0 on success, -1 on error
 */
int util_parse_url(const char *url, char *host, size_t host_size,
                   int *port, char *path, size_t path_size);

/**
 * Parse an integer "value" field from a waybeam_venc JSON response.
 * Finds the first "value": token and reads the integer that follows.
 * Example input: {"ok":true,"data":{"field":"video0.fps","value":60}}
 *
 * @param json    NUL-terminated JSON response string
 * @param out     Output integer
 * @return 0 on success, -1 if token not found or value is not parseable
 */
int util_venc_parse_int_value(const char *json, int *out);

/**
 * Parse a string "value" field from a waybeam_venc JSON response.
 * Finds the first "value": token and reads the quoted string that follows.
 * Example input: {"ok":true,"data":{"field":"video0.size","value":"1920x1080"}}
 *
 * @param json     NUL-terminated JSON response string
 * @param out      Output buffer
 * @param out_size Size of output buffer (result is always NUL-terminated)
 * @return 0 on success, -1 if token not found or string is malformed
 */
int util_venc_parse_str_value(const char *json, char *out, size_t out_size);

#endif /* ALINK_UTIL_H */
