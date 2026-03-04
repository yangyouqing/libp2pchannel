/*
 * test_p2pav_stress.c -- Stability, robustness, and stress tests for libp2pav.
 *
 * Tests:
 *   Stability:  rapid_create_destroy, rapid_start_stop, multi_session,
 *               long_running, subscriber_rejoin
 *   Robustness: null_params, send_before_start, send_after_stop,
 *               double_stop_destroy, invalid_token, pub_crash, kick_resend
 *   Stress:     large_video_frame, large_data, high_freq_send, max_subscribers
 *
 * Usage:
 *   ./test_p2pav_stress [--signaling HOST:PORT] [--cert FILE] [--key FILE]
 *                       [--token JWT] [--token-sub JWT] [--token-sub2 JWT]
 *                       [--token-sub3..10 JWT]
 */

#include "p2pav.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>

#ifndef _WIN32
#include <sys/time.h>
#endif

#define TEST_TIMEOUT_MS      15000
#define WAIT_POLL_MS         50
#define US_PER_MS            1000
#define VIDEO_FRAME_SIZE     4096
#define AUDIO_FRAME_SIZE     160
#define MAX_SUB_TOKENS       10

typedef struct {
    const char *signaling_url;
    const char *ssl_cert;
    const char *ssl_key;
    const char *token_pub;
    const char *token_sub[MAX_SUB_TOKENS];
    int         num_sub_tokens;
    unsigned long run_suffix;
} test_env_t;

typedef int (*test_func_t)(const test_env_t *env);

typedef struct {
    const char  *name;
    test_func_t  func;
} test_case_t;

static volatile int g_running = 1;
static volatile int g_peer_ready = 0;

static void on_signal(int sig) { (void)sig; g_running = 0; }

static uint64_t now_us(void)
{
#ifdef _WIN32
    return (uint64_t)clock() * (1000000ULL / CLOCKS_PER_SEC);
#else
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000000ULL + (uint64_t)tv.tv_usec;
#endif
}

static int wait_flag_timeout(volatile int *flag, int timeout_ms)
{
    int elapsed = 0;
    while (!*flag && elapsed < timeout_ms && g_running) {
        usleep((unsigned)WAIT_POLL_MS * US_PER_MS);
        elapsed += WAIT_POLL_MS;
    }
    return *flag ? 0 : -1;
}

static void sleep_ms(int ms)
{
    usleep((unsigned)(ms) * US_PER_MS);
}

static void on_peer_ready_cb(p2pav_session_t *s, const char *peer_id, void *ud)
{
    (void)s; (void)peer_id; (void)ud;
    g_peer_ready = 1;
}

static void make_pub_cfg(p2pav_session_config_t *cfg, const test_env_t *env,
                         const char *room, const char *peer_id)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->signaling_url = env->signaling_url;
    cfg->auth_token    = env->token_pub;
    cfg->room_id       = room;
    cfg->peer_id       = peer_id;
    cfg->role          = P2PAV_ROLE_PUBLISHER;
    cfg->stun_server   = "127.0.0.1:3478";
    cfg->ssl_cert_file = env->ssl_cert;
    cfg->ssl_key_file  = env->ssl_key;
}

static void make_sub_cfg(p2pav_session_config_t *cfg, const test_env_t *env,
                         int sub_idx, const char *room, const char *peer_id)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->signaling_url = env->signaling_url;
    cfg->auth_token    = (sub_idx < env->num_sub_tokens)
                          ? env->token_sub[sub_idx] : env->token_sub[0];
    cfg->room_id       = room;
    cfg->peer_id       = peer_id;
    cfg->role          = P2PAV_ROLE_SUBSCRIBER;
    cfg->stun_server   = "127.0.0.1:3478";
}

static void send_dummy_video(p2pav_session_t *pub, int count, int interval_us)
{
    uint8_t buf[VIDEO_FRAME_SIZE];
    memset(buf, 0x42, sizeof(buf));
    for (int i = 0; i < count && g_running; i++) {
        p2pav_video_frame_t vf = {
            .data = buf, .size = (int)sizeof(buf),
            .timestamp_us = (uint64_t)i * 33333,
            .is_keyframe = (i % 30 == 0) ? 1 : 0,
        };
        p2pav_video_send(pub, &vf);
        if (interval_us > 0) usleep(interval_us);
    }
}

/* =================================================================
 *  TC-S1: rapid_create_destroy -- cycle create/start/stop/destroy 50x
 * ================================================================= */
static int test_rapid_create_destroy(const test_env_t *env)
{
    for (int i = 0; i < 50 && g_running; i++) {
        char room[64];
        snprintf(room, sizeof(room), "stress_s1_%lu_%d", env->run_suffix, i);

        p2pav_session_config_t cfg;
        make_pub_cfg(&cfg, env, room, "pub1");
        p2pav_session_t *s = p2pav_session_create(&cfg);
        if (!s) return 1;

        if (p2pav_session_start(s) != P2PAV_OK) {
            p2pav_session_destroy(s);
            return 1;
        }
        sleep_ms(200);
        p2pav_session_stop(s);
        p2pav_session_destroy(s);
    }
    printf("  (50 cycles OK)\n");
    return 0;
}

/* =================================================================
 *  TC-S2: rapid_start_stop -- start/stop same sessions 20x
 * ================================================================= */
static int test_rapid_start_stop(const test_env_t *env)
{
    int ok_count = 0;
    for (int i = 0; i < 20 && g_running; i++) {
        char room[64];
        snprintf(room, sizeof(room), "stress_s2_%lu_%d", env->run_suffix, i);

        p2pav_session_config_t pub_cfg, sub_cfg;
        make_pub_cfg(&pub_cfg, env, room, "pub1");
        make_sub_cfg(&sub_cfg, env, 0, room, "sub1");

        p2pav_session_t *pub = p2pav_session_create(&pub_cfg);
        p2pav_session_t *sub = p2pav_session_create(&sub_cfg);
        if (!pub || !sub) {
            if (pub) p2pav_session_destroy(pub);
            if (sub) p2pav_session_destroy(sub);
            return 1;
        }

        p2pav_error_t r1 = p2pav_session_start(pub);
        p2pav_error_t r2 = p2pav_session_start(sub);
        if (r1 == P2PAV_OK && r2 == P2PAV_OK) ok_count++;
        sleep_ms(50);
        p2pav_session_stop(pub);
        p2pav_session_stop(sub);
        p2pav_session_destroy(pub);
        p2pav_session_destroy(sub);
    }
    printf("  (%d/20 start OK)\n", ok_count);
    return (ok_count == 20) ? 0 : 1;
}

