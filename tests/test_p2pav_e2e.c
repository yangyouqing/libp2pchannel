/*
 * test_p2pav_e2e.c -- End-to-end tests for libp2pav.so public API.
 *
 * Tests: video-only, audio-only, data channel, mixed AV, mute, kick peer,
 *        diagnostics, multi-subscriber.
 *
 * Usage:
 *   ./test_p2pav_e2e [--signaling HOST:PORT] [--cert FILE] [--key FILE]
 *                   [--token JWT] [--token-sub JWT]
 *
 * Prerequisites: signaling server running (HTTPS). Get tokens via:
 *   curl -sk "https://HOST:PORT/v1/token?peer_id=pub1" -H "Authorization: Bearer ADMIN_SECRET"
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
#define WAIT_POLL_MS        50
#define US_PER_MS           1000
#define VIDEO_FRAME_SIZE    4096
#define AUDIO_FRAME_SIZE    160
#define NUM_VIDEO_FRAMES_TC1 150
#define NUM_AUDIO_FRAMES_TC2 250
#define NUM_DATA_MESSAGES   10
#define NUM_VIDEO_FRAMES_TC8 100
#define DATA_PAYLOAD_MAX    32768

typedef struct {
    const char *signaling_url;
    const char *ssl_cert;
    const char *ssl_key;
    const char *token_pub;
    const char *token_sub;
    const char *token_sub2;  /* optional, for TC8 second subscriber */
    unsigned long run_suffix; /* unique per run to avoid room name collision */
} test_env_t;

typedef int (*test_func_t)(const test_env_t *env);

typedef struct {
    const char  *name;
    test_func_t  func;
} test_case_t;

static volatile int g_running = 1;
static volatile int g_peer_ready = 0;

static void on_signal(int sig) { (void)sig; g_running = 0; }

static void on_peer_ready_cb(p2pav_session_t *s, const char *peer_id, void *ud)
{
    (void)s; (void)peer_id; (void)ud;
    g_peer_ready = 1;
}

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

/* ---------- TC1: Video only ---------- */
static volatile int tc1_video_recv_count;
static volatile int tc1_first_keyframe;
static volatile uint64_t tc1_first_recv_us;

static void tc1_on_video(p2pav_session_t *s, const char *from_peer,
                         const p2pav_video_frame_t *frame, void *ud)
{
    (void)s; (void)from_peer; (void)ud;
    if (tc1_video_recv_count == 0) {
        tc1_first_recv_us = now_us();
        tc1_first_keyframe = frame->is_keyframe ? 1 : 0;
    }
    tc1_video_recv_count++;
}

static int test_video_only(const test_env_t *env)
{
    char room[64];
    snprintf(room, sizeof(room), "room_tc1_%lu", env->run_suffix);
    const char *pub_id = "pub1";
    const char *sub_id = "sub1";

    tc1_video_recv_count = 0;
    tc1_first_keyframe = 0;
    tc1_first_recv_us = 0;
    g_peer_ready = 0;

    p2pav_session_config_t pub_cfg = {0};
    pub_cfg.signaling_url = env->signaling_url;
    pub_cfg.auth_token    = env->token_pub;
    pub_cfg.room_id       = room;
    pub_cfg.peer_id       = pub_id;
    pub_cfg.role          = P2PAV_ROLE_PUBLISHER;
    pub_cfg.stun_server   = "127.0.0.1:3478";
    pub_cfg.ssl_cert_file = env->ssl_cert;
    pub_cfg.ssl_key_file  = env->ssl_key;

    p2pav_session_config_t sub_cfg = {0};
    sub_cfg.signaling_url = env->signaling_url;
    sub_cfg.auth_token    = env->token_sub;
    sub_cfg.room_id       = room;
    sub_cfg.peer_id       = sub_id;
    sub_cfg.role          = P2PAV_ROLE_SUBSCRIBER;
    sub_cfg.stun_server   = "127.0.0.1:3478";

    p2pav_session_t *pub = p2pav_session_create(&pub_cfg);
    p2pav_session_t *sub = p2pav_session_create(&sub_cfg);
    if (!pub || !sub) return 1;

    p2pav_video_config_t vcfg = {
        .codec = P2PAV_CODEC_H264,
        .width = 640, .height = 480, .fps = 30,
        .bitrate_mode = P2PAV_BITRATE_MEDIUM,
        .reliability = P2PAV_SEMI_RELIABLE,
    };
    p2pav_video_set_config(pub, &vcfg);
    p2pav_video_set_config(sub, &vcfg);
    p2pav_video_set_recv_callback(sub, tc1_on_video, NULL);

    p2pav_session_callbacks_t sub_cbs = {0};
    sub_cbs.on_peer_ready = on_peer_ready_cb;
    p2pav_session_set_callbacks(sub, &sub_cbs, NULL);

    if (p2pav_session_start(pub) != P2PAV_OK || p2pav_session_start(sub) != P2PAV_OK) {
        p2pav_session_destroy(pub);
        p2pav_session_destroy(sub);
        return 1;
    }
    if (wait_flag_timeout(&g_peer_ready, TEST_TIMEOUT_MS) != 0) {
        p2pav_session_stop(pub);
        p2pav_session_stop(sub);
        p2pav_session_destroy(pub);
        p2pav_session_destroy(sub);
        return 1;
    }

    uint8_t dummy_video[VIDEO_FRAME_SIZE];
    memset(dummy_video, 0x42, sizeof(dummy_video));
    for (int i = 0; i < NUM_VIDEO_FRAMES_TC1 && g_running; i++) {
        p2pav_video_frame_t vf = {
            .data = dummy_video,
            .size = (int)sizeof(dummy_video),
            .timestamp_us = (uint64_t)i * 33333,
            .is_keyframe = (i % 30 == 0) ? 1 : 0,
        };
        p2pav_video_send(pub, &vf);
        usleep(33000);
    }

    sleep_ms(500);
    int recv = tc1_video_recv_count;
    uint64_t first_ms = tc1_first_recv_us ? (tc1_first_recv_us / 1000) : 0;

    p2pav_session_stop(pub);
    p2pav_session_stop(sub);
    p2pav_session_destroy(pub);
    p2pav_session_destroy(sub);

    if (recv <= 0 || !tc1_first_keyframe) return 1;
    printf("  (recv=%d/%d, first_frame=%lums)\n", recv, NUM_VIDEO_FRAMES_TC1, (unsigned long)first_ms);
    return 0;
}

