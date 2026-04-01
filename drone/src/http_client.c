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
#include <netdb.h>

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
    struct hostent *server;
    char request[HTTP_REQUEST_SIZE];
    int bytes_received = 0;
    int total_received = 0;
    
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
    
    // Resolve host
    server = gethostbyname(host);
    if (server == NULL) {
        fprintf(stderr, "http_get: gethostbyname failed for %s\n", host);
        close(sock);
        return -1;
    }
    
    // Fill server address
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    // Use h_addr_list[0] instead of deprecated h_addr
    if (server->h_addr_list[0] != NULL) {
        memcpy(&server_addr.sin_addr.s_addr, server->h_addr_list[0], server->h_length);
    } else {
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
    
    // Receive response with timeout
    if (response != NULL && resp_size > 0) {
        // Use response buffer directly, tracking position with recv_ptr
        char *recv_ptr = response;
        
        while ((bytes_received = socket_recv_with_timeout(sock, recv_ptr, resp_size - total_received - 1, timeout_ms)) > 0) {
            total_received += bytes_received;
            recv_ptr += bytes_received;  // Advance buffer pointer
            if (total_received >= (int)resp_size - 1) {
                break;
            }
        }
        
        // Null-terminate the received data
        recv_ptr[0] = '\0';
        
        if (bytes_received < 0) {
            // Timeout or error
            close(sock);
            return -1;
        }
        
        // Parse and extract body from response buffer
        size_t body_len = 0;
        const char *body = parse_http_response(response, (size_t)total_received, &body_len);
        if (body && body_len > 0) {
            // Move body to the start of response buffer
            size_t copy_len = body_len < resp_size - 1 ? body_len : resp_size - 1;
            memmove(response, body, copy_len);
            response[copy_len] = '\0';
        }
        // If no body, response already contains the full response (headers included)
    }
    
    close(sock);
    return 0;
}

int http_get_no_response(const char *host, int port, const char *path, int timeout_ms) {
    // Just call http_get with NULL response buffer
    return http_get(host, port, path, NULL, 0, timeout_ms);
}