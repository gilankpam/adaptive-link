/**
 * @file test_util.c
 * @brief Unity unit tests for util.c utility functions.
 *
 * This test suite covers all functions in util.c including:
 * - String manipulation: strip_newline, trim_whitespace, normalize_whitespace
 * - Time functions: get_monotonic_time, now_ms, elapsed_ms_timespec, elapsed_ms_timeval
 * - URL parsing: parse_url
 *
 * Mock clock_gettime() is implemented for deterministic time function testing.
 */

#include "unity.h"
#include "util.h"
#include "alink_types.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <limits.h>
#include <dlfcn.h>

/* ============================================================
 * Mock clock_gettime() for deterministic time testing
 * ============================================================ */

static struct timespec g_mock_timespec = {0, 0};
static int g_mock_enabled = 0;

/* Function pointer to the real clock_gettime */
typedef int (*clock_gettime_fn)(clockid_t clk, struct timespec *ts);
static clock_gettime_fn real_clock_gettime = NULL;

/* Initialize the real clock_gettime function pointer */
static void init_real_clock_gettime(void) {
    if (real_clock_gettime == NULL) {
        real_clock_gettime = (clock_gettime_fn)dlsym(RTLD_NEXT, "clock_gettime");
    }
}

int clock_gettime(clockid_t clk, struct timespec *ts) {
    if (g_mock_enabled) {
        *ts = g_mock_timespec;
        return 0;
    }
    /* Fallback to real clock_gettime if mock disabled */
    init_real_clock_gettime();
    if (real_clock_gettime) {
        return real_clock_gettime(clk, ts);
    }
    return -1;
}

/* ============================================================
 * Test Setup and Teardown
 * ============================================================ */

void setUp(void) {
    g_mock_enabled = 1;
    g_mock_timespec.tv_sec = 0;
    g_mock_timespec.tv_nsec = 0;
}

void tearDown(void) {
    g_mock_enabled = 0;
}

/* ============================================================
 * Test Suite: util_strip_newline
 * ============================================================ */

void test_strip_newline_with_trailing_newline(void) {
    char str[] = "hello\n";
    util_strip_newline(str);
    TEST_ASSERT_EQUAL_STRING("hello", str);
}

void test_strip_newline_no_newline(void) {
    char str[] = "hello";
    util_strip_newline(str);
    TEST_ASSERT_EQUAL_STRING("hello", str);
}

void test_strip_newline_empty_string(void) {
    char str[] = "";
    util_strip_newline(str);
    TEST_ASSERT_EQUAL_STRING("", str);
}

void test_strip_newline_only_newline(void) {
    char str[] = "\n";
    util_strip_newline(str);
    TEST_ASSERT_EQUAL_STRING("", str);
}

void test_strip_newline_multiple_newlines(void) {
    char str[] = "hello\n\n";
    util_strip_newline(str);
    TEST_ASSERT_EQUAL_STRING("hello\n", str);
}

void test_strip_newline_carriage_return_and_newline(void) {
    char str[] = "hello\r\n";
    util_strip_newline(str);
    TEST_ASSERT_EQUAL_STRING("hello\r", str);
}

/* ============================================================
 * Test Suite: util_trim_whitespace
 * ============================================================ */

void test_trim_no_whitespace(void) {
    char str[] = "hello";
    util_trim_whitespace(str);
    TEST_ASSERT_EQUAL_STRING("hello", str);
}

void test_trim_leading_only(void) {
    char str[] = "   hello";
    util_trim_whitespace(str);
    TEST_ASSERT_EQUAL_STRING("hello", str);
}

void test_trim_trailing_only(void) {
    char str[] = "hello   ";
    util_trim_whitespace(str);
    TEST_ASSERT_EQUAL_STRING("hello", str);
}

void test_trim_both_sides(void) {
    char str[] = "   hello   ";
    util_trim_whitespace(str);
    TEST_ASSERT_EQUAL_STRING("hello", str);
}

void test_trim_only_whitespace(void) {
    char str[] = "     ";
    util_trim_whitespace(str);
    TEST_ASSERT_EQUAL_STRING("", str);
}

void test_trim_empty_string(void) {
    char str[] = "";
    util_trim_whitespace(str);
    TEST_ASSERT_EQUAL_STRING("", str);
}

void test_trim_tabs_and_spaces(void) {
    char str[] = "\t  hello\t  ";
    util_trim_whitespace(str);
    TEST_ASSERT_EQUAL_STRING("hello", str);
}