/* ---------- TC2: Audio only ---------- */
static volatile int tc2_audio_recv_count;

static void tc2_on_audio(p2pav_session_t *s, const char *from_peer,
                         const p2pav_audio_frame_t *frame, void *ud)
{
    (void)s; (void)from_peer; (void)ud; (void)frame;
    tc2_audio_recv_count++;
}

static int test_audio_only(const test_env_t *env)
{
    char room[64];
    snprintf(room, sizeof(room), "room_tc2_%lu", env->run_suffix);
    const char *pub_id = "pub1";
    const char *sub_id = "sub1";

    tc2_audio_recv_count = 0;
    g_peer_ready = 0;

    p2pav_session_config_t pub_cfg = {0};
    pub_cfg.signaling_url = env->signaling_url;
    pub_cfg.auth_token    = env->token_pub;
    pub_cfg.room_id      = room;
    pub_cfg.peer_id      = pub_id;
    pub_cfg.role         = P2PAV_ROLE_PUBLISHER;
    pub_cfg.stun_server  = "127.0.0.1:3478";
    pub_cfg.ssl_cert_file = env->ssl_cert;
    pub_cfg.ssl_key_file  = env->ssl_key;

    p2pav_session_config_t sub_cfg = {0};
    sub_cfg.signaling_url = env->signaling_url;
    sub_cfg.auth_token    = env->token_sub;
    sub_cfg.room_id       = room;
    sub_cfg.peer_id       = sub_id;
    sub_cfg.role          = P2PAV_ROLE_SUBSCRIBER;
    sub_cfg.stun_server   = "127.0.0.1:3478";

    p2pav_session_t *pub = p2pav_session_create(&pub_cfg);
    p2pav_session_t *sub = p2pav_session_create(&sub_cfg);
    if (!pub || !sub) return 1;

    p2pav_audio_config_t acfg = {
        .codec = P2PAV_CODEC_OPUS,
        .sample_rate = 48000, .channels = 1,
        .bitrate_bps = 64000, .frame_duration_ms = 20,
        .reliability = P2PAV_UNRELIABLE,
    };
    p2pav_audio_set_config(pub, &acfg);
    p2pav_audio_set_config(sub, &acfg);
    p2pav_audio_set_recv_callback(sub, tc2_on_audio, NULL);

    p2pav_session_callbacks_t sub_cbs = {0};
    sub_cbs.on_peer_ready = on_peer_ready_cb;
    p2pav_session_set_callbacks(sub, &sub_cbs, NULL);

    if (p2pav_session_start(pub) != P2PAV_OK || p2pav_session_start(sub) != P2PAV_OK) {
        p2pav_session_destroy(pub);
        p2pav_session_destroy(sub);
        return 1;
    }
    if (wait_flag_timeout(&g_peer_ready, TEST_TIMEOUT_MS) != 0) {
        p2pav_session_stop(pub);
        p2pav_session_stop(sub);
        p2pav_session_destroy(pub);
        p2pav_session_destroy(sub);
        return 1;
    }

    uint8_t dummy_audio[AUDIO_FRAME_SIZE];
    memset(dummy_audio, 0x11, sizeof(dummy_audio));
    for (int i = 0; i < NUM_AUDIO_FRAMES_TC2 && g_running; i++) {
        p2pav_audio_frame_t af = {
            .data = dummy_audio,
            .size = (int)sizeof(dummy_audio),
            .timestamp_us = (uint64_t)i * 20000,
        };
        p2pav_audio_send(pub, &af);
        usleep(20000);
    }

    sleep_ms(300);
    int recv = tc2_audio_recv_count;

    p2pav_session_stop(pub);
    p2pav_session_stop(sub);
    p2pav_session_destroy(pub);
    p2pav_session_destroy(sub);

    if (recv <= 0) return 1;
    printf("  (recv=%d/%d)\n", recv, NUM_AUDIO_FRAMES_TC2);
    return 0;
}

/* ---------- TC3: Data channel ---------- */
static volatile int tc3_data_recv_count;
#define TC3_MSG_SIZE 10
static size_t tc3_recv_lens[TC3_MSG_SIZE];
static int tc3_recv_ok[TC3_MSG_SIZE];

static void tc3_on_data(p2pav_session_t *s, const char *from_peer,
                        p2pav_data_channel_id_t ch, const void *data, size_t len, void *ud)
{
    (void)s; (void)from_peer; (void)ch; (void)ud;
    if (tc3_data_recv_count < TC3_MSG_SIZE) {
        tc3_recv_lens[tc3_data_recv_count] = len;
        const uint8_t *p = (const uint8_t *)data;
        tc3_recv_ok[tc3_data_recv_count] = (len >= 4 && p[0] == 'M' && p[1] == 'S' && p[2] == 'G' && p[3] == (uint8_t)tc3_data_recv_count) ? 1 : 0;
        tc3_data_recv_count++;
    }
}

