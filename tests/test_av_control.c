/*
 * test_av_control.c -- E2E test for VIDEO_STOP/START and AUDIO_STOP/START
 * control messages over the full QUIC transport path.
 *
 * Flow:
 *   1. Create publisher + subscriber engines, ICE connect, QUIC handshake
 *   2. Phase 1: VIDEO_STOP  -- publisher sends audio only, verify video=0
 *   3. Phase 2: VIDEO_START -- publisher sends both, verify video>0, audio>0
 *   4. Phase 3: AUDIO_STOP  -- publisher sends video only, verify audio=0
 *   5. Phase 4: AUDIO_START -- publisher sends both, verify video>0, audio>0
 *
 * Usage: ./test_av_control [--duration SECS] [--cert FILE] [--key FILE]
 */

#include "p2p_adapter.h"
#include "p2p_signaling.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <pthread.h>

#define MAX_CAND_LEN 256

static volatile int g_running = 1;
static int g_phase_duration_sec = 120;

/* ---------- Publisher state ---------- */
static volatile int g_pub_ice_connected = 0;
static volatile int g_pub_quic_connected = 0;
static volatile int g_pub_gathering_done = 0;
static char g_pub_candidates[10][MAX_CAND_LEN];
static int g_pub_candidate_count = 0;
static volatile int g_pub_video_sent = 0;
static volatile int g_pub_audio_sent = 0;

/* ---------- Subscriber state ---------- */
static volatile int g_sub_ice_connected = 0;
static volatile int g_sub_quic_connected = 0;
static volatile int g_sub_gathering_done = 0;
static char g_sub_candidates[10][MAX_CAND_LEN];
static int g_sub_candidate_count = 0;
static volatile int g_sub_video_recv = 0;
static volatile int g_sub_audio_recv = 0;

/* ---------- Error tracking ---------- */
static volatile int g_error_count = 0;

/* ================================================================
 * Publisher callbacks
 * ================================================================ */

static void pub_on_ice_state(p2p_peer_ctx_t *peer, p2p_ice_state_t state, void *ud)
{
    (void)ud;
    printf("[PUB] ICE state: %d\n", state);
    if (state == P2P_ICE_STATE_CONNECTED || state == P2P_ICE_STATE_COMPLETED)
        g_pub_ice_connected = 1;
    if (state == P2P_ICE_STATE_FAILED) {
        fprintf(stderr, "[PUB] ERROR: ICE FAILED\n");
        g_error_count++;
        g_running = 0;
    }
}

static void pub_on_candidate(p2p_peer_ctx_t *peer, const char *sdp, void *ud)
{
    (void)ud;
    if (g_pub_candidate_count < 10)
        snprintf(g_pub_candidates[g_pub_candidate_count++], MAX_CAND_LEN, "%s", sdp);
}

static void pub_on_gathering_done(p2p_peer_ctx_t *peer, void *ud)
{
    (void)ud;
    g_pub_gathering_done = 1;
}

static void pub_on_quic_connected(p2p_peer_ctx_t *peer, void *ud)
{
    (void)ud;
    printf("[PUB] QUIC connected with %s\n", peer->peer_id);
    g_pub_quic_connected = 1;
}

static void pub_on_data_recv(p2p_peer_ctx_t *peer, const p2p_frame_header_t *hdr,
                             const uint8_t *payload, void *ud)
{
    (void)ud; (void)payload;
    if (hdr->type == P2P_FRAME_TYPE_VIDEO_STOP) {
        peer->video_paused = 1;
        printf("[PUB] received VIDEO_STOP -> video_paused=1\n");
    } else if (hdr->type == P2P_FRAME_TYPE_VIDEO_START) {
        peer->video_paused = 0;
        printf("[PUB] received VIDEO_START -> video_paused=0\n");
    } else if (hdr->type == P2P_FRAME_TYPE_AUDIO_STOP) {
        peer->audio_paused = 1;
        printf("[PUB] received AUDIO_STOP -> audio_paused=1\n");
    } else if (hdr->type == P2P_FRAME_TYPE_AUDIO_START) {
        peer->audio_paused = 0;
        printf("[PUB] received AUDIO_START -> audio_paused=0\n");
    } else if (hdr->type == P2P_FRAME_TYPE_IDR_REQ) {
        printf("[PUB] received IDR_REQ\n");
    }
}