/* =================================================================
 *  TC-S3: multi_session -- 3 sequential pub+sub pairs in separate rooms
 *  (Runs sequentially to avoid ICE port conflicts on same host)
 * ================================================================= */
static volatile int s3_recv[3];

static void s3_on_video_0(p2pav_session_t *s, const char *f,
    const p2pav_video_frame_t *fr, void *u) { (void)s;(void)f;(void)fr;(void)u; s3_recv[0]++; }
static void s3_on_video_1(p2pav_session_t *s, const char *f,
    const p2pav_video_frame_t *fr, void *u) { (void)s;(void)f;(void)fr;(void)u; s3_recv[1]++; }
static void s3_on_video_2(p2pav_session_t *s, const char *f,
    const p2pav_video_frame_t *fr, void *u) { (void)s;(void)f;(void)fr;(void)u; s3_recv[2]++; }

static int test_multi_session(const test_env_t *env)
{
    p2pav_on_video_frame_t cbs[3] = { s3_on_video_0, s3_on_video_1, s3_on_video_2 };
    s3_recv[0] = s3_recv[1] = s3_recv[2] = 0;

    p2pav_video_config_t vcfg = {
        .codec = P2PAV_CODEC_H264, .width = 640, .height = 480, .fps = 30,
        .bitrate_mode = P2PAV_BITRATE_MEDIUM, .reliability = P2PAV_SEMI_RELIABLE,
    };

    if (env->num_sub_tokens < 3) {
        printf("  (skipped: need 3 sub tokens, have %d)\n", env->num_sub_tokens);
        return 0;
    }

    for (int i = 0; i < 3 && g_running; i++) {
        char room[64], sub_id[16];
        snprintf(room, sizeof(room), "stress_s3_%lu_%d", env->run_suffix, i);
        snprintf(sub_id, sizeof(sub_id), "sub%d", i + 1);
        g_peer_ready = 0;

        p2pav_session_config_t pc, sc;
        make_pub_cfg(&pc, env, room, "pub1");
        make_sub_cfg(&sc, env, i, room, sub_id);

        p2pav_session_t *pub = p2pav_session_create(&pc);
        p2pav_session_t *sub = p2pav_session_create(&sc);
        if (!pub || !sub) {
            if (pub) p2pav_session_destroy(pub);
            if (sub) p2pav_session_destroy(sub);
            return 1;
        }

        p2pav_video_set_config(pub, &vcfg);
        p2pav_video_set_config(sub, &vcfg);
        p2pav_video_set_recv_callback(sub, cbs[i], NULL);

        p2pav_session_callbacks_t scb = {0};
        scb.on_peer_ready = on_peer_ready_cb;
        p2pav_session_set_callbacks(sub, &scb, NULL);

        if (p2pav_session_start(pub) != P2PAV_OK || p2pav_session_start(sub) != P2PAV_OK) {
            p2pav_session_stop(pub); p2pav_session_stop(sub);
            p2pav_session_destroy(pub); p2pav_session_destroy(sub);
            return 1;
        }

        if (wait_flag_timeout(&g_peer_ready, TEST_TIMEOUT_MS) != 0) {
            p2pav_session_stop(pub); p2pav_session_stop(sub);
            p2pav_session_destroy(pub); p2pav_session_destroy(sub);
            return 1;
        }

        send_dummy_video(pub, 100, 33000);
        sleep_ms(500);

        p2pav_session_stop(pub); p2pav_session_stop(sub);
        p2pav_session_destroy(pub); p2pav_session_destroy(sub);
    }

    int all_ok = (s3_recv[0] > 0 && s3_recv[1] > 0 && s3_recv[2] > 0);
    printf("  (recv=%d,%d,%d)\n", s3_recv[0], s3_recv[1], s3_recv[2]);
    return all_ok ? 0 : 1;
}

/* =================================================================
 *  TC-S4: long_running -- 60 second continuous stream
 * ================================================================= */
static volatile int s4_video_recv;
static volatile int s4_audio_recv;

static void s4_on_video(p2pav_session_t *s, const char *f,
    const p2pav_video_frame_t *fr, void *u) { (void)s;(void)f;(void)fr;(void)u; s4_video_recv++; }
static void s4_on_audio(p2pav_session_t *s, const char *f,
    const p2pav_audio_frame_t *fr, void *u) { (void)s;(void)f;(void)fr;(void)u; s4_audio_recv++; }