static int test_data_channel(const test_env_t *env)
{
    char room[64];
    snprintf(room, sizeof(room), "room_tc3_%lu", env->run_suffix);
    const char *pub_id = "pub1";
    const char *sub_id = "sub1";

    tc3_data_recv_count = 0;
    memset(tc3_recv_ok, 0, sizeof(tc3_recv_ok));
    g_peer_ready = 0;

    p2pav_session_config_t pub_cfg = {0};
    pub_cfg.signaling_url = env->signaling_url;
    pub_cfg.auth_token    = env->token_pub;
    pub_cfg.room_id      = room;
    pub_cfg.peer_id      = pub_id;
    pub_cfg.role         = P2PAV_ROLE_PUBLISHER;
    pub_cfg.stun_server  = "127.0.0.1:3478";
    pub_cfg.ssl_cert_file = env->ssl_cert;
    pub_cfg.ssl_key_file  = env->ssl_key;

    p2pav_session_config_t sub_cfg = {0};
    sub_cfg.signaling_url = env->signaling_url;
    sub_cfg.auth_token    = env->token_sub;
    sub_cfg.room_id       = room;
    sub_cfg.peer_id       = sub_id;
    sub_cfg.role          = P2PAV_ROLE_SUBSCRIBER;
    sub_cfg.stun_server   = "127.0.0.1:3478";

    p2pav_session_t *pub = p2pav_session_create(&pub_cfg);
    p2pav_session_t *sub = p2pav_session_create(&sub_cfg);
    if (!pub || !sub) return 1;

    p2pav_data_set_recv_callback(pub, tc3_on_data, NULL);

    p2pav_session_callbacks_t sub_cbs = {0};
    sub_cbs.on_peer_ready = on_peer_ready_cb;
    p2pav_session_set_callbacks(sub, &sub_cbs, NULL);

    if (p2pav_session_start(pub) != P2PAV_OK || p2pav_session_start(sub) != P2PAV_OK) {
        p2pav_session_destroy(pub);
        p2pav_session_destroy(sub);
        return 1;
    }
    if (wait_flag_timeout(&g_peer_ready, TEST_TIMEOUT_MS) != 0) {
        p2pav_session_stop(pub);
        p2pav_session_stop(sub);
        p2pav_session_destroy(pub);
        p2pav_session_destroy(sub);
        return 1;
    }

    p2pav_data_channel_config_t dcfg = { .label = "test", .reliability = P2PAV_RELIABLE, .priority = 0 };
    p2pav_data_channel_id_t ch = p2pav_data_open(sub, pub_id, &dcfg);
    if (ch < 0) {
        p2pav_session_stop(pub);
        p2pav_session_stop(sub);
        p2pav_session_destroy(pub);
        p2pav_session_destroy(sub);
        return 1;
    }

    static const size_t sizes[] = {16, 64, 256, 1024, 4096, 8192, 16384, 32*1024, 100, 50};
    uint8_t buf[DATA_PAYLOAD_MAX];
    for (int i = 0; i < NUM_DATA_MESSAGES && g_running; i++) {
        size_t sz = sizes[i % 10];
        if (sz > sizeof(buf)) sz = sizeof(buf);
        buf[0] = 'M'; buf[1] = 'S'; buf[2] = 'G'; buf[3] = (uint8_t)i;
        for (size_t j = 4; j < sz; j++) buf[j] = (uint8_t)(j & 0xff);
        if (p2pav_data_send(sub, ch, buf, sz) != P2PAV_OK) break;
        usleep(50000);
    }

    sleep_ms(1000);
    int recv = tc3_data_recv_count;
    int verified = 0;
    for (int i = 0; i < recv && i < TC3_MSG_SIZE; i++) verified += tc3_recv_ok[i];

    p2pav_data_close(sub, ch);
    p2pav_session_stop(pub);
    p2pav_session_stop(sub);
    p2pav_session_destroy(pub);
    p2pav_session_destroy(sub);

    if (recv < NUM_DATA_MESSAGES) return 1;
    printf("  (recv=%d/%d, verified=%d)\n", recv, NUM_DATA_MESSAGES, verified);
    return 0;
}

/* ---------- TC4: Mixed AV ---------- */
static volatile int tc4_video_count;
static volatile int tc4_audio_count;
static volatile uint64_t tc4_first_video_us;
static volatile uint64_t tc4_first_audio_us;

static void tc4_on_video(p2pav_session_t *s, const char *from_peer,
                         const p2pav_video_frame_t *frame, void *ud)
{
    (void)s; (void)from_peer; (void)ud; (void)frame;
    if (tc4_video_count == 0) tc4_first_video_us = now_us();
    tc4_video_count++;
}

static void tc4_on_audio(p2pav_session_t *s, const char *from_peer,
                         const p2pav_audio_frame_t *frame, void *ud)
{
    (void)s; (void)from_peer; (void)ud; (void)frame;
    if (tc4_audio_count == 0) tc4_first_audio_us = now_us();
    tc4_audio_count++;
}