void test_trim_newlines(void) {
    char str[] = "\n\nhello\n\n";
    util_trim_whitespace(str);
    TEST_ASSERT_EQUAL_STRING("hello", str);
}

void test_trim_preserves_internal_spaces(void) {
    char str[] = "  hello world  ";
    util_trim_whitespace(str);
    TEST_ASSERT_EQUAL_STRING("hello world", str);
}

/* ============================================================
 * Test Suite: util_normalize_whitespace
 * ============================================================ */

void test_normalize_no_multiple_spaces(void) {
    char str[] = "hello world";
    util_normalize_whitespace(str);
    TEST_ASSERT_EQUAL_STRING("hello world", str);
}

void test_normalize_multiple_spaces(void) {
    char str[] = "hello    world";
    util_normalize_whitespace(str);
    TEST_ASSERT_EQUAL_STRING("hello world", str);
}

void test_normalize_leading_spaces(void) {
    char str[] = "    hello";
    util_normalize_whitespace(str);
    TEST_ASSERT_EQUAL_STRING(" hello", str);
}

void test_normalize_trailing_spaces(void) {
    char str[] = "hello    ";
    util_normalize_whitespace(str);
    TEST_ASSERT_EQUAL_STRING("hello ", str);
}

void test_normalize_mixed_whitespace(void) {
    char str[] = "hello\t\t  \n  world";
    util_normalize_whitespace(str);
    TEST_ASSERT_EQUAL_STRING("hello world", str);
}

void test_normalize_empty_string(void) {
    char str[] = "";
    util_normalize_whitespace(str);
    TEST_ASSERT_EQUAL_STRING("", str);
}

void test_normalize_only_whitespace(void) {
    char str[] = "   \t\t   ";
    util_normalize_whitespace(str);
    TEST_ASSERT_EQUAL_STRING(" ", str);
}

void test_normalize_single_char(void) {
    char str[] = "a";
    util_normalize_whitespace(str);
    TEST_ASSERT_EQUAL_STRING("a", str);
}

void test_normalize_all_spaces(void) {
    char str[] = "        ";
    util_normalize_whitespace(str);
    TEST_ASSERT_EQUAL_STRING(" ", str);
}

/* ============================================================
 * Test Suite: util_get_monotonic_time
 * ============================================================ */

void test_monotonic_time_returns_seconds(void) {
    g_mock_timespec.tv_sec = 12345;
    g_mock_timespec.tv_nsec = 987654321;
    
    long result = util_get_monotonic_time();
    TEST_ASSERT_EQUAL(12345, result);
}

void test_monotonic_time_zero_time(void) {
    g_mock_timespec.tv_sec = 0;
    g_mock_timespec.tv_nsec = 0;
    
    long result = util_get_monotonic_time();
    TEST_ASSERT_EQUAL(0, result);
}

void test_monotonic_time_large_value(void) {
    g_mock_timespec.tv_sec = 2147483647L;  /* LONG_MAX */
    g_mock_timespec.tv_nsec = 0;
    
    long result = util_get_monotonic_time();
    TEST_ASSERT_EQUAL(2147483647L, result);
}

/* ============================================================
 * Test Suite: util_now_ms
 * ============================================================ */

void test_now_ms_returns_milliseconds(void) {
    g_mock_timespec.tv_sec = 1000;
    g_mock_timespec.tv_nsec = 500000000;  /* 500ms */
    
    uint64_t result = util_now_ms();
    TEST_ASSERT_EQUAL_UINT64(1000500ULL, result);
}

void test_now_ms_zero_time(void) {
    g_mock_timespec.tv_sec = 0;
    g_mock_timespec.tv_nsec = 0;
    
    uint64_t result = util_now_ms();
    TEST_ASSERT_EQUAL_UINT64(0ULL, result);
}

void test_now_ms_nanosecond_precision(void) {
    g_mock_timespec.tv_sec = 100;
    g_mock_timespec.tv_nsec = 123456789;  /* 123ms + remainder */
    
    uint64_t result = util_now_ms();
    TEST_ASSERT_EQUAL_UINT64(100123ULL, result);
}

void test_now_ms_999_nanoseconds(void) {
    g_mock_timespec.tv_sec = 100;
    g_mock_timespec.tv_nsec = 999999999;  /* 999ms */
    
    uint64_t result = util_now_ms();
    TEST_ASSERT_EQUAL_UINT64(100999ULL, result);
}