/* ================================================================
 * Subscriber callbacks
 * ================================================================ */

static void sub_on_ice_state(p2p_peer_ctx_t *peer, p2p_ice_state_t state, void *ud)
{
    (void)ud;
    printf("[SUB] ICE state: %d\n", state);
    if (state == P2P_ICE_STATE_CONNECTED || state == P2P_ICE_STATE_COMPLETED)
        g_sub_ice_connected = 1;
    if (state == P2P_ICE_STATE_FAILED) {
        fprintf(stderr, "[SUB] ERROR: ICE FAILED\n");
        g_error_count++;
        g_running = 0;
    }
}

static void sub_on_candidate(p2p_peer_ctx_t *peer, const char *sdp, void *ud)
{
    (void)ud;
    if (g_sub_candidate_count < 10)
        snprintf(g_sub_candidates[g_sub_candidate_count++], MAX_CAND_LEN, "%s", sdp);
}

static void sub_on_gathering_done(p2p_peer_ctx_t *peer, void *ud)
{
    (void)ud;
    g_sub_gathering_done = 1;
}

static void sub_on_quic_connected(p2p_peer_ctx_t *peer, void *ud)
{
    (void)ud;
    printf("[SUB] QUIC connected with %s\n", peer->peer_id);
    g_sub_quic_connected = 1;
}

static void sub_on_data_recv(p2p_peer_ctx_t *peer, const p2p_frame_header_t *hdr,
                             const uint8_t *payload, void *ud)
{
    (void)ud; (void)payload; (void)peer;
    if (hdr->type == P2P_FRAME_TYPE_VIDEO)
        g_sub_video_recv++;
    else if (hdr->type == P2P_FRAME_TYPE_AUDIO)
        g_sub_audio_recv++;
}

/* ================================================================
 * Helpers
 * ================================================================ */

static int wait_flag(volatile int *flag, int timeout_ms)
{
    for (int i = 0; i < timeout_ms / 50 && g_running; i++) {
        usleep(50000);
        if (*flag) return 0;
    }
    return -1;
}

static int wait_two_flags(volatile int *a, volatile int *b, int timeout_ms)
{
    for (int i = 0; i < timeout_ms / 50 && g_running; i++) {
        usleep(50000);
        if (*a && *b) return 0;
    }
    return -1;
}

/* Publisher sender thread: sends synthetic video + audio frames,
 * respecting video_paused / audio_paused flags. */
static p2p_peer_ctx_t *g_pub_peer_for_sender = NULL;
static volatile int g_sender_running = 0;

static void *pub_sender_thread(void *arg)
{
    (void)arg;
    uint32_t vid_seq = 0, aud_seq = 0;
    uint8_t vid_payload[512];
    uint8_t aud_payload[160];
    memset(vid_payload, 0xAA, sizeof(vid_payload));
    memset(aud_payload, 0xBB, sizeof(aud_payload));

    while (g_sender_running && g_running) {
        p2p_peer_ctx_t *peer = g_pub_peer_for_sender;
        if (!peer || peer->state < P2P_PEER_STATE_QUIC_CONNECTED) {
            usleep(50000);
            continue;
        }

        if (!peer->video_paused) {
            uint8_t flags = (vid_seq % 30 == 0) ? P2P_FRAME_FLAG_KEY : 0;
            int ret = p2p_peer_send_data_via_quic(peer, P2P_FRAME_TYPE_VIDEO,
                flags, vid_seq, vid_seq * 33333ULL, vid_payload, sizeof(vid_payload));
            if (ret == 0) g_pub_video_sent++;
            vid_seq++;
        }

        if (!peer->audio_paused) {
            int ret = p2p_peer_send_data_via_quic(peer, P2P_FRAME_TYPE_AUDIO,
                0, aud_seq, aud_seq * 20000ULL, aud_payload, sizeof(aud_payload));
            if (ret == 0) g_pub_audio_sent++;
            aud_seq++;
        }

        usleep(20000);
    }
    return NULL;
}