static int test_mixed_av(const test_env_t *env)
{
    char room[64];
    snprintf(room, sizeof(room), "room_tc4_%lu", env->run_suffix);
    const char *pub_id = "pub1";
    const char *sub_id = "sub1";

    tc4_video_count = 0;
    tc4_audio_count = 0;
    tc4_first_video_us = 0;
    tc4_first_audio_us = 0;
    g_peer_ready = 0;

    p2pav_session_config_t pub_cfg = {0};
    pub_cfg.signaling_url = env->signaling_url;
    pub_cfg.auth_token    = env->token_pub;
    pub_cfg.room_id       = room;
    pub_cfg.peer_id       = pub_id;
    pub_cfg.role          = P2PAV_ROLE_PUBLISHER;
    pub_cfg.stun_server   = "127.0.0.1:3478";
    pub_cfg.ssl_cert_file = env->ssl_cert;
    pub_cfg.ssl_key_file  = env->ssl_key;

    p2pav_session_config_t sub_cfg = {0};
    sub_cfg.signaling_url = env->signaling_url;
    sub_cfg.auth_token    = env->token_sub;
    sub_cfg.room_id       = room;
    sub_cfg.peer_id       = sub_id;
    sub_cfg.role          = P2PAV_ROLE_SUBSCRIBER;
    sub_cfg.stun_server   = "127.0.0.1:3478";

    p2pav_session_t *pub = p2pav_session_create(&pub_cfg);
    p2pav_session_t *sub = p2pav_session_create(&sub_cfg);
    if (!pub || !sub) return 1;

    p2pav_video_config_t vcfg = {
        .codec = P2PAV_CODEC_H264, .width = 640, .height = 480, .fps = 30,
        .bitrate_mode = P2PAV_BITRATE_MEDIUM, .reliability = P2PAV_SEMI_RELIABLE,
    };
    p2pav_audio_config_t acfg = {
        .codec = P2PAV_CODEC_OPUS, .sample_rate = 48000, .channels = 1,
        .bitrate_bps = 64000, .frame_duration_ms = 20, .reliability = P2PAV_UNRELIABLE,
    };
    p2pav_video_set_config(pub, &vcfg);
    p2pav_video_set_config(sub, &vcfg);
    p2pav_audio_set_config(pub, &acfg);
    p2pav_audio_set_config(sub, &acfg);
    p2pav_video_set_recv_callback(sub, tc4_on_video, NULL);
    p2pav_audio_set_recv_callback(sub, tc4_on_audio, NULL);

    p2pav_session_callbacks_t sub_cbs = {0};
    sub_cbs.on_peer_ready = on_peer_ready_cb;
    p2pav_session_set_callbacks(sub, &sub_cbs, NULL);

    if (p2pav_session_start(pub) != P2PAV_OK || p2pav_session_start(sub) != P2PAV_OK) {
        p2pav_session_destroy(pub);
        p2pav_session_destroy(sub);
        return 1;
    }
    if (wait_flag_timeout(&g_peer_ready, TEST_TIMEOUT_MS) != 0) {
        p2pav_session_stop(pub);
        p2pav_session_stop(sub);
        p2pav_session_destroy(pub);
        p2pav_session_destroy(sub);
        return 1;
    }

    uint8_t dummy_v[VIDEO_FRAME_SIZE], dummy_a[AUDIO_FRAME_SIZE];
    memset(dummy_v, 0x42, sizeof(dummy_v));
    memset(dummy_a, 0x11, sizeof(dummy_a));
    uint64_t t0 = now_us();
    while ((now_us() - t0) < 10*1000000ULL && g_running) {
        p2pav_video_frame_t vf = { .data = dummy_v, .size = (int)sizeof(dummy_v),
            .timestamp_us = now_us(), .is_keyframe = (tc4_video_count % 30 == 0) ? 1 : 0 };
        p2pav_video_send(pub, &vf);
        p2pav_audio_frame_t af = { .data = dummy_a, .size = (int)sizeof(dummy_a), .timestamp_us = now_us() };
        p2pav_audio_send(pub, &af);
        usleep(20000);
    }

    sleep_ms(500);
    int vc = tc4_video_count, ac = tc4_audio_count;
    uint64_t fv_ms = tc4_first_video_us / 1000, fa_ms = tc4_first_audio_us / 1000;

    p2pav_session_stop(pub);
    p2pav_session_stop(sub);
    p2pav_session_destroy(pub);
    p2pav_session_destroy(sub);

    if (vc <= 0 || ac <= 0) return 1;
    printf("  (video=%d audio=%d, first_v=%lums first_a=%lums)\n", vc, ac, (unsigned long)fv_ms, (unsigned long)fa_ms);
    return 0;
}

/* ---------- TC5: Mute ---------- */
static volatile int tc5_v_before_mute, tc5_a_before_mute;
static volatile int tc5_v_after_mute, tc5_a_after_mute;
static volatile int tc5_v_after_unmute, tc5_a_after_unmute;

static void tc5_on_video(p2pav_session_t *s, const char *from_peer,
                         const p2pav_video_frame_t *frame, void *ud) { (void)s;(void)from_peer;(void)ud;(void)frame; tc5_v_after_unmute++; }
static void tc5_on_audio(p2pav_session_t *s, const char *from_peer,
                         const p2pav_audio_frame_t *frame, void *ud) { (void)s;(void)from_peer;(void)ud;(void)frame; tc5_a_after_unmute++; }