static int test_long_running(const test_env_t *env)
{
    char room[64];
    snprintf(room, sizeof(room), "stress_s4_%lu", env->run_suffix);

    s4_video_recv = 0;
    s4_audio_recv = 0;
    g_peer_ready = 0;

    p2pav_session_config_t pc, sc;
    make_pub_cfg(&pc, env, room, "pub1");
    make_sub_cfg(&sc, env, 0, room, "sub1");

    p2pav_session_t *pub = p2pav_session_create(&pc);
    p2pav_session_t *sub = p2pav_session_create(&sc);
    if (!pub || !sub) return 1;

    p2pav_video_config_t vcfg = {
        .codec = P2PAV_CODEC_H264, .width = 640, .height = 480, .fps = 30,
        .bitrate_mode = P2PAV_BITRATE_MEDIUM, .reliability = P2PAV_SEMI_RELIABLE,
    };
    p2pav_audio_config_t acfg = {
        .codec = P2PAV_CODEC_OPUS, .sample_rate = 48000, .channels = 1,
        .bitrate_bps = 64000, .frame_duration_ms = 20, .reliability = P2PAV_UNRELIABLE,
    };
    p2pav_video_set_config(pub, &vcfg); p2pav_video_set_config(sub, &vcfg);
    p2pav_audio_set_config(pub, &acfg); p2pav_audio_set_config(sub, &acfg);
    p2pav_video_set_recv_callback(sub, s4_on_video, NULL);
    p2pav_audio_set_recv_callback(sub, s4_on_audio, NULL);

    p2pav_session_callbacks_t scb = {0};
    scb.on_peer_ready = on_peer_ready_cb;
    p2pav_session_set_callbacks(sub, &scb, NULL);

    if (p2pav_session_start(pub) != P2PAV_OK || p2pav_session_start(sub) != P2PAV_OK) {
        p2pav_session_destroy(pub); p2pav_session_destroy(sub); return 1;
    }
    if (wait_flag_timeout(&g_peer_ready, TEST_TIMEOUT_MS) != 0) {
        p2pav_session_stop(pub); p2pav_session_stop(sub);
        p2pav_session_destroy(pub); p2pav_session_destroy(sub); return 1;
    }

    uint8_t dv[VIDEO_FRAME_SIZE], da[AUDIO_FRAME_SIZE];
    memset(dv, 0x42, sizeof(dv)); memset(da, 0x11, sizeof(da));

    int v_sent = 0, a_sent = 0;
    uint64_t t0 = now_us();
    uint64_t duration_us = 60ULL * 1000000ULL;
    while ((now_us() - t0) < duration_us && g_running) {
        p2pav_video_frame_t vf = { .data = dv, .size = (int)sizeof(dv),
            .timestamp_us = now_us(), .is_keyframe = (v_sent % 30 == 0) ? 1 : 0 };
        p2pav_video_send(pub, &vf); v_sent++;

        p2pav_audio_frame_t af = { .data = da, .size = (int)sizeof(da),
            .timestamp_us = now_us() };
        p2pav_audio_send(pub, &af); a_sent++;

        usleep(20000);
    }

    sleep_ms(500);
    int vr = s4_video_recv, ar = s4_audio_recv;

    p2pav_session_stop(pub); p2pav_session_stop(sub);
    p2pav_session_destroy(pub); p2pav_session_destroy(sub);

    float v_ratio = (v_sent > 0) ? (float)vr / (float)v_sent : 0;
    float a_ratio = (a_sent > 0) ? (float)ar / (float)a_sent : 0;
    printf("  (v=%d/%d=%.0f%%, a=%d/%d=%.0f%%)\n",
           vr, v_sent, v_ratio * 100, ar, a_sent, a_ratio * 100);
    return (v_ratio > 0.90f && a_ratio > 0.90f) ? 0 : 1;
}

/* =================================================================
 *  TC-S5: subscriber_rejoin -- sub joins/leaves 5 times
 * ================================================================= */
static volatile int s5_recv;
static void s5_on_video(p2pav_session_t *s, const char *f,
    const p2pav_video_frame_t *fr, void *u) { (void)s;(void)f;(void)fr;(void)u; s5_recv++; }

static int test_subscriber_rejoin(const test_env_t *env)
{
    char room[64];
    snprintf(room, sizeof(room), "stress_s5_%lu", env->run_suffix);

    p2pav_session_config_t pc;
    make_pub_cfg(&pc, env, room, "pub1");
    p2pav_session_t *pub = p2pav_session_create(&pc);
    if (!pub) return 1;

    p2pav_video_config_t vcfg = {
        .codec = P2PAV_CODEC_H264, .width = 640, .height = 480, .fps = 30,
        .bitrate_mode = P2PAV_BITRATE_MEDIUM, .reliability = P2PAV_SEMI_RELIABLE,
    };
    p2pav_video_set_config(pub, &vcfg);

    if (p2pav_session_start(pub) != P2PAV_OK) {
        p2pav_session_destroy(pub); return 1;
    }

    int join_ok = 0;
    for (int round = 0; round < 5 && g_running; round++) {
        s5_recv = 0;
        g_peer_ready = 0;

        p2pav_session_config_t sc;
        make_sub_cfg(&sc, env, 0, room, "sub1");
        p2pav_session_t *sub = p2pav_session_create(&sc);
        if (!sub) continue;

        p2pav_video_set_config(sub, &vcfg);
        p2pav_video_set_recv_callback(sub, s5_on_video, NULL);
        p2pav_session_callbacks_t scb = {0};
        scb.on_peer_ready = on_peer_ready_cb;
        p2pav_session_set_callbacks(sub, &scb, NULL);

        if (p2pav_session_start(sub) != P2PAV_OK) {
            p2pav_session_destroy(sub); continue;
        }
        if (wait_flag_timeout(&g_peer_ready, TEST_TIMEOUT_MS) != 0) {
            p2pav_session_stop(sub); p2pav_session_destroy(sub); continue;
        }

        send_dummy_video(pub, 60, 33000);
        sleep_ms(300);

        if (s5_recv > 0) join_ok++;
        p2pav_session_stop(sub);
        p2pav_session_destroy(sub);
        sleep_ms(500);
    }

    p2pav_session_stop(pub);
    p2pav_session_destroy(pub);

    printf("  (%d/5 rejoins received frames)\n", join_ok);
    return (join_ok >= 4) ? 0 : 1;
}

/* =================================================================
 *  TC-R1: null_params -- NULL parameter safety
 * ================================================================= */
