/**
 * @file main.c
 * @brief Entry point for the alink_drone daemon.
 *
 * Handles argument parsing, socket setup, initialization sequencing,
 * thread creation, and the main UDP receive loop.
 */
#include "alink_types.h"
#include "util.h"
#include "config.h"
#include "hardware.h"
#include "command.h"
#include "profile.h"
#include "osd.h"
#include "keyframe.h"
#include "rssi_monitor.h"
#include "tx_monitor.h"
#include "message.h"
#include "fallback.h"
#include <signal.h>

typedef struct {
    alink_config_t cfg;
    hw_state_t hw;
    cmd_ctx_t cmd;
    profile_state_t ps;
    keyframe_state_t ks;
    rssi_state_t rs;
    msg_state_t ms;
    osd_state_t osd;
    osd_udp_config_t osd_udp;

    /* Session — random ID generated once at startup; lets the GS detect
     * daemon restarts and rotate telemetry logs accordingly. */
    uint32_t session_id;

    /* Shared synchronization */
    pthread_mutex_t count_mutex;
    pthread_mutex_t tx_power_mutex;
    volatile bool initialized;
    volatile int message_count;

    /* Network */
    int sockfd;
} alink_daemon_t;

static void print_usage(void) {
    printf("Usage: ./udp_server --port <port> --pace-exec <time> --log-level <level>\n");
    printf("Options:\n");
    printf("  --ip         IP address to bind to (default: %s)\n", DEFAULT_IP);
    printf("  --port       Port to listen on (default: %d)\n", DEFAULT_PORT);
    printf("  --log-level  Logging verbosity (debug, info, error)\n");
    printf("  --pace-exec  Maj/wfb control execution pacing interval in milliseconds (default: %d ms)\n", DEFAULT_PACE_EXEC_MS);
}