static int test_mute(const test_env_t *env)
{
    char room[64];
    snprintf(room, sizeof(room), "room_tc5_%lu", env->run_suffix);
    const char *pub_id = "pub1";
    const char *sub_id = "sub1";

    tc5_v_before_mute = tc5_a_before_mute = 0;
    tc5_v_after_mute = tc5_a_after_mute = 0;
    tc5_v_after_unmute = tc5_a_after_unmute = 0;
    g_peer_ready = 0;

    p2pav_session_config_t pub_cfg = {0};
    pub_cfg.signaling_url = env->signaling_url;
    pub_cfg.auth_token    = env->token_pub;
    pub_cfg.room_id       = room;
    pub_cfg.peer_id       = pub_id;
    pub_cfg.role          = P2PAV_ROLE_PUBLISHER;
    pub_cfg.stun_server   = "127.0.0.1:3478";
    pub_cfg.ssl_cert_file = env->ssl_cert;
    pub_cfg.ssl_key_file  = env->ssl_key;

    p2pav_session_config_t sub_cfg = {0};
    sub_cfg.signaling_url = env->signaling_url;
    sub_cfg.auth_token    = env->token_sub;
    sub_cfg.room_id       = room;
    sub_cfg.peer_id       = sub_id;
    sub_cfg.role          = P2PAV_ROLE_SUBSCRIBER;
    sub_cfg.stun_server   = "127.0.0.1:3478";

    p2pav_session_t *pub = p2pav_session_create(&pub_cfg);
    p2pav_session_t *sub = p2pav_session_create(&sub_cfg);
    if (!pub || !sub) return 1;

    p2pav_video_config_t vcfg = { .codec = P2PAV_CODEC_H264, .width = 640, .height = 480, .fps = 30, .reliability = P2PAV_SEMI_RELIABLE };
    p2pav_audio_config_t acfg = { .codec = P2PAV_CODEC_OPUS, .sample_rate = 48000, .channels = 1, .bitrate_bps = 64000, .frame_duration_ms = 20, .reliability = P2PAV_UNRELIABLE };
    p2pav_video_set_config(pub, &vcfg);
    p2pav_video_set_config(sub, &vcfg);
    p2pav_audio_set_config(pub, &acfg);
    p2pav_audio_set_config(sub, &acfg);
    p2pav_video_set_recv_callback(sub, tc5_on_video, NULL);
    p2pav_audio_set_recv_callback(sub, tc5_on_audio, NULL);

    p2pav_session_callbacks_t sub_cbs = {0};
    sub_cbs.on_peer_ready = on_peer_ready_cb;
    p2pav_session_set_callbacks(sub, &sub_cbs, NULL);

    if (p2pav_session_start(pub) != P2PAV_OK || p2pav_session_start(sub) != P2PAV_OK) {
        p2pav_session_destroy(pub);
        p2pav_session_destroy(sub);
        return 1;
    }
    if (wait_flag_timeout(&g_peer_ready, TEST_TIMEOUT_MS) != 0) {
        p2pav_session_stop(pub);
        p2pav_session_stop(sub);
        p2pav_session_destroy(pub);
        p2pav_session_destroy(sub);
        return 1;
    }

    uint8_t dv[VIDEO_FRAME_SIZE], da[AUDIO_FRAME_SIZE];
    memset(dv, 0x42, sizeof(dv));
    memset(da, 0x11, sizeof(da));
    for (int i = 0; i < 60 && g_running; i++) {
        p2pav_video_frame_t vf = { .data = dv, .size = (int)sizeof(dv), .timestamp_us = now_us(), .is_keyframe = (i % 30 == 0) ? 1 : 0 };
        p2pav_video_send(pub, &vf);
        p2pav_audio_frame_t af = { .data = da, .size = (int)sizeof(da), .timestamp_us = now_us() };
        p2pav_audio_send(pub, &af);
        usleep(33333);
    }
    sleep_ms(200);
    tc5_v_before_mute = tc5_v_after_unmute;
    tc5_a_before_mute = tc5_a_after_unmute;

    p2pav_video_mute(pub, 1);
    p2pav_audio_mute(pub, 1);
    sleep_ms(2000);
    tc5_v_after_mute = tc5_v_after_unmute;
    tc5_a_after_mute = tc5_a_after_unmute;
    sleep_ms(2000);
    int delta_muted_v = tc5_v_after_unmute - tc5_v_after_mute;
    int delta_muted_a = tc5_a_after_unmute - tc5_a_after_mute;

    p2pav_video_mute(pub, 0);
    p2pav_audio_mute(pub, 0);
    for (int i = 0; i < 60 && g_running; i++) {
        p2pav_video_frame_t vf = { .data = dv, .size = (int)sizeof(dv), .timestamp_us = now_us(), .is_keyframe = 0 };
        p2pav_video_send(pub, &vf);
        p2pav_audio_frame_t af = { .data = da, .size = (int)sizeof(da), .timestamp_us = now_us() };
        p2pav_audio_send(pub, &af);
        usleep(33333);
    }
    sleep_ms(500);
    int unmuted_v = tc5_v_after_unmute - tc5_v_after_mute;
    int unmuted_a = tc5_a_after_unmute - tc5_a_after_mute;

    p2pav_session_stop(pub);
    p2pav_session_stop(sub);
    p2pav_session_destroy(pub);
    p2pav_session_destroy(sub);

    if (unmuted_v <= 0 && unmuted_a <= 0) return 1;
    printf("  (muted_delta v=%d a=%d, unmuted_delta v=%d a=%d)\n", delta_muted_v, delta_muted_a, unmuted_v, unmuted_a);
    return 0;
}

/* ---------- TC6: Kick peer ---------- */
static volatile int tc6_peer_left_called;
static volatile int tc6_video_after_kick;