static int test_null_params(const test_env_t *env)
{
    (void)env;
    int pass = 1;

    if (p2pav_session_create(NULL) != NULL) pass = 0;

    uint8_t dummy[64] = {0};
    p2pav_video_frame_t vf = { .data = dummy, .size = 64, .timestamp_us = 0, .is_keyframe = 0 };
    p2pav_audio_frame_t af = { .data = dummy, .size = 64, .timestamp_us = 0 };

    if (p2pav_video_send(NULL, &vf) != P2PAV_ERR_INVALID_PARAM) pass = 0;
    if (p2pav_audio_send(NULL, &af) != P2PAV_ERR_INVALID_PARAM) pass = 0;
    if (p2pav_video_send(NULL, NULL) != P2PAV_ERR_INVALID_PARAM) pass = 0;
    if (p2pav_data_send(NULL, 0, dummy, 1) != P2PAV_ERR_INVALID_PARAM) pass = 0;
    if (p2pav_data_send(NULL, 0, NULL, 0) != P2PAV_ERR_INVALID_PARAM) pass = 0;
    if (p2pav_session_start(NULL) != P2PAV_ERR_INVALID_PARAM) pass = 0;

    p2pav_session_stop(NULL);
    p2pav_session_destroy(NULL);
    p2pav_video_mute(NULL, 1);
    p2pav_audio_mute(NULL, 1);
    p2pav_video_set_recv_callback(NULL, NULL, NULL);
    p2pav_audio_set_recv_callback(NULL, NULL, NULL);

    if (p2pav_get_timing(NULL, NULL) != P2PAV_ERR_INVALID_PARAM) pass = 0;
    if (p2pav_get_net_stats(NULL, NULL, NULL) != P2PAV_ERR_INVALID_PARAM) pass = 0;

    printf("  (all NULL checks %s)\n", pass ? "OK" : "FAILED");
    return pass ? 0 : 1;
}

/* =================================================================
 *  TC-R2: send_before_start -- send without connecting
 * ================================================================= */
static int test_send_before_start(const test_env_t *env)
{
    char room[64];
    snprintf(room, sizeof(room), "stress_r2_%lu", env->run_suffix);

    p2pav_session_config_t pc;
    make_pub_cfg(&pc, env, room, "pub1");
    p2pav_session_t *s = p2pav_session_create(&pc);
    if (!s) return 1;

    uint8_t dummy[64] = {0};
    p2pav_video_frame_t vf = { .data = dummy, .size = 64, .timestamp_us = 0, .is_keyframe = 1 };
    p2pav_audio_frame_t af = { .data = dummy, .size = 64, .timestamp_us = 0 };

    int pass = 1;
    if (p2pav_video_send(s, &vf) != P2PAV_ERR_NOT_CONNECTED) pass = 0;
    if (p2pav_audio_send(s, &af) != P2PAV_ERR_NOT_CONNECTED) pass = 0;

    p2pav_session_destroy(s);
    printf("  (%s)\n", pass ? "correctly rejected" : "FAILED");
    return pass ? 0 : 1;
}

/* =================================================================
 *  TC-R3: send_after_stop -- send after session stopped
 * ================================================================= */
static int test_send_after_stop(const test_env_t *env)
{
    char room[64];
    snprintf(room, sizeof(room), "stress_r3_%lu", env->run_suffix);

    p2pav_session_config_t pc;
    make_pub_cfg(&pc, env, room, "pub1");
    p2pav_session_t *s = p2pav_session_create(&pc);
    if (!s) return 1;

    if (p2pav_session_start(s) != P2PAV_OK) {
        p2pav_session_destroy(s); return 1;
    }
    sleep_ms(500);
    p2pav_session_stop(s);

    uint8_t dummy[64] = {0};
    p2pav_video_frame_t vf = { .data = dummy, .size = 64, .timestamp_us = 0, .is_keyframe = 1 };

    int pass = (p2pav_video_send(s, &vf) != P2PAV_OK);

    p2pav_session_destroy(s);
    printf("  (%s)\n", pass ? "correctly rejected" : "FAILED");
    return pass ? 0 : 1;
}

/* =================================================================
 *  TC-R4: double_stop_destroy -- repeated stop/destroy safety
 * ================================================================= */
static int test_double_stop_destroy(const test_env_t *env)
{
    char room[64];
    snprintf(room, sizeof(room), "stress_r4_%lu", env->run_suffix);

    /* Test 1: start → stop → stop → destroy */
    p2pav_session_config_t pc;
    make_pub_cfg(&pc, env, room, "pub1");
    p2pav_session_t *s = p2pav_session_create(&pc);
    if (!s) return 1;

    p2pav_session_start(s);
    sleep_ms(200);
    p2pav_session_stop(s);
    p2pav_session_stop(s);
    p2pav_session_destroy(s);

    /* Test 2: create → destroy (no start) */
    snprintf(room, sizeof(room), "stress_r4b_%lu", env->run_suffix);
    make_pub_cfg(&pc, env, room, "pub1");
    s = p2pav_session_create(&pc);
    if (!s) return 1;
    p2pav_session_destroy(s);

    printf("  (no crash)\n");
    return 0;
}

/* =================================================================
 *  TC-R5: invalid_token -- bad JWT should fail gracefully
 * ================================================================= */
static int test_invalid_token(const test_env_t *env)
{
    char room[64];
    snprintf(room, sizeof(room), "stress_r5_%lu", env->run_suffix);

    p2pav_session_config_t cfg;
    make_pub_cfg(&cfg, env, room, "pub1");
    cfg.auth_token = "invalid.jwt.token.garbage";

    p2pav_session_t *s = p2pav_session_create(&cfg);
    if (!s) return 1;

    p2pav_error_t ret = p2pav_session_start(s);
    sleep_ms(1000);

    p2pav_session_stop(s);
    p2pav_session_destroy(s);

    printf("  (start returned %d, no crash)\n", ret);
    return 0;
}

/* =================================================================
 *  TC-R6: pub_crash -- publisher abruptly destroys while streaming
 * ================================================================= */
static volatile int r6_peer_left;
static volatile int r6_disconnected;
static void r6_on_peer_left(p2pav_session_t *s, const char *pid, void *ud)
{ (void)s;(void)pid;(void)ud; r6_peer_left = 1; }
static void r6_on_disconnected(p2pav_session_t *s, p2pav_error_t r, void *ud)
{ (void)s;(void)r;(void)ud; r6_disconnected = 1; }

