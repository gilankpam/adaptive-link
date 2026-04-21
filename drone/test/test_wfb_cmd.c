/**
 * @file test_wfb_cmd.c
 * @brief Unity tests for cmd_wfb_set_fec / cmd_wfb_set_radio.
 *
 * Spins a UDP "fake wfb_tx" on 127.0.0.1:ephemeral, points cmd_ctx at it,
 * and verifies the on-wire request bytes and the rc-driven return path.
 */

#include "unity.h"
#include "command.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>

/* ---- fake wfb_tx responder ---- */

typedef struct {
    int fd;
    uint16_t port;
    /* Last request captured for assertions. */
    uint8_t req_buf[32];
    ssize_t req_len;
    /* Responder config. */
    uint32_t reply_rc;    /* host byte order; htonl'd on wire */
    int reply;            /* 0 = stay silent, 1 = reply */
    int done;
} fake_wfb_t;

static void *responder_thread(void *arg) {
    fake_wfb_t *f = (fake_wfb_t *)arg;
    struct sockaddr_in from;
    socklen_t fromlen = sizeof(from);

    f->req_len = recvfrom(f->fd, f->req_buf, sizeof(f->req_buf), 0,
                          (struct sockaddr *)&from, &fromlen);

    if (f->reply && f->req_len >= 5) {
        uint8_t resp[8];
        memcpy(resp, f->req_buf, 4);         /* echo req_id */
        uint32_t rc_net = htonl(f->reply_rc);
        memcpy(resp + 4, &rc_net, 4);
        sendto(f->fd, resp, sizeof(resp), 0,
               (struct sockaddr *)&from, fromlen);
    }

    f->done = 1;
    return NULL;
}

static int fake_wfb_start(fake_wfb_t *f, int reply, uint32_t rc) {
    memset(f, 0, sizeof(*f));
    f->reply = reply;
    f->reply_rc = rc;

    f->fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (f->fd < 0) return -1;

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = 0;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(f->fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(f->fd);
        return -1;
    }
    socklen_t alen = sizeof(addr);
    if (getsockname(f->fd, (struct sockaddr *)&addr, &alen) < 0) {
        close(f->fd);
        return -1;
    }
    f->port = ntohs(addr.sin_port);
    return 0;
}

static void fake_wfb_stop(fake_wfb_t *f) {
    if (f->fd >= 0) close(f->fd);
    f->fd = -1;
}

/* ---- tests ---- */

void setUp(void) {}
void tearDown(void) {}

void test_set_fec_success(void) {
    fake_wfb_t f;
    TEST_ASSERT_EQUAL_INT(0, fake_wfb_start(&f, 1, 0));

    pthread_t t;
    pthread_create(&t, NULL, responder_thread, &f);

    cmd_ctx_t ctx;
    cmd_init(&ctx, 0, LOG_LEVEL_ERROR, f.port);

    int rc = cmd_wfb_set_fec(&ctx, 8, 12);
    pthread_join(t, NULL);

    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_EQUAL_INT(7, f.req_len);
    TEST_ASSERT_EQUAL_UINT8(1, f.req_buf[4]);  /* CMD_SET_FEC */
    TEST_ASSERT_EQUAL_UINT8(8, f.req_buf[5]);  /* k */
    TEST_ASSERT_EQUAL_UINT8(12, f.req_buf[6]); /* n */

    if (ctx.wfb_ctl_fd >= 0) close(ctx.wfb_ctl_fd);
    fake_wfb_stop(&f);
}

void test_set_fec_failure_rc(void) {
    fake_wfb_t f;
    TEST_ASSERT_EQUAL_INT(0, fake_wfb_start(&f, 1, EINVAL));

    pthread_t t;
    pthread_create(&t, NULL, responder_thread, &f);

    cmd_ctx_t ctx;
    cmd_init(&ctx, 0, LOG_LEVEL_ERROR, f.port);

    int rc = cmd_wfb_set_fec(&ctx, 4, 8);
    pthread_join(t, NULL);

    TEST_ASSERT_EQUAL_INT(EINVAL, rc);

    if (ctx.wfb_ctl_fd >= 0) close(ctx.wfb_ctl_fd);
    fake_wfb_stop(&f);
}