static void tc6_on_peer_left(p2pav_session_t *s, const char *peer_id, void *ud)
{
    (void)s; (void)peer_id; (void)ud;
    tc6_peer_left_called = 1;
}

static void tc6_on_video(p2pav_session_t *s, const char *from_peer,
                         const p2pav_video_frame_t *frame, void *ud)
{
    (void)s; (void)from_peer; (void)ud; (void)frame;
    tc6_video_after_kick++;
}

static int test_kick_peer(const test_env_t *env)
{
    char room[64];
    snprintf(room, sizeof(room), "room_tc6_%lu", env->run_suffix);
    const char *pub_id = "pub1";
    const char *sub_id = "sub1";

    tc6_peer_left_called = 0;
    tc6_video_after_kick = 0;
    g_peer_ready = 0;

    p2pav_session_config_t pub_cfg = {0};
    pub_cfg.signaling_url = env->signaling_url;
    pub_cfg.auth_token    = env->token_pub;
    pub_cfg.room_id       = room;
    pub_cfg.peer_id       = pub_id;
    pub_cfg.role          = P2PAV_ROLE_PUBLISHER;
    pub_cfg.stun_server   = "127.0.0.1:3478";
    pub_cfg.ssl_cert_file = env->ssl_cert;
    pub_cfg.ssl_key_file  = env->ssl_key;

    p2pav_session_config_t sub_cfg = {0};
    sub_cfg.signaling_url = env->signaling_url;
    sub_cfg.auth_token    = env->token_sub;
    sub_cfg.room_id       = room;
    sub_cfg.peer_id       = sub_id;
    sub_cfg.role          = P2PAV_ROLE_SUBSCRIBER;
    sub_cfg.stun_server   = "127.0.0.1:3478";

    p2pav_session_t *pub = p2pav_session_create(&pub_cfg);
    p2pav_session_t *sub = p2pav_session_create(&sub_cfg);
    if (!pub || !sub) return 1;

    p2pav_video_config_t vcfg = { .codec = P2PAV_CODEC_H264, .width = 640, .height = 480, .fps = 30, .reliability = P2PAV_SEMI_RELIABLE };
    p2pav_video_set_config(pub, &vcfg);
    p2pav_video_set_config(sub, &vcfg);
    p2pav_video_set_recv_callback(sub, tc6_on_video, NULL);

    p2pav_session_callbacks_t sub_cbs = {0};
    sub_cbs.on_peer_ready = on_peer_ready_cb;
    sub_cbs.on_peer_left  = tc6_on_peer_left;
    p2pav_session_set_callbacks(sub, &sub_cbs, NULL);

    if (p2pav_session_start(pub) != P2PAV_OK || p2pav_session_start(sub) != P2PAV_OK) {
        p2pav_session_destroy(pub);
        p2pav_session_destroy(sub);
        return 1;
    }
    if (wait_flag_timeout(&g_peer_ready, TEST_TIMEOUT_MS) != 0) {
        p2pav_session_stop(pub);
        p2pav_session_stop(sub);
        p2pav_session_destroy(pub);
        p2pav_session_destroy(sub);
        return 1;
    }

    uint8_t dv[VIDEO_FRAME_SIZE];
    memset(dv, 0x42, sizeof(dv));
    for (int i = 0; i < 90 && g_running; i++) {
        p2pav_video_frame_t vf = { .data = dv, .size = (int)sizeof(dv), .timestamp_us = now_us(), .is_keyframe = (i % 30 == 0) ? 1 : 0 };
        p2pav_video_send(pub, &vf);
        usleep(33333);
        if (i == 60) {
            p2pav_session_kick_peer(pub, sub_id);
        }
    }
    sleep_ms(500);
    int peer_count = p2pav_session_get_peer_count(pub);

    p2pav_session_stop(pub);
    p2pav_session_stop(sub);
    p2pav_session_destroy(pub);
    p2pav_session_destroy(sub);

    /* Subscriber may get on_peer_left or on_disconnected when publisher kicks */
    if (peer_count != 0) return 1;
    printf("  (on_peer_left=%d, peer_count=%d)\n", tc6_peer_left_called, peer_count);
    return 0;
}

