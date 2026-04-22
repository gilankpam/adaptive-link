/**
 * @file http_client.c
 * @brief Native HTTP client using raw sockets (no curl dependency).
 * 
 * Provides lightweight HTTP GET functionality with timeout support.
 * Significantly faster than spawning curl processes.
 */

#include "http_client.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define HTTP_REQUEST_SIZE 1024
#define HTTP_RECV_BUFFER 4096
#define DEFAULT_TIMEOUT_MS 1000

/**
 * Set socket to non-blocking mode.
 */
static int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

/**
 * Set socket timeout using select().
 * Returns 0 on success, -1 on timeout/error.
 */
static int socket_recv_with_timeout(int sock, char *buf, size_t len, int timeout_ms) {
    fd_set readfds;
    struct timeval tv;
    int ready;
    
    FD_ZERO(&readfds);
    FD_SET(sock, &readfds);
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    
    ready = select(sock + 1, &readfds, NULL, NULL, &tv);
    if (ready <= 0) {
        return -1;  // Timeout or error
    }
    
    return recv(sock, buf, len, 0);
}

/**
 * Parse HTTP response to extract body.
 * Returns pointer to body start, or NULL if parsing fails.
 */
static const char *parse_http_response(const char *response, size_t len, size_t *body_len) {
    const char *header_end = strstr(response, "\r\n\r\n");
    if (!header_end) {
        return NULL;
    }
    
    header_end += 4;  // Skip "\r\n\r\n"
    *body_len = len - (header_end - response);
    return header_end;
}

int http_get(const char *host, int port, const char *path,
             char *response, size_t resp_size, int timeout_ms) {
    int sock = -1;
    struct sockaddr_in server_addr;
    char request[HTTP_REQUEST_SIZE];
    int bytes_received = 0;
    int total_received = 0;

    /* The majestic camera API always runs on the same SoC as this daemon, so
     * pin the TCP peer to 127.0.0.1 and skip name resolution entirely.
     * getaddrinfo() used to be called here; on boards with a stale /etc/hosts
     * or broken resolv.conf that could stall for seconds on every call.
     * The `host` parameter is still used below for the HTTP Host: header. */
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    server_addr.sin_addr.s_addr = htonl(0x7f000001);  /* 127.0.0.1 */

    // Create socket
    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("socket");
        return -1;
    }

    // Set non-blocking for timeout support
    if (set_nonblocking(sock) < 0) {
        perror("fcntl");
        close(sock);
        return -1;
    }
    
    // Connect with timeout
    if (connect(sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        if (errno != EINPROGRESS) {
            perror("connect");
            close(sock);
            return -1;
        }
        // Check if connection completed using select
        fd_set writefds;
        struct timeval tv;
        FD_ZERO(&writefds);
        FD_SET(sock, &writefds);
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
        
        int ret = select(sock + 1, NULL, &writefds, NULL, &tv);
        if (ret <= 0) {
            close(sock);
            return -1;  // Connection timeout
        }
        
        // Check socket error status
        int error;
        socklen_t len = sizeof(error);
        if (getsockopt(sock, SOL_SOCKET, SO_ERROR, &error, &len) < 0 || error) {
            close(sock);
            return -1;
        }
    }
    
    // Build HTTP request
    snprintf(request, sizeof(request),
             "GET %s HTTP/1.1\r\n"
             "Host: %s\r\n"
             "Connection: close\r\n"
             "Accept: */*\r\n"
             "\r\n",
             path, host);
    
    // Send request with timeout
    int bytes_sent = send(sock, request, strlen(request), 0);
    if (bytes_sent <= 0) {
        perror("send");
        close(sock);
        return -1;
    }
    
    // Receive response — always read at least the status line to detect errors
    {
        char local_buf[HTTP_RECV_BUFFER];
        char *recv_buf = (response != NULL && resp_size > 0) ? response : local_buf;
        size_t recv_size = (response != NULL && resp_size > 0) ? resp_size : sizeof(local_buf);
        char *recv_ptr = recv_buf;

        while ((bytes_received = socket_recv_with_timeout(sock, recv_ptr,
                    recv_size - total_received - 1, timeout_ms)) > 0) {
            total_received += bytes_received;
            recv_ptr += bytes_received;
            if (total_received >= (int)recv_size - 1)
                break;
        }
        recv_ptr[0] = '\0';

        if (total_received == 0) {
            fprintf(stderr, "http_get: no response from %s:%d%s\n", host, port, path);
            close(sock);
            return -1;
        }

        // Check HTTP status code
        int http_status = 0;
        if (strncmp(recv_buf, "HTTP/", 5) == 0) {
            const char *sp = strchr(recv_buf, ' ');
            if (sp) http_status = atoi(sp + 1);
        }

        if (http_status < 200 || http_status >= 300) {
            // Extract just the status line for the error message
            char status_line[128];
            const char *eol = strstr(recv_buf, "\r\n");
            size_t slen = eol ? (size_t)(eol - recv_buf) : strlen(recv_buf);
            if (slen >= sizeof(status_line)) slen = sizeof(status_line) - 1;
            memcpy(status_line, recv_buf, slen);
            status_line[slen] = '\0';

            size_t body_len = 0;
            const char *body = parse_http_response(recv_buf, (size_t)total_received, &body_len);
            fprintf(stderr, "http_get: %s:%d%s -> %s | %.*s\n",
                    host, port, path, status_line,
                    (int)(body_len > 200 ? 200 : body_len),
                    body ? body : "(no body)");
            close(sock);
            return -1;
        }

        // If caller wants the response body, extract it
        if (response != NULL && resp_size > 0) {
            size_t body_len = 0;
            const char *body = parse_http_response(response, (size_t)total_received, &body_len);
            if (body && body_len > 0) {
                size_t copy_len = body_len < resp_size - 1 ? body_len : resp_size - 1;
                memmove(response, body, copy_len);
                response[copy_len] = '\0';
            }
        }
    }

    close(sock);
    return 0;
}