static int test_pub_crash(const test_env_t *env)
{
    char room[64];
    snprintf(room, sizeof(room), "stress_r6_%lu", env->run_suffix);
    r6_peer_left = 0;
    r6_disconnected = 0;
    g_peer_ready = 0;

    p2pav_session_config_t pc, sc;
    make_pub_cfg(&pc, env, room, "pub1");
    make_sub_cfg(&sc, env, 0, room, "sub1");

    p2pav_session_t *pub = p2pav_session_create(&pc);
    p2pav_session_t *sub = p2pav_session_create(&sc);
    if (!pub || !sub) return 1;

    p2pav_video_config_t vcfg = {
        .codec = P2PAV_CODEC_H264, .width = 640, .height = 480, .fps = 30,
        .reliability = P2PAV_SEMI_RELIABLE,
    };
    p2pav_video_set_config(pub, &vcfg);
    p2pav_video_set_config(sub, &vcfg);

    p2pav_session_callbacks_t scb = {0};
    scb.on_peer_ready = on_peer_ready_cb;
    scb.on_peer_left = r6_on_peer_left;
    scb.on_disconnected = r6_on_disconnected;
    p2pav_session_set_callbacks(sub, &scb, NULL);

    if (p2pav_session_start(pub) != P2PAV_OK || p2pav_session_start(sub) != P2PAV_OK) {
        p2pav_session_destroy(pub); p2pav_session_destroy(sub); return 1;
    }
    if (wait_flag_timeout(&g_peer_ready, TEST_TIMEOUT_MS) != 0) {
        p2pav_session_stop(pub); p2pav_session_stop(sub);
        p2pav_session_destroy(pub); p2pav_session_destroy(sub); return 1;
    }

    send_dummy_video(pub, 30, 33000);

    p2pav_session_stop(pub);
    p2pav_session_destroy(pub);

    sleep_ms(3000);

    int notified = r6_peer_left || r6_disconnected;
    p2pav_session_stop(sub);
    p2pav_session_destroy(sub);

    printf("  (peer_left=%d, disconnected=%d)\n", r6_peer_left, r6_disconnected);
    return notified ? 0 : 1;
}

/* =================================================================
 *  TC-R7: kick_and_resend -- kick sub then keep sending
 * ================================================================= */
static volatile int r7_peer_left;
static void r7_on_peer_left(p2pav_session_t *s, const char *pid, void *ud)
{ (void)s;(void)pid;(void)ud; r7_peer_left = 1; }

static int test_kick_resend(const test_env_t *env)
{
    char room[64];
    snprintf(room, sizeof(room), "stress_r7_%lu", env->run_suffix);
    r7_peer_left = 0;
    g_peer_ready = 0;

    p2pav_session_config_t pc, sc;
    make_pub_cfg(&pc, env, room, "pub1");
    make_sub_cfg(&sc, env, 0, room, "sub1");

    p2pav_session_t *pub = p2pav_session_create(&pc);
    p2pav_session_t *sub = p2pav_session_create(&sc);
    if (!pub || !sub) return 1;

    p2pav_video_config_t vcfg = {
        .codec = P2PAV_CODEC_H264, .width = 640, .height = 480, .fps = 30,
        .reliability = P2PAV_SEMI_RELIABLE,
    };
    p2pav_video_set_config(pub, &vcfg);
    p2pav_video_set_config(sub, &vcfg);

    p2pav_session_callbacks_t scb = {0};
    scb.on_peer_ready = on_peer_ready_cb;
    scb.on_peer_left = r7_on_peer_left;
    p2pav_session_set_callbacks(sub, &scb, NULL);

    if (p2pav_session_start(pub) != P2PAV_OK || p2pav_session_start(sub) != P2PAV_OK) {
        p2pav_session_destroy(pub); p2pav_session_destroy(sub); return 1;
    }
    if (wait_flag_timeout(&g_peer_ready, TEST_TIMEOUT_MS) != 0) {
        p2pav_session_stop(pub); p2pav_session_stop(sub);
        p2pav_session_destroy(pub); p2pav_session_destroy(sub); return 1;
    }

    send_dummy_video(pub, 30, 33000);
    p2pav_session_kick_peer(pub, "sub1");
    send_dummy_video(pub, 100, 10000);

    int peer_count = p2pav_session_get_peer_count(pub);

    p2pav_session_stop(pub); p2pav_session_stop(sub);
    p2pav_session_destroy(pub); p2pav_session_destroy(sub);

    printf("  (peer_count=%d after kick, no crash)\n", peer_count);
    return (peer_count == 0) ? 0 : 1;
}

/* =================================================================
 *  TC-P1: large_video_frame -- near REASM_VIDEO_MAX_SIZE (512KB)
 * ================================================================= */
static volatile int p1_recv_size;
static void p1_on_video(p2pav_session_t *s, const char *f,
    const p2pav_video_frame_t *fr, void *u)
{ (void)s;(void)f;(void)u; p1_recv_size = fr->size; }

