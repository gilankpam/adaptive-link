/**
 * @file cmd_server.c
 * @brief Unix domain socket command listener thread.
 */
#include "cmd_server.h"
#include "util.h"

static char *cmdsrv_handle_get(const char *command, const alink_config_t *cfg) {
    static char trimmed[128];
    strncpy(trimmed, command, sizeof(trimmed)-1);
    trimmed[sizeof(trimmed)-1] = '\0';
    util_trim_whitespace(trimmed);

    if (cfg->verbose_mode) printf("[DEBUG] GET command received: '%s'\n", trimmed);

    if (strcmp(trimmed, "osd_level") == 0) {
        static char result[16];
        snprintf(result, sizeof(result), "%d", cfg->osd_level);
        return result;

    } else if (strcmp(trimmed, "multiply_font_size_by") == 0) {
        static char result[16];
        snprintf(result, sizeof(result), "%.3f", cfg->multiply_font_size_by);
        return result;

    } else if (strcmp(trimmed, "roi_focus_mode") == 0) {
        static char result[6];
        snprintf(result, sizeof(result), "%d", cfg->roi_focus_mode);
        return result;

    } else if (strcmp(trimmed, "limit_max_score_to") == 0) {
        static char result[6];
        snprintf(result, sizeof(result), "%d", cfg->limit_max_score_to);
        return result;

    } else {
        if (cfg->verbose_mode) {
            return "Unknown GET command";
        }
        return "";
    }
}

static char *cmdsrv_handle_set(const char *command, const char *arg_str,
                                alink_config_t *cfg, profile_state_t *ps,
                                hw_state_t *hw, osd_state_t *osd) {
    static char result[16];

    if (strcmp(command, "osd_level") == 0) {
        int val = atoi(arg_str);
        if (val < 0) val = 0;
        if (val > 6) val = 6;
        cfg->osd_level = val;

        if (val == 0) {
            fclose(fopen("/tmp/MSPOSD.msg", "w"));
        }

        snprintf(result, sizeof(result), "%d", cfg->osd_level);

        char val_str[16];
        snprintf(val_str, sizeof(val_str), "%d", val);
        config_update_param("osd_level", val_str);

        return result;

    } else if (strcmp(command, "multiply_font_size_by") == 0) {
        float val = atof(arg_str);
        if (val < 0.3f) val = 0.3f;
        if (val > 2.5f) val = 2.5f;
        cfg->multiply_font_size_by = val;

        hw_get_resolution(hw);
        osd_adjust_font_size(osd, hw->x_res, cfg->multiply_font_size_by);

        snprintf(result, sizeof(result), "%.3f", cfg->multiply_font_size_by);

        char val_str[16];
        snprintf(val_str, sizeof(val_str), "%.3f", val);
        config_update_param("multiply_font_size_by", val_str);

        return result;

    } else if (strcmp(command, "roi_focus_mode") == 0) {
        bool val = (atoi(arg_str) != 0);
        cfg->roi_focus_mode = val;

        static char roi_result[6];
        snprintf(roi_result, sizeof(roi_result), "%d", cfg->roi_focus_mode);

        if (cfg->roi_focus_mode) {
            if (hw_setup_roi(hw) != 0) {
                printf("Failed to re-set focus mode regions\n");
            } else {
                printf("Focus mode regions re-set in majestic.yaml\n");
                printf("%s\n", system("killall -HUP majestic") == 0 ? "killMajSuccess" : "killMajFailure");
            }
        }

        if (ps->selectedProfile != NULL) {
            profile_apply(ps, ps->selectedProfile, osd);
            printf("Profile re-applied to update roi\n");
        }

        config_update_param("roi_focus_mode", roi_result);

        return roi_result;

    } else if (strcmp(command, "limit_max_score_to") == 0) {
        int val = atoi(arg_str);
        if (val < 1000) val = 1000;
        if (val > 2000) val = 2000;
        cfg->limit_max_score_to = val;

        snprintf(result, sizeof(result), "%d", cfg->limit_max_score_to);

        return result;

    } else {
        if (cfg->verbose_mode) {
            return "Unknown SET command";
        }
        return "";
    }
}

