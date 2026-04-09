#include "unity.h"
#include <string.h>
#include <stdint.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <stdbool.h>

// Mocks
static int mock_sendto_called = 0;
static char mock_sendto_buf[256];

ssize_t mock_sendto(int sockfd, const void *buf, size_t len, int flags,
                    const struct sockaddr *dest_addr, socklen_t addrlen) {
    (void)sockfd; (void)flags; (void)dest_addr; (void)addrlen;
    mock_sendto_called++;
    size_t copy_len = len < sizeof(mock_sendto_buf) ? len : sizeof(mock_sendto_buf);
    memcpy(mock_sendto_buf, buf, copy_len);
    return len;
}

static uint64_t mock_time_ms = 10000;
uint64_t mock_util_now_ms(void) {
    uint64_t t = mock_time_ms;
    mock_time_ms += 10;
    return t;
}

int mock_clock_gettime(clockid_t clk_id, struct timespec *tp) {
    (void)clk_id;
    tp->tv_sec = mock_time_ms / 1000;
    tp->tv_nsec = (mock_time_ms % 1000) * 1000000;
    return 0;
}

#define sendto mock_sendto
#define util_now_ms mock_util_now_ms
#define clock_gettime mock_clock_gettime

// Include the C file to test it with macros replaced
#include "../src/message.c"

// Stubs for functions called by message.c
void profile_apply_direct(profile_state_t *ps, const Profile *profile, int profile_index, void *osd) {
    (void)ps; (void)profile; (void)profile_index; (void)osd;
}
void keyframe_handle_special(keyframe_state_t *ks, const char *msg, const alink_config_t *cfg, float prev_gop, volatile bool *paused, pthread_mutex_t *mut, const cmd_ctx_t *cmd) {
    (void)ks; (void)msg; (void)cfg; (void)prev_gop; (void)paused; (void)mut; (void)cmd;
}

void setUp(void) {
    mock_sendto_called = 0;
    mock_time_ms = 10000;
    memset(mock_sendto_buf, 0, sizeof(mock_sendto_buf));
}

void tearDown(void) {}

void test_handle_hello_valid(void) {
    msg_state_t ms;
    memset(&ms, 0, sizeof(ms));
    hw_state_t hw = {0};
    hw.global_fps = 90;
    hw.x_res = 1920;
    hw.y_res = 1080;
    struct sockaddr_in addr = {0};

    int ret = msg_handle_hello(&ms, "12345", 5, &hw, 0, &addr);
    TEST_ASSERT_EQUAL(0, ret);
    TEST_ASSERT_TRUE(ms.time_synced);
    TEST_ASSERT_EQUAL(1, mock_sendto_called);
    
    uint32_t len_be;
    memcpy(&len_be, mock_sendto_buf, 4);
    uint32_t payload_len = ntohl(len_be);
    
    char payload[128] = {0};
    memcpy(payload, mock_sendto_buf + 4, payload_len);
    
    TEST_ASSERT_EQUAL_STRING("I:12345:10000:10010:90:1920:1080", payload);
}

void test_handle_hello_malformed(void) {
    msg_state_t ms;
    memset(&ms, 0, sizeof(ms));
    hw_state_t hw = {0};
    struct sockaddr_in addr = {0};

    // Neg
    TEST_ASSERT_NOT_EQUAL(0, msg_handle_hello(&ms, "-100", 4, &hw, 0, &addr));
    // Zero
    TEST_ASSERT_NOT_EQUAL(0, msg_handle_hello(&ms, "0", 1, &hw, 0, &addr));
    // Non-numeric
    TEST_ASSERT_NOT_EQUAL(0, msg_handle_hello(&ms, "abc", 3, &hw, 0, &addr));
}

void test_handle_hello_forward_compat(void) {
    msg_state_t ms;
    memset(&ms, 0, sizeof(ms));
    hw_state_t hw = {0};
    struct sockaddr_in addr = {0};

    int ret = msg_handle_hello(&ms, "555:extra:stuff", 15, &hw, 0, &addr);
    TEST_ASSERT_EQUAL(0, ret);
    TEST_ASSERT_TRUE(ms.time_synced);
}

void test_msg_update_latency(void) {
    msg_state_t ms;
    memset(&ms, 0, sizeof(ms));
    osd_state_t os;
    ms.osd = &os;
    ms.time_synced = true;
    
    pthread_mutex_t mut = PTHREAD_MUTEX_INITIALIZER;
    ms.pause_mutex = &mut;
    bool paused = false;
    ms.paused = &paused;
    profile_state_t ps;
    ms.ps = &ps;

    mock_time_ms = 500;
    
    msg_process(&ms, "P:1:short:0:1:1:1:1:1:1:450");
    
    TEST_ASSERT_EQUAL(50, ms.last_latency_ms);
    TEST_ASSERT_EQUAL(50, ms.avg_latency_ms);
    TEST_ASSERT_EQUAL(50, msg_get_latency(&ms));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_handle_hello_valid);
    RUN_TEST(test_handle_hello_malformed);
    RUN_TEST(test_handle_hello_forward_compat);
    RUN_TEST(test_msg_update_latency);
    return UNITY_END();
}