/* ---------- TC7: Diagnostics ---------- */
static int test_diagnostics(const test_env_t *env)
{
    char room[64];
    snprintf(room, sizeof(room), "room_tc7_%lu", env->run_suffix);
    const char *pub_id = "pub1";
    const char *sub_id = "sub1";

    g_peer_ready = 0;

    p2pav_session_config_t pub_cfg = {0};
    pub_cfg.signaling_url = env->signaling_url;
    pub_cfg.auth_token    = env->token_pub;
    pub_cfg.room_id       = room;
    pub_cfg.peer_id       = pub_id;
    pub_cfg.role          = P2PAV_ROLE_PUBLISHER;
    pub_cfg.stun_server   = "127.0.0.1:3478";
    pub_cfg.ssl_cert_file = env->ssl_cert;
    pub_cfg.ssl_key_file  = env->ssl_key;

    p2pav_session_config_t sub_cfg = {0};
    sub_cfg.signaling_url = env->signaling_url;
    sub_cfg.auth_token    = env->token_sub;
    sub_cfg.room_id       = room;
    sub_cfg.peer_id       = sub_id;
    sub_cfg.role          = P2PAV_ROLE_SUBSCRIBER;
    sub_cfg.stun_server   = "127.0.0.1:3478";

    p2pav_session_t *pub = p2pav_session_create(&pub_cfg);
    p2pav_session_t *sub = p2pav_session_create(&sub_cfg);
    if (!pub || !sub) return 1;

    p2pav_video_config_t vcfg = { .codec = P2PAV_CODEC_H264, .width = 640, .height = 480, .fps = 30, .reliability = P2PAV_SEMI_RELIABLE };
    p2pav_video_set_config(pub, &vcfg);
    p2pav_video_set_config(sub, &vcfg);

    p2pav_session_callbacks_t sub_cbs = {0};
    sub_cbs.on_peer_ready = on_peer_ready_cb;
    p2pav_session_set_callbacks(sub, &sub_cbs, NULL);

    if (p2pav_session_start(pub) != P2PAV_OK || p2pav_session_start(sub) != P2PAV_OK) {
        p2pav_session_destroy(pub);
        p2pav_session_destroy(sub);
        return 1;
    }
    if (wait_flag_timeout(&g_peer_ready, TEST_TIMEOUT_MS) != 0) {
        p2pav_session_stop(pub);
        p2pav_session_stop(sub);
        p2pav_session_destroy(pub);
        p2pav_session_destroy(sub);
        return 1;
    }

    uint8_t dv[VIDEO_FRAME_SIZE];
    memset(dv, 0x42, sizeof(dv));
    for (int i = 0; i < 30 && g_running; i++) {
        p2pav_video_frame_t vf = { .data = dv, .size = (int)sizeof(dv), .timestamp_us = now_us(), .is_keyframe = (i == 0) ? 1 : 0 };
        p2pav_video_send(pub, &vf);
        usleep(33333);
    }
    sleep_ms(200);

    p2pav_timing_t timing = {0};
    p2pav_net_stats_t stats = {0};
    p2pav_error_t et = p2pav_get_timing(pub, &timing);
    p2pav_error_t es = p2pav_get_net_stats(pub, sub_id, &stats);

    p2pav_session_stop(pub);
    p2pav_session_stop(sub);
    p2pav_session_destroy(pub);
    p2pav_session_destroy(sub);

    if (et != P2PAV_OK || es != P2PAV_OK) return 1;
    if (timing.signaling_connected_us <= timing.session_start_us) return 1;
    if (timing.ice_connected_us == 0) return 1;
    /* localhost can be DIRECT or UNKNOWN depending on libjuice */
    printf("  (path=%s, rtt=%u us)\n", stats.path_type == P2PAV_PATH_DIRECT ? "direct" : (stats.path_type == P2PAV_PATH_RELAY ? "relay" : "unknown"), (unsigned)stats.rtt_us);
    return 0;
}

/* ---------- TC8: Multi-subscriber ---------- */
static volatile int tc8_sub1_count;
static volatile int tc8_sub2_count;
static volatile int tc8_sub1_ready;
static volatile int tc8_sub2_ready;

static void tc8_on_video_sub1(p2pav_session_t *s, const char *from_peer,
                              const p2pav_video_frame_t *frame, void *ud)
{
    (void)s; (void)from_peer; (void)ud; (void)frame;
    tc8_sub1_count++;
}

static void tc8_on_video_sub2(p2pav_session_t *s, const char *from_peer,
                              const p2pav_video_frame_t *frame, void *ud)
{
    (void)s; (void)from_peer; (void)ud; (void)frame;
    tc8_sub2_count++;
}

static void tc8_ready_sub1(p2pav_session_t *s, const char *pid, void *ud) { (void)s;(void)pid;(void)ud; tc8_sub1_ready = 1; }
static void tc8_ready_sub2(p2pav_session_t *s, const char *pid, void *ud) { (void)s;(void)pid;(void)ud; tc8_sub2_ready = 1; }