static void sighandler(int sig) { (void)sig; g_running = 0; }

static void print_time(void)
{
    time_t t = time(NULL);
    struct tm *tm = localtime(&t);
    char buf[32];
    strftime(buf, sizeof(buf), "%H:%M:%S", tm);
    printf("[%s] ", buf);
}

/* Run one test phase. Returns 0 on success, -1 on failure. */
static int run_phase(const char *name, int phase_num,
                     p2p_peer_ctx_t *sub_peer,
                     int expect_video, int expect_audio,
                     int duration_sec)
{
    print_time();
    printf("--- Phase %d: %s (%ds) ---\n", phase_num, name, duration_sec);

    g_sub_video_recv = 0;
    g_sub_audio_recv = 0;
    g_pub_video_sent = 0;
    g_pub_audio_sent = 0;

    uint64_t start = p2p_now_us();
    uint64_t end = start + (uint64_t)duration_sec * 1000000ULL;
    int report_interval = duration_sec > 30 ? 30 : 5;
    uint64_t next_report = start + (uint64_t)report_interval * 1000000ULL;

    while (g_running && p2p_now_us() < end) {
        usleep(100000);
        if (p2p_now_us() >= next_report) {
            int elapsed = (int)((p2p_now_us() - start) / 1000000);
            printf("  [%ds] pub_sent: v=%d a=%d | sub_recv: v=%d a=%d\n",
                   elapsed, g_pub_video_sent, g_pub_audio_sent,
                   g_sub_video_recv, g_sub_audio_recv);
            next_report = p2p_now_us() + (uint64_t)report_interval * 1000000ULL;
        }
    }

    if (!g_running) return -1;

    int v_recv = g_sub_video_recv;
    int a_recv = g_sub_audio_recv;
    int v_sent = g_pub_video_sent;
    int a_sent = g_pub_audio_sent;

    print_time();
    printf("  Result: pub_sent v=%d a=%d | sub_recv v=%d a=%d\n",
           v_sent, a_sent, v_recv, a_recv);

    int ok = 1;
    if (expect_video) {
        if (v_recv <= 0) {
            fprintf(stderr, "  FAIL: expected video frames but received 0\n");
            ok = 0;
        }
        if (v_sent <= 0) {
            fprintf(stderr, "  FAIL: expected publisher to send video but sent 0\n");
            ok = 0;
        }
    } else {
        if (v_recv > 0) {
            fprintf(stderr, "  FAIL: expected 0 video frames but received %d\n", v_recv);
            ok = 0;
        }
        if (v_sent > 0) {
            fprintf(stderr, "  FAIL: expected publisher to NOT send video but sent %d\n", v_sent);
            ok = 0;
        }
    }

    if (expect_audio) {
        if (a_recv <= 0) {
            fprintf(stderr, "  FAIL: expected audio frames but received 0\n");
            ok = 0;
        }
        if (a_sent <= 0) {
            fprintf(stderr, "  FAIL: expected publisher to send audio but sent 0\n");
            ok = 0;
        }
    } else {
        if (a_recv > 0) {
            fprintf(stderr, "  FAIL: expected 0 audio frames but received %d\n", a_recv);
            ok = 0;
        }
        if (a_sent > 0) {
            fprintf(stderr, "  FAIL: expected publisher to NOT send audio but sent %d\n", a_sent);
            ok = 0;
        }
    }

    if (ok) {
        print_time();
        printf("[OK] Phase %d passed: %s\n\n", phase_num, name);
    } else {
        g_error_count++;
        fprintf(stderr, "[FAIL] Phase %d FAILED: %s\n\n", phase_num, name);
    }
    return ok ? 0 : -1;
}

/* ================================================================
 * Main
 * ================================================================ */