/* ============================================================
 * Test Suite: util_elapsed_ms_timespec
 * ============================================================ */

void test_elapsed_timespec_positive_simple(void) {
    struct timespec past = {1000, 0};
    struct timespec current = {1002, 0};
    
    long result = util_elapsed_ms_timespec(&current, &past);
    TEST_ASSERT_EQUAL(2000, result);
}

void test_elapsed_timespec_with_nanosecond_conversion(void) {
    struct timespec past = {1000, 0};
    struct timespec current = {1000, 500000000};  /* 500ms later */
    
    long result = util_elapsed_ms_timespec(&current, &past);
    TEST_ASSERT_EQUAL(500, result);
}

void test_elapsed_timespec_negative_returns_zero(void) {
    struct timespec past = {1002, 0};
    struct timespec current = {1000, 0};
    
    long result = util_elapsed_ms_timespec(&current, &past);
    TEST_ASSERT_EQUAL(0, result);
}

void test_elapsed_timespec_overflow_returns_LONG_MAX(void) {
    struct timespec past = {0, 0};
    struct timespec current = {10000000000000000LL, 0};  /* Would overflow long long */
    
    long result = util_elapsed_ms_timespec(&current, &past);
    TEST_ASSERT_EQUAL(LONG_MAX, result);
}

void test_elapsed_timespec_zero_duration(void) {
    struct timespec past = {1000, 500000000};
    struct timespec current = {1000, 500000000};
    
    long result = util_elapsed_ms_timespec(&current, &past);
    TEST_ASSERT_EQUAL(0, result);
}

void test_elapsed_timespec_negative_nsec_handling(void) {
    struct timespec past = {1000, 900000000};  /* 900ms */
    struct timespec current = {1001, 100000000}; /* 100ms (100.1s total) */
    
    long result = util_elapsed_ms_timespec(&current, &past);
    /* 1001.1 - 1000.9 = 0.2s = 200ms */
    TEST_ASSERT_EQUAL(200, result);
}

void test_elapsed_timespec_mixed_sec_and_nsec(void) {
    struct timespec past = {1000, 100000000};  /* 1000.1s */
    struct timespec current = {1001, 600000000}; /* 1001.6s */
    
    long result = util_elapsed_ms_timespec(&current, &past);
    /* 1001.6 - 1000.1 = 1.5s = 1500ms */
    TEST_ASSERT_EQUAL(1500, result);
}

/* ============================================================
 * Test Suite: util_elapsed_ms_timeval
 * ============================================================ */

void test_elapsed_timeval_positive_simple(void) {
    struct timeval past = {1000, 0};
    struct timeval current = {1002, 0};
    
    long result = util_elapsed_ms_timeval(&current, &past);
    TEST_ASSERT_EQUAL(2000, result);
}

void test_elapsed_timeval_with_microsecond_conversion(void) {
    struct timeval past = {1000, 0};
    struct timeval current = {1000, 500000};  /* 500ms later */
    
    long result = util_elapsed_ms_timeval(&current, &past);
    TEST_ASSERT_EQUAL(500, result);
}

void test_elapsed_timeval_negative_returns_zero(void) {
    struct timeval past = {1002, 0};
    struct timeval current = {1000, 0};
    
    long result = util_elapsed_ms_timeval(&current, &past);
    TEST_ASSERT_EQUAL(0, result);
}

void test_elapsed_timeval_overflow_returns_LONG_MAX(void) {
    struct timeval past = {0, 0};
    struct timeval current = {10000000000000000LL, 0};  /* Would overflow long long */
    
    long result = util_elapsed_ms_timeval(&current, &past);
    TEST_ASSERT_EQUAL(LONG_MAX, result);
}

void test_elapsed_timeval_zero_duration(void) {
    struct timeval past = {1000, 500000};
    struct timeval current = {1000, 500000};
    
    long result = util_elapsed_ms_timeval(&current, &past);
    TEST_ASSERT_EQUAL(0, result);
}

void test_elapsed_timeval_negative_usec_handling(void) {
    struct timeval past = {1000, 900000};  /* 900ms */
    struct timeval current = {1001, 100000}; /* 100ms (100.1s total) */
    
    long result = util_elapsed_ms_timeval(&current, &past);
    /* 1001.1 - 1000.9 = 0.2s = 200ms */
    TEST_ASSERT_EQUAL(200, result);
}

