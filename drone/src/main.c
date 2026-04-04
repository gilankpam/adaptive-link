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

    /* Shared synchronization */
    pthread_mutex_t count_mutex;
    pthread_mutex_t pause_mutex;
    pthread_mutex_t tx_power_mutex;
    volatile bool paused;
    volatile bool initialized;
    volatile int message_count;

    /* Network */
    int sockfd;
} alink_daemon_t;

static void print_usage(void) {
    printf("Usage: ./udp_server --port <port> --pace-exec <time> --verbose\n");
    printf("Options:\n");
    printf("  --ip         IP address to bind to (default: %s)\n", DEFAULT_IP);
    printf("  --port       Port to listen on (default: %d)\n", DEFAULT_PORT);
    printf("  --verbose    Enable verbose output\n");
    printf("  --debug-log  Enable debug logging of parameter changes\n");
    printf("  --pace-exec  Maj/wfb control execution pacing interval in milliseconds (default: %d ms)\n", DEFAULT_PACE_EXEC_MS);
}

int main(int argc, char *argv[]) {
    static alink_daemon_t daemon;

    /* Initialize subsystems with defaults */
    config_set_defaults(&daemon.cfg);
    hw_init(&daemon.hw);
    keyframe_init(&daemon.ks);
    osd_init(&daemon.osd);

    daemon.count_mutex = (pthread_mutex_t)PTHREAD_MUTEX_INITIALIZER;
    daemon.pause_mutex = (pthread_mutex_t)PTHREAD_MUTEX_INITIALIZER;
    daemon.tx_power_mutex = (pthread_mutex_t)PTHREAD_MUTEX_INITIALIZER;
    daemon.paused = false;
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
            strncpy(ip, argv[++i], INET_ADDRSTRLEN);
        } else if (strcmp(argv[i], "--verbose") == 0) {
            daemon.cfg.verbose_mode = true;
        } else if (strcmp(argv[i], "--debug-log") == 0) {
            daemon.cfg.debug_log = true;
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
                perror("Error creating outgoing UDP socket");
                return 1;
            }

            printf("OSD UDP output enabled to %s:%d\n", daemon.osd_udp.udp_out_ip, daemon.osd_udp.udp_out_port);
        } else {
            print_usage();
            return 1;
        }
    }

    /* Initialize command context */
    cmd_init(&daemon.cmd, pace_exec, daemon.cfg.verbose_mode);

    /* Initialize profile state */
    profile_init(&daemon.ps, &daemon.cfg, &daemon.hw, &daemon.cmd);

    /* Initialize RSSI monitor */
    rssi_init(&daemon.rs, daemon.cfg.verbose_mode);

    /* Initialize message processor */
    msg_init(&daemon.ms, &daemon.ps, &daemon.ks, &daemon.osd, &daemon.cfg,
             &daemon.pause_mutex, &daemon.paused, &daemon.cmd);

    /* Create UDP socket for incoming messages */
    if ((daemon.sockfd = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
        perror("Socket creation failed. TX connected? Make sure video and tunnel are working");
        osd_error("Adaptive-Link:  Check wfb tunnel functionality");
        exit(EXIT_FAILURE);
    }

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = inet_addr(ip);
    server_addr.sin_port = htons(port);

    if (bind(daemon.sockfd, (const struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("Bind failed");
        osd_error("Adaptive-Link:  Check wfb tunnel functionality");
        close(daemon.sockfd);
        exit(EXIT_FAILURE);
    }

    printf("Listening on UDP port %d, IP: %s...\n", port, ip);

    /* Determine power factor and load tables if required */
    if (!daemon.cfg.use_0_to_4_txpower) {
        hw_determine_tx_factor(&daemon.hw);
    } else {
        daemon.hw.tx_factor = 1;
        hw_load_tx_power_table(&daemon.hw);
        hw_print_tx_power_table(&daemon.hw);
    }
    printf("TX Power Factor: %d\n", daemon.hw.tx_factor);

    /* Get required values from wfb.yaml */
    if (daemon.cfg.get_card_info_from_yaml) {
        hw_load_vtx_info(&daemon.hw);
    }
    printf("ldpc_tx: %d\nstbc: %d\n", daemon.hw.ldpc_tx, daemon.hw.stbc);

    /* Get camera bin */
    if (hw_get_camera_bin(&daemon.hw) != 0) {
        printf("Didn't retrieve camera bin filename. Continuing...\n");
    }

    hw_get_resolution(&daemon.hw);
    osd_adjust_font_size(&daemon.osd, daemon.hw.x_res, daemon.cfg.multiply_font_size_by);

    /* Get FPS value from majestic */
    int fps = hw_get_video_fps();
    if (fps >= 0) {
        printf("Video FPS: %d\n", fps);
        daemon.hw.global_fps = fps;
        if (fps == 0) {
            daemon.cfg.limitFPS = 0;
        }
    } else {
        printf("Failed to retrieve video FPS from majestic.\n");
        daemon.cfg.limitFPS = 0;
    }

    /* Check if roi_focus_mode is enabled */
    if (daemon.cfg.roi_focus_mode) {
        if (hw_setup_roi(&daemon.hw, &daemon.cmd) != 0) {
            printf("Failed to set up focus mode regions based on majestic resolution\n");
        } else {
            printf("Focus mode regions set in majestic.yaml\n");
        }
    }

    /* Start drone antenna monitoring thread */
    pthread_t rssi_thread;
    if (pthread_create(&rssi_thread, NULL, rssi_thread_func, &daemon.rs)) {
        fprintf(stderr, "Error creating drone RSSI monitoring thread\n");
    }

    /* Start the fallback counting thread */
    fallback_thread_arg_t fallback_arg = {
        .ps = &daemon.ps,
        .osd = &daemon.osd,
        .cfg = &daemon.cfg,
        .count_mutex = &daemon.count_mutex,
        .pause_mutex = &daemon.pause_mutex,
        .message_count = &daemon.message_count,
        .paused = &daemon.paused,
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
                perror("recvfrom failed");
                break;
            }

            daemon.initialized = true;

            /* Increment message count */
            pthread_mutex_lock(&daemon.count_mutex);
            daemon.message_count++;
            pthread_mutex_unlock(&daemon.count_mutex);

            /* Null-terminate the received data */
            buffer[n] = '\0';

            /* Extract the length of the message (first 4 bytes) */
            uint32_t msg_length;
            memcpy(&msg_length, buffer, sizeof(msg_length));
            msg_length = ntohl(msg_length);

            if (daemon.cfg.verbose_mode) {
                printf("Received message (%u bytes): %s\n", msg_length, buffer + sizeof(msg_length));
            }

            /* Strip length off the start of the message */
            char *message = buffer + sizeof(uint32_t);
            /* See if it's a special command, otherwise process it */
            if (strncmp(message, "special:", 8) == 0) {
                keyframe_handle_special(&daemon.ks, message, &daemon.cfg,
                                        daemon.ps.prevSetGop,
                                        &daemon.paused, &daemon.pause_mutex,
                                        &daemon.cmd);
            } else {
                msg_process(&daemon.ms, message);
            }
        } else if (activity < 0) {
            perror("select failed");
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