int main(int argc, char *argv[]) {
    static alink_daemon_t daemon;

    /* A closing HTTP peer (majestic restart, OOM) would otherwise deliver
     * SIGPIPE on the next send() and kill the daemon. Convert to EPIPE. */
    signal(SIGPIPE, SIG_IGN);

    /* Generate a random session ID so the GS can detect daemon restarts. */
    {
        FILE *urand = fopen("/dev/urandom", "r");
        if (urand) {
            if (fread(&daemon.session_id, sizeof(daemon.session_id), 1, urand) != 1)
                daemon.session_id = (uint32_t)(getpid() ^ time(NULL));
            fclose(urand);
        } else {
            daemon.session_id = (uint32_t)(getpid() ^ time(NULL));
        }
    }

    /* Initialize subsystems with defaults */
    config_set_defaults(&daemon.cfg);
    hw_init(&daemon.hw);
    keyframe_init(&daemon.ks);
    osd_init(&daemon.osd);

    daemon.count_mutex = (pthread_mutex_t)PTHREAD_MUTEX_INITIALIZER;
    daemon.tx_power_mutex = (pthread_mutex_t)PTHREAD_MUTEX_INITIALIZER;
    daemon.initialized = false;
    daemon.message_count = 0;

    /* Load configuration */
    config_load(&daemon.cfg, CONFIG_FILE);

    /* Network setup */
    struct sockaddr_in server_addr, client_addr;
    socklen_t client_addr_len = sizeof(client_addr);
    char buffer[BUFFER_SIZE];
    int port = DEFAULT_PORT;
    char ip[INET_ADDRSTRLEN] = DEFAULT_IP;
    long pace_exec = DEFAULT_PACE_EXEC_MS * 1000L;

    /* Initialize OSD UDP config */
    daemon.osd_udp.udp_out_sock = -1;

    /* Parse command-line arguments */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            port = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--ip") == 0 && i + 1 < argc) {
            strncpy(ip, argv[++i], INET_ADDRSTRLEN - 1);
            ip[INET_ADDRSTRLEN - 1] = '\0';
        } else if (strcmp(argv[i], "--log-level") == 0 && i + 1 < argc) {
            const char* level = argv[++i];
            if (strcmp(level, "debug") == 0) daemon.cfg.log_level = LOG_LEVEL_DEBUG;
            else if (strcmp(level, "info") == 0) daemon.cfg.log_level = LOG_LEVEL_INFO;
            else if (strcmp(level, "error") == 0) daemon.cfg.log_level = LOG_LEVEL_ERROR;
            else {
                fprintf(stderr, "Unknown log level: %s\n", level);
            }
        } else if (strcmp(argv[i], "--pace-exec") == 0 && i + 1 < argc) {
            int ms = atoi(argv[++i]);
            pace_exec = ms * 1000L;
        } else if (strcmp(argv[i], "--osd2udp") == 0 && i + 1 < argc) {
            char *ip_port = argv[++i];
            char *colon_pos = strchr(ip_port, ':');
            if (colon_pos) {
                *colon_pos = '\0';
                strncpy(daemon.osd_udp.udp_out_ip, ip_port, INET_ADDRSTRLEN);
                daemon.osd_udp.udp_out_port = atoi(colon_pos + 1);
            } else {
                fprintf(stderr, "Invalid format for --osd2udp. Expected <ip:port>\n");
                return 1;
            }

            if ((daemon.osd_udp.udp_out_sock = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
                ERROR_LOG(&daemon.cfg, "Error creating outgoing UDP socket\n");
                return 1;
            }

            INFO_LOG(&daemon.cfg, "OSD UDP output enabled to %s:%d\n", daemon.osd_udp.udp_out_ip, daemon.osd_udp.udp_out_port);
        } else {
            print_usage();
            return 1;
        }
    }

    daemon.hw.log_level = daemon.cfg.log_level;
    daemon.ks.log_level = daemon.cfg.log_level;

    /* Initialize command context */
    cmd_init(&daemon.cmd, pace_exec, daemon.cfg.log_level, daemon.cfg.wfb_control_port);

    /* Initialize profile state */
    profile_init(&daemon.ps, &daemon.cfg, &daemon.hw, &daemon.cmd);

    /* Initialize RSSI monitor */
    rssi_init(&daemon.rs, daemon.cfg.log_level);

    /* Initialize message processor */
    msg_init(&daemon.ms, &daemon.ps, &daemon.ks, &daemon.osd, &daemon.cfg,
             &daemon.cmd);

    /* Create UDP socket for incoming messages */
    if ((daemon.sockfd = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
        ERROR_LOG(&daemon.cfg, "Socket creation failed. TX connected? Make sure video and tunnel are working\n");
        osd_error("Adaptive-Link:  Check wfb tunnel functionality");
        exit(EXIT_FAILURE);
    }

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = inet_addr(ip);
    server_addr.sin_port = htons(port);

    if (bind(daemon.sockfd, (const struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        ERROR_LOG(&daemon.cfg, "Bind failed\n");
        osd_error("Adaptive-Link:  Check wfb tunnel functionality");
        close(daemon.sockfd);
        exit(EXIT_FAILURE);
    }

    INFO_LOG(&daemon.cfg, "Listening on UDP port %d, IP: %s...\n", port, ip);

    /* Get required values from wfb.yaml */
    if (daemon.cfg.get_card_info_from_yaml) {
        hw_load_vtx_info(&daemon.hw);
    }
    INFO_LOG(&daemon.cfg, "ldpc_tx: %d\nstbc: %d\n", daemon.hw.ldpc_tx, daemon.hw.stbc);

    /* Get camera bin */
    if (hw_get_camera_bin(&daemon.hw) != 0) {
        INFO_LOG(&daemon.cfg, "Didn't retrieve camera bin filename. Continuing...\n");
    }

    hw_get_resolution(&daemon.hw);
    hw_setup_roi(&daemon.hw);
    osd_adjust_font_size(&daemon.osd, daemon.hw.x_res, daemon.cfg.multiply_font_size_by);

    /* Get FPS value from majestic */
    int fps = hw_get_video_fps(&daemon.hw);
    if (fps >= 0) {
        INFO_LOG(&daemon.cfg, "Video FPS: %d\n", fps);
        daemon.hw.global_fps = fps;
    } else {
        ERROR_LOG(&daemon.cfg, "Failed to retrieve video FPS from majestic.\n");
    }

    /* Start drone antenna monitoring thread */
    pthread_t rssi_thread;
    if (pthread_create(&rssi_thread, NULL, rssi_thread_func, &daemon.rs)) {
        ERROR_LOG(&daemon.cfg, "Error creating drone RSSI monitoring thread\n");
    }

    /* Start the fallback counting thread */
    fallback_thread_arg_t fallback_arg = {
        .ps = &daemon.ps,
        .osd = &daemon.osd,
        .cfg = &daemon.cfg,
        .count_mutex = &daemon.count_mutex,
        .message_count = &daemon.message_count,
        .initialized = &daemon.initialized
    };
    pthread_t count_thread;
    pthread_create(&count_thread, NULL, fallback_thread_func, &fallback_arg);

    /* Start the periodic OSD update thread */
    osd_thread_arg_t osd_arg = {
        .udp_config = &daemon.osd_udp,
        .osd = &daemon.osd,
        .cfg = &daemon.cfg,
        .hw = &daemon.hw,
        .ps = &daemon.ps,
        .ks = &daemon.ks,
        .rs = &daemon.rs,
        .ms = &daemon.ms,
        .cmd = &daemon.cmd,
        .initialized = &daemon.initialized
    };
    pthread_t osd_thread;
    pthread_create(&osd_thread, NULL, osd_thread_func, &osd_arg);

    /* Start the async profile worker thread */
    pthread_t profile_worker_thread;
    pthread_create(&profile_worker_thread, NULL, profile_worker_func, &daemon.ps);

    /* Start the periodic TX dropped thread */
    txmon_thread_arg_t txmon_arg = {
        .ps = &daemon.ps,
        .ks = &daemon.ks,
        .hw = &daemon.hw,
        .cfg = &daemon.cfg,
        .cmd = &daemon.cmd,
        .initialized = &daemon.initialized
    };
    pthread_t tx_dropped_thread;
    pthread_create(&tx_dropped_thread, NULL, txmon_thread_func, &txmon_arg);

    /* Main loop for processing incoming messages */
    while (1) {
        fd_set rfds;
        struct timeval tv = {0, 50000}; /* 50ms timeout */
        FD_ZERO(&rfds);
        FD_SET(daemon.sockfd, &rfds);

        int activity = select(daemon.sockfd + 1, &rfds, NULL, NULL, &tv);

        if (activity > 0 && FD_ISSET(daemon.sockfd, &rfds)) {
            int n = recvfrom(daemon.sockfd, buffer, sizeof(buffer) - 1, 0,
                             (struct sockaddr *)&client_addr, &client_addr_len);
            if (n < 0) {
                ERROR_LOG(&daemon.cfg, "recvfrom failed\n");
                break;
            }

            daemon.initialized = true;

            /* Increment message count */
            pthread_mutex_lock(&daemon.count_mutex);
            daemon.message_count++;
            pthread_mutex_unlock(&daemon.count_mutex);

            /* Reject any datagram too short to contain even the length prefix. */
            if ((size_t)n < sizeof(uint32_t)) {
                ERROR_LOG(&daemon.cfg, "bad frame: short datagram (%d bytes)\n", n);
                continue;
            }

            /* Extract the length of the message (first 4 bytes) */
            uint32_t msg_length;
            memcpy(&msg_length, buffer, sizeof(msg_length));
            msg_length = ntohl(msg_length);

            /* Bound-check declared length against bytes actually received.
             * Catches truncation and buggy/malformed senders before downstream
             * strncmp/strtok can run past the real payload. */
            if (msg_length == 0 ||
                msg_length > (uint32_t)(n - (int)sizeof(uint32_t))) {
                ERROR_LOG(&daemon.cfg, "bad frame: declared=%u recv=%d\n", msg_length, n);
                continue;
            }

            /* Null-terminate at the end of the declared payload, not the end
             * of the recv buffer — keeps downstream parsers inside the frame. */
            buffer[sizeof(uint32_t) + msg_length] = '\0';

            if (daemon.cfg.log_level >= LOG_LEVEL_DEBUG) {
                INFO_LOG(&daemon.cfg, "Received message (%u bytes): %s\n", msg_length, buffer + sizeof(msg_length));
            }

            /* Strip length off the start of the message */
            char *message = buffer + sizeof(uint32_t);
            if (msg_length >= 2 && strncmp(message, "H:", 2) == 0) {
                msg_handle_hello(message + 2, msg_length - 2,
                                 &daemon.hw, daemon.sockfd, &client_addr,
                                 daemon.session_id);
                continue;   /* handshake is NOT a heartbeat — do not touch message_count */
            }
            msg_process(&daemon.ms, message, msg_length);
        } else if (activity < 0) {
            ERROR_LOG(&daemon.cfg, "select failed\n");
            break;
        }
        /* Timeout or data processed — periodic tick runs here */
    }

    /* Close the socket */
    close(daemon.sockfd);

    /* Close outgoing OSD socket if it was opened */
    if (daemon.osd_udp.udp_out_sock != -1) {
        close(daemon.osd_udp.udp_out_sock);
    }

    return 0;
}