void *cmdsrv_thread_func(void *arg) {
    cmdsrv_thread_arg_t *ta = (cmdsrv_thread_arg_t *)arg;

    int srv_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (srv_fd < 0) {
        perror("alink_cmd socket");
        return NULL;
    }

    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    strncpy(addr.sun_path, ALINK_CMD_SOCKET_PATH, sizeof(addr.sun_path) - 1);
    unlink(ALINK_CMD_SOCKET_PATH);

    if (bind(srv_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("alink_cmd bind");
        close(srv_fd);
        return NULL;
    }

    if (listen(srv_fd, 1) < 0) {
        perror("alink_cmd listen");
        close(srv_fd);
        return NULL;
    }

    printf("alink: command socket listening on %s\n", ALINK_CMD_SOCKET_PATH);

    for (;;) {
        int cl_fd = accept(srv_fd, NULL, NULL);
        if (cl_fd < 0) {
            perror("alink_cmd accept");
            continue;
        }

        struct alink_msg_hdr hdr;
        ssize_t n = read(cl_fd, &hdr, sizeof(hdr));
        if (n != sizeof(hdr)) {
            fprintf(stderr, "alink_cmd: invalid header read (%zd bytes)\n", n);
            close(cl_fd);
            continue;
        }

        hdr.cmd = ntohs(hdr.cmd);
        hdr.len = ntohs(hdr.len);

        /* CMD_SET_POWER */
        if (hdr.cmd == CMD_SET_POWER && hdr.len == sizeof(uint32_t)) {
            uint32_t net_v;
            if (read(cl_fd, &net_v, sizeof(net_v)) != sizeof(net_v)) {
                fprintf(stderr, "alink_cmd: failed to read power value\n");
                close(cl_fd);
                continue;
            }

            int v = ntohl(net_v);
            int32_t status;

            if (v >= 0 && v <= 4) {
                pthread_mutex_lock(ta->tx_power_mutex);
                ta->cfg->power_level_0_to_4 = v;
                pthread_mutex_unlock(ta->tx_power_mutex);
                printf("alink: TX power updated to %d via command socket\n", v);

                if (ta->cfg->use_0_to_4_txpower && ta->ps->selectedProfile != NULL) {
                    profile_apply(ta->ps, ta->ps->selectedProfile, ta->osd);
                    printf("Profile re-applied to SET power to %d\n", v);
                }
                status = 0;
            } else {
                status = 1;
            }

            struct alink_msg_hdr resp_hdr = {
                .cmd = htons(CMD_SET_POWER | CMD_STATUS_REPLY),
                .len = htons(sizeof(status))
            };
            int32_t net_status = htonl(status);

            if (write(cl_fd, &resp_hdr, sizeof(resp_hdr)) < 0)
                perror("write resp_hdr (CMD_SET_POWER)");

            if (write(cl_fd, &net_status, sizeof(net_status)) < 0)
                perror("write net_status (CMD_SET_POWER)");
        }

        /* CMD_ANTENNA_STATS */
        else if (hdr.cmd == CMD_ANTENNA_STATS) {
            if (hdr.len > 0 && hdr.len < MAX_RSSI_LINE) {
                char buf[MAX_RSSI_LINE] = {0};
                ssize_t m = read(cl_fd, buf, hdr.len);
                if (m != hdr.len) {
                    fprintf(stderr, "alink_cmd: malformed RX_ANT payload (read %zd bytes, expected %u)\n", m, hdr.len);
                    close(cl_fd);
                    continue;
                }
                buf[hdr.len] = '\0';

                if (rssi_enqueue(ta->rs, buf) < 0) {
                    fprintf(stderr, "RSSI queue full, dropping line\n");
                }

            } else {
                fprintf(stderr, "alink_cmd: invalid RX_ANT length: %u\n", hdr.len);
            }

            struct alink_msg_hdr resp_hdr = {
                .cmd = htons(CMD_ANTENNA_STATS | CMD_STATUS_REPLY),
                .len = htons(sizeof(uint32_t))
            };
            uint32_t ok = htonl(0);

            if (write(cl_fd, &resp_hdr, sizeof(resp_hdr)) < 0)
                perror("write resp_hdr (CMD_ANTENNA_STATS)");

            if (write(cl_fd, &ok, sizeof(ok)) < 0)
                perror("write ok (CMD_ANTENNA_STATS)");
        }

        /* CMD_GET or CMD_SET */
        else if (hdr.cmd == CMD_GET || hdr.cmd == CMD_SET) {
            if (hdr.len > 0 && hdr.len < 256) {
                char buf[256] = {0};
                ssize_t m = read(cl_fd, buf, hdr.len);
                if (m != hdr.len) {
                    fprintf(stderr, "alink_cmd: malformed command payload (read %zd bytes, expected %u)\n", m, hdr.len);
                    close(cl_fd);
                    continue;
                }
                buf[hdr.len] = '\0';

                char *response;
                if (hdr.cmd == CMD_GET) {
                    response = cmdsrv_handle_get(buf, ta->cfg);
                } else {
                    char *space = strchr(buf, ' ');
                    if (space) {
                        *space = '\0';
                        response = cmdsrv_handle_set(buf, space + 1, ta->cfg, ta->ps, ta->hw, ta->osd);
                    } else {
                        response = "Missing argument for SET command";
                    }
                }

                struct alink_msg_hdr resp_hdr = {
                    .cmd = htons(hdr.cmd | CMD_STATUS_REPLY),
                    .len = htons(strlen(response))
                };
                if (write(cl_fd, &resp_hdr, sizeof(resp_hdr)) < 0)
                    perror("write resp_hdr (GET/SET)");

                if (write(cl_fd, response, strlen(response)) < 0)
                    perror("write response (GET/SET)");
            } else {
                fprintf(stderr, "alink_cmd: invalid GET/SET length: %u\n", hdr.len);
            }
        }
        else {
            fprintf(stderr, "alink_cmd: unknown command: 0x%04x\n", hdr.cmd);

            struct alink_msg_hdr resp_hdr = {
                .cmd = htons(hdr.cmd | CMD_STATUS_REPLY),
                .len = htons(sizeof(uint32_t))
            };
            uint32_t err = htonl(1);

            if (write(cl_fd, &resp_hdr, sizeof(resp_hdr)) < 0)
                perror("write resp_hdr (unknown cmd)");

            if (write(cl_fd, &err, sizeof(err)) < 0)
                perror("write err (unknown cmd)");
        }

        close(cl_fd);
    }

    close(srv_fd);
    unlink(ALINK_CMD_SOCKET_PATH);
    return NULL;
}