static int test_large_video_frame(const test_env_t *env)
{
    char room[64];
    snprintf(room, sizeof(room), "stress_p1_%lu", env->run_suffix);
    p1_recv_size = 0;
    g_peer_ready = 0;

    p2pav_session_config_t pc, sc;
    make_pub_cfg(&pc, env, room, "pub1");
    make_sub_cfg(&sc, env, 0, room, "sub1");

    p2pav_session_t *pub = p2pav_session_create(&pc);
    p2pav_session_t *sub = p2pav_session_create(&sc);
    if (!pub || !sub) return 1;

    p2pav_video_config_t vcfg = {
        .codec = P2PAV_CODEC_H264, .width = 1920, .height = 1080, .fps = 30,
        .bitrate_mode = P2PAV_BITRATE_HIGH, .reliability = P2PAV_SEMI_RELIABLE,
    };
    p2pav_video_set_config(pub, &vcfg);
    p2pav_video_set_config(sub, &vcfg);
    p2pav_video_set_recv_callback(sub, p1_on_video, NULL);

    p2pav_session_callbacks_t scb = {0};
    scb.on_peer_ready = on_peer_ready_cb;
    p2pav_session_set_callbacks(sub, &scb, NULL);

    if (p2pav_session_start(pub) != P2PAV_OK || p2pav_session_start(sub) != P2PAV_OK) {
        p2pav_session_destroy(pub); p2pav_session_destroy(sub); return 1;
    }
    if (wait_flag_timeout(&g_peer_ready, TEST_TIMEOUT_MS) != 0) {
        p2pav_session_stop(pub); p2pav_session_stop(sub);
        p2pav_session_destroy(pub); p2pav_session_destroy(sub); return 1;
    }

    int large_size = 500 * 1024;
    uint8_t *big_frame = (uint8_t *)malloc(large_size);
    if (!big_frame) {
        p2pav_session_stop(pub); p2pav_session_stop(sub);
        p2pav_session_destroy(pub); p2pav_session_destroy(sub); return 1;
    }
    memset(big_frame, 0xAB, large_size);

    /* Send multiple attempts -- the 500KB frame needs ~425 fragments,
       so with semi-reliable transport some attempts may fail reassembly. */
    for (int attempt = 0; attempt < 5 && p1_recv_size == 0; attempt++) {
        p2pav_video_frame_t vf = {
            .data = big_frame, .size = large_size,
            .timestamp_us = now_us(), .is_keyframe = 1,
        };
        p2pav_video_send(pub, &vf);
        sleep_ms(2000);
    }

    free(big_frame);
    int recv_sz = p1_recv_size;

    p2pav_session_stop(pub); p2pav_session_stop(sub);
    p2pav_session_destroy(pub); p2pav_session_destroy(sub);

    int pass = (recv_sz == large_size);
    printf("  (sent=%dKB, recv=%dKB%s)\n", large_size / 1024, recv_sz / 1024,
           pass ? "" : ", fragmentation loss");
    return pass ? 0 : 1;
}

/* =================================================================
 *  TC-P2: large_data -- near REASM_DATA_MAX_SIZE (64KB)
 * ================================================================= */
static volatile int p2_recv_len;
static volatile int p2_data_ok;
static void p2_on_data(p2pav_session_t *s, const char *from,
    p2pav_data_channel_id_t ch, const void *data, size_t len, void *u)
{
    (void)s;(void)from;(void)ch;(void)u;
    p2_recv_len = (int)len;
    const uint8_t *p = (const uint8_t *)data;
    p2_data_ok = (len >= 4 && p[0] == 'D' && p[1] == 'A' && p[2] == 'T' && p[3] == 'A');
}

static int test_large_data(const test_env_t *env)
{
    char room[64];
    snprintf(room, sizeof(room), "stress_p2_%lu", env->run_suffix);
    p2_recv_len = 0;
    p2_data_ok = 0;
    g_peer_ready = 0;

    p2pav_session_config_t pc, sc;
    make_pub_cfg(&pc, env, room, "pub1");
    make_sub_cfg(&sc, env, 0, room, "sub1");

    p2pav_session_t *pub = p2pav_session_create(&pc);
    p2pav_session_t *sub = p2pav_session_create(&sc);
    if (!pub || !sub) return 1;

    p2pav_data_set_recv_callback(pub, p2_on_data, NULL);

    p2pav_session_callbacks_t scb = {0};
    scb.on_peer_ready = on_peer_ready_cb;
    p2pav_session_set_callbacks(sub, &scb, NULL);

    if (p2pav_session_start(pub) != P2PAV_OK || p2pav_session_start(sub) != P2PAV_OK) {
        p2pav_session_destroy(pub); p2pav_session_destroy(sub); return 1;
    }
    if (wait_flag_timeout(&g_peer_ready, TEST_TIMEOUT_MS) != 0) {
        p2pav_session_stop(pub); p2pav_session_stop(sub);
        p2pav_session_destroy(pub); p2pav_session_destroy(sub); return 1;
    }

    int data_size = 60 * 1024;
    uint8_t *buf = (uint8_t *)malloc(data_size);
    if (!buf) {
        p2pav_session_stop(pub); p2pav_session_stop(sub);
        p2pav_session_destroy(pub); p2pav_session_destroy(sub); return 1;
    }
    buf[0] = 'D'; buf[1] = 'A'; buf[2] = 'T'; buf[3] = 'A';
    for (int i = 4; i < data_size; i++) buf[i] = (uint8_t)(i & 0xFF);

    p2pav_data_channel_config_t dcfg = { .label = "big", .reliability = P2PAV_RELIABLE, .priority = 0 };
    p2pav_data_channel_id_t ch = p2pav_data_open(sub, "pub1", &dcfg);
    if (ch < 0) {
        free(buf);
        p2pav_session_stop(pub); p2pav_session_stop(sub);
        p2pav_session_destroy(pub); p2pav_session_destroy(sub); return 1;
    }

    p2pav_data_send(sub, ch, buf, data_size);
    sleep_ms(3000);
    free(buf);

    p2pav_data_close(sub, ch);
    int rlen = p2_recv_len;
    int rok = p2_data_ok;

    p2pav_session_stop(pub); p2pav_session_stop(sub);
    p2pav_session_destroy(pub); p2pav_session_destroy(sub);

    printf("  (sent=%dKB, recv=%dKB, header_ok=%d)\n",
           data_size / 1024, rlen / 1024, rok);
    return (rlen == data_size && rok) ? 0 : 1;
}

/* =================================================================
 *  TC-P3: high_freq_send -- 1000 frames at ~1ms interval
 * ================================================================= */
static volatile int p3_recv_count;
static void p3_on_video(p2pav_session_t *s, const char *f,
    const p2pav_video_frame_t *fr, void *u) { (void)s;(void)f;(void)fr;(void)u; p3_recv_count++; }