void test_elapsed_timeval_mixed_sec_and_usec(void) {
    struct timeval past = {1000, 100000};  /* 1000.1s */
    struct timeval current = {1001, 600000}; /* 1001.6s */
    
    long result = util_elapsed_ms_timeval(&current, &past);
    /* 1001.6 - 1000.1 = 1.5s = 1500ms */
    TEST_ASSERT_EQUAL(1500, result);
}

/* ============================================================
 * Test Suite: util_parse_url
 * ============================================================ */

void test_parse_url_full_with_port_and_path(void) {
    char host[64];
    int port;
    char path[256];
    
    int result = util_parse_url("http://example.com:8080/api/test", 
                                host, sizeof(host), &port, path, sizeof(path));
    
    TEST_ASSERT_EQUAL(0, result);
    TEST_ASSERT_EQUAL_STRING("example.com", host);
    TEST_ASSERT_EQUAL(8080, port);
    TEST_ASSERT_EQUAL_STRING("/api/test", path);
}

void test_parse_url_no_port(void) {
    char host[64];
    int port;
    char path[256];
    
    int result = util_parse_url("http://example.com/path", 
                                host, sizeof(host), &port, path, sizeof(path));
    
    TEST_ASSERT_EQUAL(0, result);
    TEST_ASSERT_EQUAL_STRING("example.com", host);
    TEST_ASSERT_EQUAL(80, port);
    TEST_ASSERT_EQUAL_STRING("/path", path);
}

void test_parse_url_no_path(void) {
    char host[64];
    int port;
    char path[256];
    
    int result = util_parse_url("http://example.com:8080", 
                                host, sizeof(host), &port, path, sizeof(path));
    
    TEST_ASSERT_EQUAL(0, result);
    TEST_ASSERT_EQUAL_STRING("example.com", host);
    TEST_ASSERT_EQUAL(8080, port);
    TEST_ASSERT_EQUAL_STRING("", path);
}

void test_parse_url_https_default_port(void) {
    char host[64];
    int port;
    char path[256];
    
    int result = util_parse_url("https://example.com/", 
                                host, sizeof(host), &port, path, sizeof(path));
    
    TEST_ASSERT_EQUAL(0, result);
    TEST_ASSERT_EQUAL_STRING("example.com", host);
    TEST_ASSERT_EQUAL(443, port);
    TEST_ASSERT_EQUAL_STRING("/", path);
}

void test_parse_url_with_query_string(void) {
    char host[64];
    int port;
    char path[256];
    
    int result = util_parse_url("http://host.com/path?query=value&foo=bar", 
                                host, sizeof(host), &port, path, sizeof(path));
    
    TEST_ASSERT_EQUAL(0, result);
    TEST_ASSERT_EQUAL_STRING("host.com", host);
    TEST_ASSERT_EQUAL(80, port);
    TEST_ASSERT_EQUAL_STRING("/path?query=value&foo=bar", path);
}

void test_parse_url_null_url_returns_error(void) {
    char host[64];
    int port;
    char path[256];
    
    int result = util_parse_url(NULL, host, sizeof(host), &port, path, sizeof(path));
    TEST_ASSERT_EQUAL(-1, result);
}

void test_parse_url_null_host_returns_error(void) {
    int port;
    char path[256];
    
    int result = util_parse_url("http://example.com", NULL, 0, &port, path, sizeof(path));
    TEST_ASSERT_EQUAL(-1, result);
}

void test_parse_url_null_port_returns_error(void) {
    char host[64];
    char path[256];
    
    int result = util_parse_url("http://example.com", host, sizeof(host), NULL, path, sizeof(path));
    TEST_ASSERT_EQUAL(-1, result);
}

void test_parse_url_null_path_returns_error(void) {
    char host[64];
    int port;
    
    int result = util_parse_url("http://example.com", host, sizeof(host), &port, NULL, 0);
    TEST_ASSERT_EQUAL(-1, result);
}

void test_parse_url_invalid_port_too_large(void) {
    char host[64];
    int port;
    char path[256];
    
    int result = util_parse_url("http://example.com:99999/path", 
                                host, sizeof(host), &port, path, sizeof(path));
    
    TEST_ASSERT_EQUAL(0, result);
    TEST_ASSERT_EQUAL_STRING("example.com", host);
    TEST_ASSERT_EQUAL(80, port);  /* Invalid port, defaults to 80 */
    TEST_ASSERT_EQUAL_STRING("/path", path);
}

