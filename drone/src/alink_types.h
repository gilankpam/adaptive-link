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
#define MAX_PROFILES      20
#define MAX_OUTPUT        512
#define MAX_CMD           1024
#define RAW_BUF           2048

/* ─── Network defaults ─── */
#define DEFAULT_PORT      9999
#define DEFAULT_IP        "10.5.0.10"

/* ─── File paths ─── */
#define CONFIG_FILE              "/etc/alink.conf"
#define PROFILE_FILE             "/etc/txprofiles.conf"
#define WFB_YAML                 "/etc/wfb.yaml"
#define WIFI_ADAPTERS_YAML       "/etc/wlan_adapters.yaml"

/* ─── Timing defaults ─── */
#define DEFAULT_PACE_EXEC_MS  20

/* ─── Hardware constants ─── */
#define MCS_COUNT      8
#define POWER_LEVELS   11

/* ─── RSSI queue constants ─── */
#define MAX_RSSI_QUEUE  64
#define MAX_RSSI_LINE   256

/* ─── Keyframe constants ─── */
#define MAX_CODES        5
#define CODE_LENGTH      8
#define EXPIRY_TIME_MS   1000

/* ─── Named magic numbers ─── */
#define ANTENNA_SPREAD_THRESHOLD_DB   20
#define HIGH_RES_PIXEL_THRESHOLD      1300000
#define FALLBACK_SCORE                999
#define SCORE_RANGE_BASE              1000
#define SCORE_RANGE_MAX               2000
#define NUM_ANTENNA_SLOTS             4
#define RSSI_HISTORY_SIZE             20

/* ─── Utility macros ─── */
#define min(a, b) ((a) < (b) ? (a) : (b))

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

/* ─── Keyframe request tracking ─── */
typedef struct {
    char code[CODE_LENGTH];
    struct timespec timestamp;
} KeyframeRequest;

#endif /* ALINK_TYPES_H */