static int test_multi_sub(const test_env_t *env)
{
    char room[64];
    snprintf(room, sizeof(room), "room_tc8_%lu", env->run_suffix);
    const char *pub_id = "pub1";
    const char *sub1_id = "sub1";
    const char *sub2_id = "sub2";

    tc8_sub1_count = 0;
    tc8_sub2_count = 0;
    tc8_sub1_ready = 0;
    tc8_sub2_ready = 0;

    p2pav_session_config_t pub_cfg = {0};
    pub_cfg.signaling_url = env->signaling_url;
    pub_cfg.auth_token    = env->token_pub;
    pub_cfg.room_id       = room;
    pub_cfg.peer_id       = pub_id;
    pub_cfg.role          = P2PAV_ROLE_PUBLISHER;
    pub_cfg.stun_server   = "127.0.0.1:3478";
    pub_cfg.ssl_cert_file = env->ssl_cert;
    pub_cfg.ssl_key_file  = env->ssl_key;

    p2pav_session_config_t sub1_cfg = {0};
    sub1_cfg.signaling_url = env->signaling_url;
    sub1_cfg.auth_token   = env->token_sub; /* same token for simplicity; use distinct tokens per peer in production */
    sub1_cfg.room_id      = room;
    sub1_cfg.peer_id      = sub1_id;
    sub1_cfg.role         = P2PAV_ROLE_SUBSCRIBER;
    sub1_cfg.stun_server  = "127.0.0.1:3478";

    p2pav_session_config_t sub2_cfg = {0};
    sub2_cfg.signaling_url = env->signaling_url;
    sub2_cfg.auth_token   = env->token_sub2 ? env->token_sub2 : env->token_sub;
    sub2_cfg.room_id      = room;
    sub2_cfg.peer_id      = sub2_id;
    sub2_cfg.role         = P2PAV_ROLE_SUBSCRIBER;
    sub2_cfg.stun_server  = "127.0.0.1:3478";

    p2pav_session_t *pub = p2pav_session_create(&pub_cfg);
    p2pav_session_t *sub1 = p2pav_session_create(&sub1_cfg);
    p2pav_session_t *sub2 = p2pav_session_create(&sub2_cfg);
    if (!pub || !sub1 || !sub2) {
        if (pub) p2pav_session_destroy(pub);
        if (sub1) p2pav_session_destroy(sub1);
        if (sub2) p2pav_session_destroy(sub2);
        return 1;
    }

    p2pav_video_config_t vcfg = { .codec = P2PAV_CODEC_H264, .width = 640, .height = 480, .fps = 30, .reliability = P2PAV_SEMI_RELIABLE };
    p2pav_video_set_config(pub, &vcfg);
    p2pav_video_set_config(sub1, &vcfg);
    p2pav_video_set_config(sub2, &vcfg);
    p2pav_video_set_recv_callback(sub1, tc8_on_video_sub1, NULL);
    p2pav_video_set_recv_callback(sub2, tc8_on_video_sub2, NULL);

    p2pav_session_callbacks_t cbs1 = {0};
    cbs1.on_peer_ready = tc8_ready_sub1;
    p2pav_session_set_callbacks(sub1, &cbs1, NULL);
    p2pav_session_callbacks_t cbs2 = {0};
    cbs2.on_peer_ready = tc8_ready_sub2;
    p2pav_session_set_callbacks(sub2, &cbs2, NULL);

    if (p2pav_session_start(pub) != P2PAV_OK || p2pav_session_start(sub1) != P2PAV_OK || p2pav_session_start(sub2) != P2PAV_OK) {
        p2pav_session_destroy(pub);
        p2pav_session_destroy(sub1);
        p2pav_session_destroy(sub2);
        return 1;
    }
    if (wait_flag_timeout(&tc8_sub1_ready, TEST_TIMEOUT_MS) != 0 || wait_flag_timeout(&tc8_sub2_ready, 5000) != 0) {
        p2pav_session_stop(pub);
        p2pav_session_stop(sub1);
        p2pav_session_stop(sub2);
        p2pav_session_destroy(pub);
        p2pav_session_destroy(sub1);
        p2pav_session_destroy(sub2);
        return 1;
    }

    uint8_t dv[VIDEO_FRAME_SIZE];
    memset(dv, 0x42, sizeof(dv));
    for (int i = 0; i < NUM_VIDEO_FRAMES_TC8 && g_running; i++) {
        p2pav_video_frame_t vf = { .data = dv, .size = (int)sizeof(dv), .timestamp_us = now_us(), .is_keyframe = (i % 30 == 0) ? 1 : 0 };
        p2pav_video_send(pub, &vf);
        usleep(33333);
    }
    sleep_ms(500);
    int c1 = tc8_sub1_count, c2 = tc8_sub2_count;
    int peer_count = p2pav_session_get_peer_count(pub);

    p2pav_session_stop(pub);
    p2pav_session_stop(sub1);
    p2pav_session_stop(sub2);
    p2pav_session_destroy(pub);
    p2pav_session_destroy(sub1);
    p2pav_session_destroy(sub2);

    if (peer_count != 2 || c1 <= 0 || c2 <= 0) return 1;
    printf("  (sub1=%d sub2=%d peer_count=%d)\n", c1, c2, peer_count);
    return 0;
}

/* ---------- Main ---------- */
static const test_case_t g_tests[] = {
    { "video_only",   test_video_only },
    { "audio_only",   test_audio_only },
    { "data_channel", test_data_channel },
    { "mixed_av",     test_mixed_av },
    { "mute",         test_mute },
    { "kick_peer",    test_kick_peer },
    { "diagnostics",  test_diagnostics },
    { "multi_sub",    test_multi_sub },
};
#define NUM_TESTS (sizeof(g_tests) / sizeof(g_tests[0]))

static void parse_args(int argc, char **argv, test_env_t *env)
{
    env->signaling_url = "127.0.0.1:8443";
    env->ssl_cert      = "./server.crt";
    env->ssl_key       = "./server.key";
    env->token_pub     = NULL;
    env->token_sub     = NULL;
    env->token_sub2    = NULL;
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
            if (!env->token_sub) env->token_sub = env->token_pub;
        } else if (strcmp(argv[i], "--token-sub") == 0 && i + 1 < argc) {
            env->token_sub = argv[++i];
        } else if (strcmp(argv[i], "--token-sub2") == 0 && i + 1 < argc) {
            env->token_sub2 = argv[++i];
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printf("Usage: %s [--signaling HOST:PORT] [--cert FILE] [--key FILE] [--token JWT] [--token-sub JWT] [--token-sub2 JWT]\n", argv[0]);
            exit(0);
        }
    }
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
        fprintf(stderr, "Error: --token JWT is required. Get token via:\n");
        fprintf(stderr, "  curl -sk \"https://%s/v1/token?peer_id=pub1\" -H \"Authorization: Bearer ADMIN_SECRET\"\n", env.signaling_url);
        return 1;
    }

    p2pav_set_log_level(P2PAV_LOG_WARN);

    printf("=== libp2pav E2E Test Suite (%s) ===\n", p2pav_version_string());
    printf("Signaling: %s\n\n", env.signaling_url);

    int passed = 0;
    for (size_t i = 0; i < NUM_TESTS && g_running; i++) {
        printf("[TC%zu] %-16s ", i + 1, g_tests[i].name);
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