void test_parse_url_invalid_port_zero(void) {
    char host[64];
    int port;
    char path[256];
    
    int result = util_parse_url("http://example.com:0/path", 
                                host, sizeof(host), &port, path, sizeof(path));
    
    TEST_ASSERT_EQUAL(0, result);
    TEST_ASSERT_EQUAL_STRING("example.com", host);
    TEST_ASSERT_EQUAL(80, port);  /* Invalid port, defaults to 80 */
}

void test_parse_url_localhost_defaults(void) {
    char host[64];
    int port;
    char path[256];
    
    int result = util_parse_url("/path", host, sizeof(host), &port, path, sizeof(path));
    
    TEST_ASSERT_EQUAL(0, result);
    TEST_ASSERT_EQUAL_STRING("localhost", host);
    TEST_ASSERT_EQUAL(80, port);
    TEST_ASSERT_EQUAL_STRING("/path", path);
}

void test_parse_url_empty_path(void) {
    char host[64];
    int port;
    char path[256];
    
    int result = util_parse_url("http://host.com", host, sizeof(host), &port, path, sizeof(path));
    
    TEST_ASSERT_EQUAL(0, result);
    TEST_ASSERT_EQUAL_STRING("host.com", host);
    TEST_ASSERT_EQUAL(80, port);
    TEST_ASSERT_EQUAL_STRING("", path);
}

void test_parse_url_just_slash_path(void) {
    char host[64];
    int port;
    char path[256];
    
    int result = util_parse_url("http://host.com/", host, sizeof(host), &port, path, sizeof(path));
    
    TEST_ASSERT_EQUAL(0, result);
    TEST_ASSERT_EQUAL_STRING("host.com", host);
    TEST_ASSERT_EQUAL(80, port);
    TEST_ASSERT_EQUAL_STRING("/", path);
}

void test_parse_url_host_only_no_scheme(void) {
    char host[64];
    int port;
    char path[256];
    
    int result = util_parse_url("example.com", host, sizeof(host), &port, path, sizeof(path));
    
    TEST_ASSERT_EQUAL(0, result);
    TEST_ASSERT_EQUAL_STRING("example.com", host);
    TEST_ASSERT_EQUAL(80, port);
    TEST_ASSERT_EQUAL_STRING("", path);
}

void test_parse_url_buffer_overflow_protection(void) {
    char host[8];  /* Small buffer */
    int port;
    char path[256];
    
    int result = util_parse_url("http://verylonghostnamethatexceedsbuffer.com:8080/path", 
                                host, sizeof(host), &port, path, sizeof(path));
    
    TEST_ASSERT_EQUAL(0, result);
    TEST_ASSERT_LESS_OR_EQUAL(7, strlen(host));  /* Should be truncated */
    TEST_ASSERT_EQUAL(8080, port);
}

void test_parse_url_ipv4_address(void) {
    char host[64];
    int port;
    char path[256];
    
    int result = util_parse_url("http://192.168.1.1:8080/api", 
                                host, sizeof(host), &port, path, sizeof(path));
    
    TEST_ASSERT_EQUAL(0, result);
    TEST_ASSERT_EQUAL_STRING("192.168.1.1", host);
    TEST_ASSERT_EQUAL(8080, port);
    TEST_ASSERT_EQUAL_STRING("/api", path);
}

void test_parse_url_with_fragment(void) {
    char host[64];
    int port;
    char path[256];
    
    int result = util_parse_url("http://example.com/path#section", 
                                host, sizeof(host), &port, path, sizeof(path));
    
    TEST_ASSERT_EQUAL(0, result);
    TEST_ASSERT_EQUAL_STRING("example.com", host);
    TEST_ASSERT_EQUAL(80, port);
    TEST_ASSERT_EQUAL_STRING("/path#section", path);
}

/* ============================================================
 * util_venc_parse_int_value tests
 * ============================================================ */

void test_venc_parse_int_typical(void) {
    int val = 0;
    int ret = util_venc_parse_int_value(
        "{\"ok\":true,\"data\":{\"field\":\"video0.fps\",\"value\":60}}", &val);
    TEST_ASSERT_EQUAL(0, ret);
    TEST_ASSERT_EQUAL(60, val);
}

void test_venc_parse_int_zero(void) {
    int val = -1;
    int ret = util_venc_parse_int_value(
        "{\"ok\":true,\"data\":{\"field\":\"video0.fps\",\"value\":0}}", &val);
    TEST_ASSERT_EQUAL(0, ret);
    TEST_ASSERT_EQUAL(0, val);
}

