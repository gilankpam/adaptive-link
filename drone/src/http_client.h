/**
 * @file http_client.h
 * @brief Native HTTP client using raw sockets (no curl dependency).
 * 
 * Provides lightweight HTTP GET functionality with timeout support.
 * Significantly faster than spawning curl processes.
 */

#ifndef HTTP_CLIENT_H
#define HTTP_CLIENT_H

#include <stddef.h>

/**
 * Perform an HTTP GET request using raw sockets.
 * 
 * @param host      Target host (e.g., "localhost" or "127.0.0.1")
 * @param port      Target port (e.g., 80)
 * @param path      URL path with query string (e.g., "/api/v1/set?bitrate=12345")
 * @param response  Buffer to store response body (optional, can be NULL)
 * @param resp_size Size of response buffer
 * @param timeout_ms Timeout in milliseconds
 * @return 0 on success, non-zero on error
 */
int http_get(const char *host, int port, const char *path,
             char *response, size_t resp_size, int timeout_ms);

#endif /* HTTP_CLIENT_H */