static int test_high_freq_send(const test_env_t *env)
{
    char room[64];
    snprintf(room, sizeof(room), "stress_p3_%lu", env->run_suffix);
    p3_recv_count = 0;
    g_peer_ready = 0;

    p2pav_session_config_t pc, sc;
    make_pub_cfg(&pc, env, room, "pub1");
    make_sub_cfg(&sc, env, 0, room, "sub1");

    p2pav_session_t *pub = p2pav_session_create(&pc);
    p2pav_session_t *sub = p2pav_session_create(&sc);
    if (!pub || !sub) return 1;

    p2pav_video_config_t vcfg = {
        .codec = P2PAV_CODEC_H264, .width = 640, .height = 480, .fps = 30,
        .reliability = P2PAV_SEMI_RELIABLE,
    };
    p2pav_video_set_config(pub, &vcfg);
    p2pav_video_set_config(sub, &vcfg);
    p2pav_video_set_recv_callback(sub, p3_on_video, NULL);

    p2pav_session_callbacks_t scb = {0};
    scb.on_peer_ready = on_peer_ready_cb;
    p2pav_session_set_callbacks(sub, &scb, NULL);

    if (p2pav_session_start(pub) != P2PAV_OK || p2pav_session_start(sub) != P2PAV_OK) {
        p2pav_session_destroy(pub); p2pav_session_destroy(sub); return 1;
    }
    if (wait_flag_timeout(&g_peer_ready, TEST_TIMEOUT_MS) != 0) {
        p2pav_session_stop(pub); p2pav_session_stop(sub);
        p2pav_session_destroy(pub); p2pav_session_destroy(sub); return 1;
    }

    uint8_t small[512];
    memset(small, 0x33, sizeof(small));
    for (int i = 0; i < 1000 && g_running; i++) {
        p2pav_video_frame_t vf = {
            .data = small, .size = (int)sizeof(small),
            .timestamp_us = now_us(),
            .is_keyframe = (i % 30 == 0) ? 1 : 0,
        };
        p2pav_video_send(pub, &vf);
        usleep(1000);
    }

    sleep_ms(2000);
    int recv = p3_recv_count;

    p2pav_session_stop(pub); p2pav_session_stop(sub);
    p2pav_session_destroy(pub); p2pav_session_destroy(sub);

    printf("  (sent=1000, recv=%d)\n", recv);
    return (recv > 0) ? 0 : 1;
}

/* =================================================================
 *  TC-P4: max_subscribers -- P2PAV_MAX_PEERS (10) subs
 * ================================================================= */
#define P4_MAX_SUBS 10
static volatile int p4_recv[P4_MAX_SUBS];
static volatile int p4_ready_count;

static void p4_on_video_0(p2pav_session_t *s,const char *f,const p2pav_video_frame_t *fr,void *u){(void)s;(void)f;(void)fr;(void)u;p4_recv[0]++;}
static void p4_on_video_1(p2pav_session_t *s,const char *f,const p2pav_video_frame_t *fr,void *u){(void)s;(void)f;(void)fr;(void)u;p4_recv[1]++;}
static void p4_on_video_2(p2pav_session_t *s,const char *f,const p2pav_video_frame_t *fr,void *u){(void)s;(void)f;(void)fr;(void)u;p4_recv[2]++;}
static void p4_on_video_3(p2pav_session_t *s,const char *f,const p2pav_video_frame_t *fr,void *u){(void)s;(void)f;(void)fr;(void)u;p4_recv[3]++;}
static void p4_on_video_4(p2pav_session_t *s,const char *f,const p2pav_video_frame_t *fr,void *u){(void)s;(void)f;(void)fr;(void)u;p4_recv[4]++;}
static void p4_on_video_5(p2pav_session_t *s,const char *f,const p2pav_video_frame_t *fr,void *u){(void)s;(void)f;(void)fr;(void)u;p4_recv[5]++;}
static void p4_on_video_6(p2pav_session_t *s,const char *f,const p2pav_video_frame_t *fr,void *u){(void)s;(void)f;(void)fr;(void)u;p4_recv[6]++;}
static void p4_on_video_7(p2pav_session_t *s,const char *f,const p2pav_video_frame_t *fr,void *u){(void)s;(void)f;(void)fr;(void)u;p4_recv[7]++;}
static void p4_on_video_8(p2pav_session_t *s,const char *f,const p2pav_video_frame_t *fr,void *u){(void)s;(void)f;(void)fr;(void)u;p4_recv[8]++;}
static void p4_on_video_9(p2pav_session_t *s,const char *f,const p2pav_video_frame_t *fr,void *u){(void)s;(void)f;(void)fr;(void)u;p4_recv[9]++;}

static void p4_on_ready(p2pav_session_t *s, const char *pid, void *ud)
{ (void)s;(void)pid;(void)ud; p4_ready_count++; }