void test_venc_parse_int_negative(void) {
    int val = 0;
    int ret = util_venc_parse_int_value(
        "{\"ok\":true,\"data\":{\"field\":\"fpv.roiQp\",\"value\":-12}}", &val);
    TEST_ASSERT_EQUAL(0, ret);
    TEST_ASSERT_EQUAL(-12, val);
}

void test_venc_parse_int_no_value_key(void) {
    int val = 0;
    int ret = util_venc_parse_int_value("{\"ok\":true,\"data\":{}}", &val);
    TEST_ASSERT_EQUAL(-1, ret);
}

void test_venc_parse_int_value_is_string(void) {
    int val = 0;
    int ret = util_venc_parse_int_value(
        "{\"ok\":true,\"data\":{\"value\":\"notanint\"}}", &val);
    TEST_ASSERT_EQUAL(-1, ret);
}

void test_venc_parse_int_whitespace_after_colon(void) {
    int val = 0;
    int ret = util_venc_parse_int_value(
        "{\"ok\":true,\"data\":{\"value\": 90}}", &val);
    TEST_ASSERT_EQUAL(0, ret);
    TEST_ASSERT_EQUAL(90, val);
}

/* ============================================================
 * util_venc_parse_str_value tests
 * ============================================================ */

void test_venc_parse_str_typical(void) {
    char out[32];
    int ret = util_venc_parse_str_value(
        "{\"ok\":true,\"data\":{\"field\":\"video0.size\",\"value\":\"1920x1080\"}}",
        out, sizeof(out));
    TEST_ASSERT_EQUAL(0, ret);
    TEST_ASSERT_EQUAL_STRING("1920x1080", out);
}

void test_venc_parse_str_path(void) {
    char out[128];
    int ret = util_venc_parse_str_value(
        "{\"ok\":true,\"data\":{\"field\":\"isp.sensorBin\","
        "\"value\":\"/etc/sensors/imx415_infinity6e.bin\"}}",
        out, sizeof(out));
    TEST_ASSERT_EQUAL(0, ret);
    TEST_ASSERT_EQUAL_STRING("/etc/sensors/imx415_infinity6e.bin", out);
}

void test_venc_parse_str_no_value_key(void) {
    char out[32];
    int ret = util_venc_parse_str_value("{\"ok\":true,\"data\":{}}", out, sizeof(out));
    TEST_ASSERT_EQUAL(-1, ret);
}

void test_venc_parse_str_value_is_int(void) {
    char out[32];
    int ret = util_venc_parse_str_value(
        "{\"ok\":true,\"data\":{\"value\":60}}", out, sizeof(out));
    TEST_ASSERT_EQUAL(-1, ret);
}

void test_venc_parse_str_truncates_to_buffer(void) {
    char out[5]; /* smaller than the actual value */
    int ret = util_venc_parse_str_value(
        "{\"ok\":true,\"data\":{\"value\":\"1920x1080\"}}", out, sizeof(out));
    TEST_ASSERT_EQUAL(0, ret);
    TEST_ASSERT_EQUAL_STRING("1920", out); /* truncated to 4 chars + NUL */
}

void test_venc_parse_str_whitespace_after_colon(void) {
    char out[32];
    int ret = util_venc_parse_str_value(
        "{\"ok\":true,\"data\":{\"value\": \"1280x720\"}}", out, sizeof(out));
    TEST_ASSERT_EQUAL(0, ret);
    TEST_ASSERT_EQUAL_STRING("1280x720", out);
}

/* ============================================================
 * Main - Test Runner
 * ============================================================ */

