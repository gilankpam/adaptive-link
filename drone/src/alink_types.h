/**
 * @file alink_types.h
 * @brief Shared type definitions, constants, and macros for the alink_drone daemon.
 *
 * This header-only module defines all structures, enumerations, and preprocessor
 * constants used across the alink_drone modules. It has no dependencies beyond
 * the standard C library.
 */
#ifndef ALINK_TYPES_H
#define ALINK_TYPES_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdbool.h>
#include <sys/time.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <time.h>
#include <math.h>
#include <ctype.h>
#include <limits.h>
/* ─── Buffer and size constants ─── */
#define MAX_COMMAND_SIZE  256
#define BUFFER_SIZE       1024

/* ─── Network defaults ─── */
#define DEFAULT_PORT      9999
#define DEFAULT_IP        "10.5.0.10"

/* ─── File paths ─── */
#define CONFIG_FILE              "/etc/alink.conf"

/* ─── Timing defaults ─── */
#define DEFAULT_PACE_EXEC_MS  20

/* ─── RSSI queue constants ─── */
#define MAX_RSSI_QUEUE  64
#define MAX_RSSI_LINE   256

/* ─── Named magic numbers ─── */
#define ANTENNA_SPREAD_THRESHOLD_DB   20
#define NUM_ANTENNA_SLOTS             4
#define RSSI_HISTORY_SIZE             20

/* ─── Log Level Enum ─── */
typedef enum {
    LOG_LEVEL_ERROR = 0,
    LOG_LEVEL_INFO,
    LOG_LEVEL_DEBUG
} log_level_t;

/* ─── Profile struct ─── */
typedef struct {
    int rangeMin;
    int rangeMax;
    char setGI[10];
    int setMCS;
    int setFecK;
    int setFecN;
    int setBitrate;
    float setGop;
    int wfbPower;
    int bandwidth;
} Profile;

/* ─── OSD UDP output config ─── */
typedef struct {
    int udp_out_sock;
    char udp_out_ip[INET_ADDRSTRLEN];
    int udp_out_port;
} osd_udp_config_t;

#endif /* ALINK_TYPES_H */