void test_set_fec_timeout(void) {
    fake_wfb_t f;
    TEST_ASSERT_EQUAL_INT(0, fake_wfb_start(&f, 0, 0));  /* no reply */

    pthread_t t;
    pthread_create(&t, NULL, responder_thread, &f);

    cmd_ctx_t ctx;
    cmd_init(&ctx, 0, LOG_LEVEL_ERROR, f.port);

    struct timeval t0, t1;
    gettimeofday(&t0, NULL);
    int rc = cmd_wfb_set_fec(&ctx, 1, 2);
    gettimeofday(&t1, NULL);

    pthread_join(t, NULL);

    long elapsed_ms = (t1.tv_sec - t0.tv_sec) * 1000L +
                      (t1.tv_usec - t0.tv_usec) / 1000L;

    TEST_ASSERT_NOT_EQUAL(0, rc);
    TEST_ASSERT_TRUE_MESSAGE(elapsed_ms >= 80 && elapsed_ms <= 250,
                             "timeout should fire near the 100ms SO_RCVTIMEO");

    if (ctx.wfb_ctl_fd >= 0) close(ctx.wfb_ctl_fd);
    fake_wfb_stop(&f);
}

void test_set_radio_20mhz(void) {
    fake_wfb_t f;
    TEST_ASSERT_EQUAL_INT(0, fake_wfb_start(&f, 1, 0));

    pthread_t t;
    pthread_create(&t, NULL, responder_thread, &f);

    cmd_ctx_t ctx;
    cmd_init(&ctx, 0, LOG_LEVEL_ERROR, f.port);

    int rc = cmd_wfb_set_radio(&ctx,
                               /*stbc=*/1, /*ldpc=*/true, /*short_gi=*/false,
                               /*bandwidth=*/20, /*mcs=*/3,
                               /*vht_mode=*/false, /*vht_nss=*/1);
    pthread_join(t, NULL);

    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_EQUAL_INT(12, f.req_len);
    TEST_ASSERT_EQUAL_UINT8(2, f.req_buf[4]);   /* CMD_SET_RADIO */
    TEST_ASSERT_EQUAL_UINT8(1, f.req_buf[5]);   /* stbc */
    TEST_ASSERT_EQUAL_UINT8(1, f.req_buf[6]);   /* ldpc */
    TEST_ASSERT_EQUAL_UINT8(0, f.req_buf[7]);   /* short_gi */
    TEST_ASSERT_EQUAL_UINT8(20, f.req_buf[8]);  /* bandwidth */
    TEST_ASSERT_EQUAL_UINT8(3, f.req_buf[9]);   /* mcs */
    TEST_ASSERT_EQUAL_UINT8(0, f.req_buf[10]);  /* vht_mode */
    TEST_ASSERT_EQUAL_UINT8(1, f.req_buf[11]);  /* vht_nss */

    if (ctx.wfb_ctl_fd >= 0) close(ctx.wfb_ctl_fd);
    fake_wfb_stop(&f);
}

void test_set_radio_80mhz_vht(void) {
    fake_wfb_t f;
    TEST_ASSERT_EQUAL_INT(0, fake_wfb_start(&f, 1, 0));

    pthread_t t;
    pthread_create(&t, NULL, responder_thread, &f);

    cmd_ctx_t ctx;
    cmd_init(&ctx, 0, LOG_LEVEL_ERROR, f.port);

    int rc = cmd_wfb_set_radio(&ctx,
                               /*stbc=*/0, /*ldpc=*/false, /*short_gi=*/true,
                               /*bandwidth=*/80, /*mcs=*/5,
                               /*vht_mode=*/true, /*vht_nss=*/1);
    pthread_join(t, NULL);

    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_EQUAL_UINT8(80, f.req_buf[8]);
    TEST_ASSERT_EQUAL_UINT8(1, f.req_buf[7]);   /* short_gi true */
    TEST_ASSERT_EQUAL_UINT8(1, f.req_buf[10]);  /* vht_mode true */

    if (ctx.wfb_ctl_fd >= 0) close(ctx.wfb_ctl_fd);
    fake_wfb_stop(&f);
}

void test_set_radio_failure_rc(void) {
    fake_wfb_t f;
    TEST_ASSERT_EQUAL_INT(0, fake_wfb_start(&f, 1, EIO));

    pthread_t t;
    pthread_create(&t, NULL, responder_thread, &f);

    cmd_ctx_t ctx;
    cmd_init(&ctx, 0, LOG_LEVEL_ERROR, f.port);

    int rc = cmd_wfb_set_radio(&ctx, 0, false, false, 20, 1, false, 1);
    pthread_join(t, NULL);

    TEST_ASSERT_EQUAL_INT(EIO, rc);

    if (ctx.wfb_ctl_fd >= 0) close(ctx.wfb_ctl_fd);
    fake_wfb_stop(&f);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_set_fec_success);
    RUN_TEST(test_set_fec_failure_rc);
    RUN_TEST(test_set_fec_timeout);
    RUN_TEST(test_set_radio_20mhz);
    RUN_TEST(test_set_radio_80mhz_vht);
    RUN_TEST(test_set_radio_failure_rc);
    return UNITY_END();
}