int main(void) {
    UNITY_BEGIN();
    
    /* String manipulation tests */
    RUN_TEST(test_strip_newline_with_trailing_newline);
    RUN_TEST(test_strip_newline_no_newline);
    RUN_TEST(test_strip_newline_empty_string);
    RUN_TEST(test_strip_newline_only_newline);
    RUN_TEST(test_strip_newline_multiple_newlines);
    RUN_TEST(test_strip_newline_carriage_return_and_newline);
    
    RUN_TEST(test_trim_no_whitespace);
    RUN_TEST(test_trim_leading_only);
    RUN_TEST(test_trim_trailing_only);
    RUN_TEST(test_trim_both_sides);
    RUN_TEST(test_trim_only_whitespace);
    RUN_TEST(test_trim_empty_string);
    RUN_TEST(test_trim_tabs_and_spaces);
    RUN_TEST(test_trim_newlines);
    RUN_TEST(test_trim_preserves_internal_spaces);
    
    RUN_TEST(test_normalize_no_multiple_spaces);
    RUN_TEST(test_normalize_multiple_spaces);
    RUN_TEST(test_normalize_leading_spaces);
    RUN_TEST(test_normalize_trailing_spaces);
    RUN_TEST(test_normalize_mixed_whitespace);
    RUN_TEST(test_normalize_empty_string);
    RUN_TEST(test_normalize_only_whitespace);
    RUN_TEST(test_normalize_single_char);
    RUN_TEST(test_normalize_all_spaces);
    
    /* Time function tests */
    RUN_TEST(test_monotonic_time_returns_seconds);
    RUN_TEST(test_monotonic_time_zero_time);
    RUN_TEST(test_monotonic_time_large_value);
    
    RUN_TEST(test_now_ms_returns_milliseconds);
    RUN_TEST(test_now_ms_zero_time);
    RUN_TEST(test_now_ms_nanosecond_precision);
    RUN_TEST(test_now_ms_999_nanoseconds);
    
    /* Elapsed time tests */
    RUN_TEST(test_elapsed_timespec_positive_simple);
    RUN_TEST(test_elapsed_timespec_with_nanosecond_conversion);
    RUN_TEST(test_elapsed_timespec_negative_returns_zero);
    RUN_TEST(test_elapsed_timespec_overflow_returns_LONG_MAX);
    RUN_TEST(test_elapsed_timespec_zero_duration);
    RUN_TEST(test_elapsed_timespec_negative_nsec_handling);
    RUN_TEST(test_elapsed_timespec_mixed_sec_and_nsec);
    
    RUN_TEST(test_elapsed_timeval_positive_simple);
    RUN_TEST(test_elapsed_timeval_with_microsecond_conversion);
    RUN_TEST(test_elapsed_timeval_negative_returns_zero);
    RUN_TEST(test_elapsed_timeval_overflow_returns_LONG_MAX);
    RUN_TEST(test_elapsed_timeval_zero_duration);
    RUN_TEST(test_elapsed_timeval_negative_usec_handling);
    RUN_TEST(test_elapsed_timeval_mixed_sec_and_usec);
    
    /* URL parsing tests */
    RUN_TEST(test_parse_url_full_with_port_and_path);
    RUN_TEST(test_parse_url_no_port);
    RUN_TEST(test_parse_url_no_path);
    RUN_TEST(test_parse_url_https_default_port);
    RUN_TEST(test_parse_url_with_query_string);
    RUN_TEST(test_parse_url_null_url_returns_error);
    RUN_TEST(test_parse_url_null_host_returns_error);
    RUN_TEST(test_parse_url_null_port_returns_error);
    RUN_TEST(test_parse_url_null_path_returns_error);
    RUN_TEST(test_parse_url_invalid_port_too_large);
    RUN_TEST(test_parse_url_invalid_port_zero);
    RUN_TEST(test_parse_url_localhost_defaults);
    RUN_TEST(test_parse_url_empty_path);
    RUN_TEST(test_parse_url_just_slash_path);
    RUN_TEST(test_parse_url_host_only_no_scheme);
    RUN_TEST(test_parse_url_buffer_overflow_protection);
    RUN_TEST(test_parse_url_ipv4_address);
    RUN_TEST(test_parse_url_with_fragment);

    /* venc int value parsing tests */
    RUN_TEST(test_venc_parse_int_typical);
    RUN_TEST(test_venc_parse_int_zero);
    RUN_TEST(test_venc_parse_int_negative);
    RUN_TEST(test_venc_parse_int_no_value_key);
    RUN_TEST(test_venc_parse_int_value_is_string);
    RUN_TEST(test_venc_parse_int_whitespace_after_colon);

    /* venc string value parsing tests */
    RUN_TEST(test_venc_parse_str_typical);
    RUN_TEST(test_venc_parse_str_path);
    RUN_TEST(test_venc_parse_str_no_value_key);
    RUN_TEST(test_venc_parse_str_value_is_int);
    RUN_TEST(test_venc_parse_str_truncates_to_buffer);
    RUN_TEST(test_venc_parse_str_whitespace_after_colon);

    return UNITY_END();
}