static int test_max_subscribers(const test_env_t *env)
{
    if (env->num_sub_tokens < P4_MAX_SUBS) {
        printf("  (skipped: need %d sub tokens, have %d)\n", P4_MAX_SUBS, env->num_sub_tokens);
        return 0;
    }

    p2pav_on_video_frame_t cbs[P4_MAX_SUBS] = {
        p4_on_video_0, p4_on_video_1, p4_on_video_2, p4_on_video_3, p4_on_video_4,
        p4_on_video_5, p4_on_video_6, p4_on_video_7, p4_on_video_8, p4_on_video_9,
    };

    char room[64];
    snprintf(room, sizeof(room), "stress_p4_%lu", env->run_suffix);
    memset((void *)p4_recv, 0, sizeof(p4_recv));
    p4_ready_count = 0;

    p2pav_session_config_t pc;
    make_pub_cfg(&pc, env, room, "pub1");
    p2pav_session_t *pub = p2pav_session_create(&pc);
    if (!pub) return 1;

    p2pav_video_config_t vcfg = {
        .codec = P2PAV_CODEC_H264, .width = 640, .height = 480, .fps = 30,
        .reliability = P2PAV_SEMI_RELIABLE,
    };
    p2pav_video_set_config(pub, &vcfg);

    if (p2pav_session_start(pub) != P2PAV_OK) {
        p2pav_session_destroy(pub); return 1;
    }

    p2pav_session_t *subs[P4_MAX_SUBS] = {0};
    for (int i = 0; i < P4_MAX_SUBS; i++) {
        char sub_id[16];
        snprintf(sub_id, sizeof(sub_id), "sub%d", i + 1);

        p2pav_session_config_t sc;
        make_sub_cfg(&sc, env, i, room, sub_id);
        subs[i] = p2pav_session_create(&sc);
        if (!subs[i]) goto p4_fail;

        p2pav_video_set_config(subs[i], &vcfg);
        p2pav_video_set_recv_callback(subs[i], cbs[i], NULL);

        p2pav_session_callbacks_t scb = {0};
        scb.on_peer_ready = p4_on_ready;
        p2pav_session_set_callbacks(subs[i], &scb, NULL);

        if (p2pav_session_start(subs[i]) != P2PAV_OK) goto p4_fail;
        sleep_ms(300);
    }

    /* Wait for all subscribers to be ready */
    int elapsed = 0;
    while (p4_ready_count < P4_MAX_SUBS && elapsed < 30000 && g_running) {
        usleep(WAIT_POLL_MS * US_PER_MS);
        elapsed += WAIT_POLL_MS;
    }
    if (p4_ready_count < P4_MAX_SUBS) goto p4_fail;

    send_dummy_video(pub, 50, 33000);
    sleep_ms(2000);

    int peer_count = p2pav_session_get_peer_count(pub);
    int all_recv = 1;
    for (int i = 0; i < P4_MAX_SUBS; i++) {
        if (p4_recv[i] <= 0) all_recv = 0;
    }

    p2pav_session_stop(pub); p2pav_session_destroy(pub);
    for (int i = 0; i < P4_MAX_SUBS; i++) {
        if (subs[i]) { p2pav_session_stop(subs[i]); p2pav_session_destroy(subs[i]); }
    }

    printf("  (peer_count=%d, all_recv=%d)\n", peer_count, all_recv);
    return (peer_count == P4_MAX_SUBS && all_recv) ? 0 : 1;

p4_fail:
    p2pav_session_stop(pub); p2pav_session_destroy(pub);
    for (int i = 0; i < P4_MAX_SUBS; i++) {
        if (subs[i]) { p2pav_session_stop(subs[i]); p2pav_session_destroy(subs[i]); }
    }
    return 1;
}

/* =================================================================
 *  Test table and main
 * ================================================================= */

static const test_case_t g_tests[] = {
    /* Stability */
    { "rapid_create_destroy", test_rapid_create_destroy },
    { "rapid_start_stop",     test_rapid_start_stop },
    { "multi_session",        test_multi_session },
    { "long_running",         test_long_running },
    { "sub_rejoin",           test_subscriber_rejoin },
    /* Robustness */
    { "null_params",          test_null_params },
    { "send_before_start",    test_send_before_start },
    { "send_after_stop",      test_send_after_stop },
    { "double_stop_destroy",  test_double_stop_destroy },
    { "invalid_token",        test_invalid_token },
    { "pub_crash",            test_pub_crash },
    { "kick_resend",          test_kick_resend },
    /* Stress */
    { "large_video_frame",    test_large_video_frame },
    { "large_data",           test_large_data },
    { "high_freq_send",       test_high_freq_send },
    { "max_subscribers",      test_max_subscribers },
};
#define NUM_TESTS (sizeof(g_tests) / sizeof(g_tests[0]))

static void parse_args(int argc, char **argv, test_env_t *env)
{
    memset(env, 0, sizeof(*env));
    env->signaling_url = "127.0.0.1:8443";
    env->ssl_cert      = "./server.crt";
    env->ssl_key       = "./server.key";
    env->run_suffix    = (unsigned long)time(NULL);
    if (env->run_suffix == 0) env->run_suffix = 1;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--signaling") == 0 && i + 1 < argc) {
            env->signaling_url = argv[++i];
        } else if (strcmp(argv[i], "--cert") == 0 && i + 1 < argc) {
            env->ssl_cert = argv[++i];
        } else if (strcmp(argv[i], "--key") == 0 && i + 1 < argc) {
            env->ssl_key = argv[++i];
        } else if (strcmp(argv[i], "--token") == 0 && i + 1 < argc) {
            env->token_pub = argv[++i];
        } else if (strcmp(argv[i], "--token-sub") == 0 && i + 1 < argc) {
            if (env->num_sub_tokens < MAX_SUB_TOKENS)
                env->token_sub[env->num_sub_tokens++] = argv[++i];
            else ++i;
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printf("Usage: %s [--signaling HOST:PORT] [--cert FILE] [--key FILE]\n"
                   "       [--token JWT] [--token-sub JWT ...]\n", argv[0]);
            exit(0);
        }
    }

    if (env->num_sub_tokens == 0 && env->token_pub)
        env->token_sub[env->num_sub_tokens++] = env->token_pub;
}

int main(int argc, char **argv)
{
#ifndef _WIN32
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
#endif

    test_env_t env;
    parse_args(argc, argv, &env);

    if (!env.token_pub) {
        fprintf(stderr, "Error: --token JWT is required.\n");
        return 1;
    }

    p2pav_set_log_level(P2PAV_LOG_WARN);

    printf("=== libp2pav Stress Test Suite (%s) ===\n", p2pav_version_string());
    printf("Signaling: %s  |  Sub tokens: %d\n\n", env.signaling_url, env.num_sub_tokens);

    int passed = 0;
    for (size_t i = 0; i < NUM_TESTS && g_running; i++) {
        printf("[TC%zu] %-22s ", i + 1, g_tests[i].name);
        fflush(stdout);
        int r = g_tests[i].func(&env);
        if (r == 0) {
            printf("PASS\n");
            passed++;
        } else {
            printf("FAIL\n");
        }
    }

    printf("\n%d/%zu PASSED\n", passed, NUM_TESTS);
    return (passed == (int)NUM_TESTS) ? 0 : 1;
}