int main(int argc, char **argv)
{
    signal(SIGINT, sighandler);
    signal(SIGTERM, sighandler);

    const char *cert = "certs/server.crt";
    const char *key  = "certs/server.key";

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--duration") == 0 && i + 1 < argc)
            g_phase_duration_sec = atoi(argv[++i]);
        else if (strcmp(argv[i], "--cert") == 0 && i + 1 < argc)
            cert = argv[++i];
        else if (strcmp(argv[i], "--key") == 0 && i + 1 < argc)
            key = argv[++i];
        else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printf("Usage: %s [--duration SECS] [--cert FILE] [--key FILE]\n", argv[0]);
            return 0;
        }
    }

    p2p_adapter_set_log_level(2);

    printf("=============================================================\n");
    printf("  AV Control E2E Test (VIDEO_STOP/START + AUDIO_STOP/START)\n");
    printf("  Phase duration: %d seconds  (~%d min total)\n",
           g_phase_duration_sec, g_phase_duration_sec * 4 / 60);
    printf("  cert=%s key=%s\n", cert, key);
    printf("=============================================================\n\n");

    /* --- Create engines --- */
    p2p_engine_t pub_engine, sub_engine;

    p2p_engine_config_t pub_cfg;
    memset(&pub_cfg, 0, sizeof(pub_cfg));
    pub_cfg.role = P2P_ROLE_PUBLISHER;
    pub_cfg.ssl_cert_file = cert;
    pub_cfg.ssl_key_file = key;
    pub_cfg.callbacks.on_peer_ice_state = pub_on_ice_state;
    pub_cfg.callbacks.on_peer_ice_candidate = pub_on_candidate;
    pub_cfg.callbacks.on_peer_ice_gathering_done = pub_on_gathering_done;
    pub_cfg.callbacks.on_peer_quic_connected = pub_on_quic_connected;
    pub_cfg.callbacks.on_peer_data_recv = pub_on_data_recv;

    p2p_engine_config_t sub_cfg;
    memset(&sub_cfg, 0, sizeof(sub_cfg));
    sub_cfg.role = P2P_ROLE_SUBSCRIBER;
    sub_cfg.ssl_cert_file = cert;
    sub_cfg.ssl_key_file = key;
    sub_cfg.callbacks.on_peer_ice_state = sub_on_ice_state;
    sub_cfg.callbacks.on_peer_ice_candidate = sub_on_candidate;
    sub_cfg.callbacks.on_peer_ice_gathering_done = sub_on_gathering_done;
    sub_cfg.callbacks.on_peer_quic_connected = sub_on_quic_connected;
    sub_cfg.callbacks.on_peer_data_recv = sub_on_data_recv;

    if (p2p_engine_init(&pub_engine, &pub_cfg) != 0) {
        fprintf(stderr, "FAIL: publisher engine init\n");
        return 1;
    }
    if (p2p_engine_init(&sub_engine, &sub_cfg) != 0) {
        fprintf(stderr, "FAIL: subscriber engine init\n");
        p2p_engine_destroy(&pub_engine);
        return 1;
    }

    /* Start xquic engine threads */
    p2p_engine_start(&pub_engine);
    p2p_engine_start(&sub_engine);

    /* Add peers */
    p2p_peer_ctx_t *pub_peer = p2p_engine_add_peer(&pub_engine, "sub1");
    p2p_peer_ctx_t *sub_peer = p2p_engine_add_peer(&sub_engine, "pub1");
    if (!pub_peer || !sub_peer) {
        fprintf(stderr, "FAIL: add peer\n");
        goto cleanup;
    }

    /* --- Exchange SDP + candidates --- */
    printf("--- Setting up ICE connection ---\n");
    char pub_sdp[P2P_SIG_MAX_SDP_SIZE], sub_sdp[P2P_SIG_MAX_SDP_SIZE];
    p2p_peer_get_local_description(pub_peer, pub_sdp, sizeof(pub_sdp));
    p2p_peer_get_local_description(sub_peer, sub_sdp, sizeof(sub_sdp));

    p2p_peer_set_remote_description(pub_peer, sub_sdp);
    p2p_peer_set_remote_description(sub_peer, pub_sdp);

    p2p_peer_gather_candidates(pub_peer);
    p2p_peer_gather_candidates(sub_peer);

    if (wait_two_flags(&g_pub_gathering_done, &g_sub_gathering_done, 10000) != 0) {
        fprintf(stderr, "FAIL: ICE gathering timeout\n");
        goto cleanup;
    }

    for (int i = 0; i < g_pub_candidate_count; i++)
        p2p_peer_add_remote_candidate(sub_peer, g_pub_candidates[i]);
    for (int i = 0; i < g_sub_candidate_count; i++)
        p2p_peer_add_remote_candidate(pub_peer, g_sub_candidates[i]);

    p2p_peer_set_remote_gathering_done(pub_peer);
    p2p_peer_set_remote_gathering_done(sub_peer);

    /* Wait for ICE */
    if (wait_two_flags(&g_pub_ice_connected, &g_sub_ice_connected, 15000) != 0) {
        fprintf(stderr, "FAIL: ICE connection timeout\n");
        goto cleanup;
    }
    printf("[OK] ICE connected\n");

    /* Wait for QUIC handshake */
    if (wait_two_flags(&g_pub_quic_connected, &g_sub_quic_connected, 15000) != 0) {
        fprintf(stderr, "FAIL: QUIC handshake timeout (pub=%d sub=%d)\n",
                g_pub_quic_connected, g_sub_quic_connected);
        goto cleanup;
    }
    printf("[OK] QUIC handshake complete\n\n");

    /* --- Start publisher sender thread --- */
    g_pub_peer_for_sender = pub_peer;
    g_sender_running = 1;
    pthread_t sender_tid;
    pthread_create(&sender_tid, NULL, pub_sender_thread, NULL);

    /* Allow a brief warm-up for the data path */
    usleep(500000);

    /* ===== Phase 1: VIDEO_STOP -- expect audio only ===== */
    printf("[SUB] Sending VIDEO_STOP...\n");
    if (p2p_peer_send_video_stop(sub_peer) != 0) {
        fprintf(stderr, "WARN: p2p_peer_send_video_stop returned error\n");
        g_error_count++;
    }
    usleep(500000);
    run_phase("VIDEO_STOP (audio only)", 1, sub_peer,
              /*expect_video=*/0, /*expect_audio=*/1, g_phase_duration_sec);

    if (!g_running) goto stop_sender;

    /* ===== Phase 2: VIDEO_START -- expect both ===== */
    printf("[SUB] Sending VIDEO_START...\n");
    if (p2p_peer_send_video_start(sub_peer) != 0) {
        fprintf(stderr, "WARN: p2p_peer_send_video_start returned error\n");
        g_error_count++;
    }
    usleep(500000);
    run_phase("VIDEO_START (both A+V)", 2, sub_peer,
              /*expect_video=*/1, /*expect_audio=*/1, g_phase_duration_sec);

    if (!g_running) goto stop_sender;

    /* ===== Phase 3: AUDIO_STOP -- expect video only ===== */
    printf("[SUB] Sending AUDIO_STOP...\n");
    if (p2p_peer_send_audio_stop(sub_peer) != 0) {
        fprintf(stderr, "WARN: p2p_peer_send_audio_stop returned error\n");
        g_error_count++;
    }
    usleep(500000);
    run_phase("AUDIO_STOP (video only)", 3, sub_peer,
              /*expect_video=*/1, /*expect_audio=*/0, g_phase_duration_sec);

    if (!g_running) goto stop_sender;

    /* ===== Phase 4: AUDIO_START -- expect both ===== */
    printf("[SUB] Sending AUDIO_START...\n");
    if (p2p_peer_send_audio_start(sub_peer) != 0) {
        fprintf(stderr, "WARN: p2p_peer_send_audio_start returned error\n");
        g_error_count++;
    }
    usleep(500000);
    run_phase("AUDIO_START (both A+V)", 4, sub_peer,
              /*expect_video=*/1, /*expect_audio=*/1, g_phase_duration_sec);

stop_sender:
    g_sender_running = 0;
    pthread_join(sender_tid, NULL);

cleanup:
    printf("\n=============================================================\n");
    if (g_error_count == 0) {
        printf("  ALL 4 PHASES PASSED  --  total errors: 0\n");
    } else {
        printf("  TEST FAILED  --  total errors: %d\n", g_error_count);
    }
    printf("=============================================================\n");

    p2p_engine_stop(&pub_engine);
    p2p_engine_stop(&sub_engine);
    p2p_engine_destroy(&pub_engine);
    p2p_engine_destroy(&sub_engine);

    return g_error_count ? 1 : 0;
}
