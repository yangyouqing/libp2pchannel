/*
 * test_video_stop.c -- Test that VIDEO_STOP control message from subscriber
 * causes the publisher to stop sending video to that specific peer.
 *
 * Flow:
 *   1. Create publisher + subscriber ICE engines, connect locally
 *   2. Publisher sends video frames, subscriber counts received frames
 *   3. Subscriber sends VIDEO_STOP
 *   4. Publisher checks peer->video_paused before sending (same as send_to_all_peers)
 *   5. Verify subscriber receives 0 frames after VIDEO_STOP
 *   6. Subscriber sends VIDEO_START, verify frames resume
 *
 * Usage: ./test_video_stop
 */

#include "p2p_adapter.h"
#include "p2p_signaling.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>

#define MAX_CAND_LEN 256

static volatile int g_running = 1;

/* Publisher state */
static volatile int g_pub_connected = 0;
static volatile int g_pub_gathering_done = 0;
static char g_pub_candidates[10][MAX_CAND_LEN];
static int g_pub_candidate_count = 0;

/* Subscriber state */
static volatile int g_sub_connected = 0;
static volatile int g_sub_gathering_done = 0;
static char g_sub_candidates[10][MAX_CAND_LEN];
static int g_sub_candidate_count = 0;

/* Frame counters */
static volatile int g_sub_video_recv = 0;
static volatile int g_pub_ctrl_recv_type = 0;

/* --- Publisher callbacks --- */

static void pub_on_ice_state(p2p_peer_ctx_t *peer, p2p_ice_state_t state, void *ud)
{
    (void)ud;
    printf("[PUB] ICE state: %d\n", state);
    if (state == P2P_ICE_STATE_CONNECTED || state == P2P_ICE_STATE_COMPLETED)
        g_pub_connected = 1;
    if (state == P2P_ICE_STATE_FAILED) g_running = 0;
}

static void pub_on_candidate(p2p_peer_ctx_t *peer, const char *sdp, void *ud)
{
    (void)ud;
    if (g_pub_candidate_count < 10)
        snprintf(g_pub_candidates[g_pub_candidate_count++],
                 MAX_CAND_LEN, "%s", sdp);
}

static void pub_on_gathering_done(p2p_peer_ctx_t *peer, void *ud)
{
    (void)ud;
    g_pub_gathering_done = 1;
}

static void pub_on_data_recv(p2p_peer_ctx_t *peer, const p2p_frame_header_t *hdr,
                             const uint8_t *payload, void *ud)
{
    (void)ud; (void)payload;
    g_pub_ctrl_recv_type = hdr->type;

    if (hdr->type == P2P_FRAME_TYPE_VIDEO_STOP) {
        peer->video_paused = 1;
        printf("[PUB] received VIDEO_STOP from %s -> video_paused=1\n", peer->peer_id);
    } else if (hdr->type == P2P_FRAME_TYPE_VIDEO_START) {
        peer->video_paused = 0;
        printf("[PUB] received VIDEO_START from %s -> video_paused=0\n", peer->peer_id);
    }
}

/* --- Subscriber callbacks --- */

static void sub_on_ice_state(p2p_peer_ctx_t *peer, p2p_ice_state_t state, void *ud)
{
    (void)ud;
    printf("[SUB] ICE state: %d\n", state);
    if (state == P2P_ICE_STATE_CONNECTED || state == P2P_ICE_STATE_COMPLETED)
        g_sub_connected = 1;
    if (state == P2P_ICE_STATE_FAILED) g_running = 0;
}

static void sub_on_candidate(p2p_peer_ctx_t *peer, const char *sdp, void *ud)
{
    (void)ud;
    if (g_sub_candidate_count < 10)
        snprintf(g_sub_candidates[g_sub_candidate_count++],
                 MAX_CAND_LEN, "%s", sdp);
}

static void sub_on_gathering_done(p2p_peer_ctx_t *peer, void *ud)
{
    (void)ud;
    g_sub_gathering_done = 1;
}

static void sub_on_data_recv(p2p_peer_ctx_t *peer, const p2p_frame_header_t *hdr,
                             const uint8_t *payload, void *ud)
{
    (void)ud; (void)payload; (void)peer;
    if (hdr->type == P2P_FRAME_TYPE_VIDEO)
        g_sub_video_recv++;
}

/* --- Helpers --- */

static int wait_flags(volatile int *a, volatile int *b, int timeout_ms)
{
    for (int i = 0; i < timeout_ms / 50 && g_running; i++) {
        usleep(50000);
        if (*a && *b) return 0;
    }
    return -1;
}

static void send_video_frame(p2p_peer_ctx_t *peer, uint32_t seq)
{
    if (peer->video_paused) return;
    uint8_t dummy[512];
    memset(dummy, 0x42, sizeof(dummy));
    p2p_peer_send_data(peer, P2P_FRAME_TYPE_VIDEO, 0, seq, seq * 33333ULL,
                       dummy, sizeof(dummy));
}

static void sighandler(int sig) { (void)sig; g_running = 0; }

int main(int argc, char **argv)
{
    signal(SIGINT, sighandler);
    p2p_adapter_set_log_level(2);

    const char *cert = (argc > 1) ? argv[1] : "certs/server.crt";
    const char *key  = (argc > 2) ? argv[2] : "certs/server.key";

    printf("=== Test: VIDEO_STOP / VIDEO_START control messages ===\n");
    printf("  cert=%s key=%s\n\n", cert, key);

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
    pub_cfg.callbacks.on_peer_data_recv = pub_on_data_recv;

    p2p_engine_config_t sub_cfg;
    memset(&sub_cfg, 0, sizeof(sub_cfg));
    sub_cfg.role = P2P_ROLE_SUBSCRIBER;
    sub_cfg.ssl_cert_file = cert;
    sub_cfg.ssl_key_file = key;
    sub_cfg.callbacks.on_peer_ice_state = sub_on_ice_state;
    sub_cfg.callbacks.on_peer_ice_candidate = sub_on_candidate;
    sub_cfg.callbacks.on_peer_ice_gathering_done = sub_on_gathering_done;
    sub_cfg.callbacks.on_peer_data_recv = sub_on_data_recv;

    if (p2p_engine_init(&pub_engine, &pub_cfg) != 0 ||
        p2p_engine_init(&sub_engine, &sub_cfg) != 0) {
        fprintf(stderr, "FAIL: engine init\n");
        return 1;
    }

    p2p_peer_ctx_t *pub_peer = p2p_engine_add_peer(&pub_engine, "sub1");
    p2p_peer_ctx_t *sub_peer = p2p_engine_add_peer(&sub_engine, "pub1");
    if (!pub_peer || !sub_peer) {
        fprintf(stderr, "FAIL: add peer\n");
        return 1;
    }

    /* --- Exchange SDP + candidates --- */
    char pub_sdp[P2P_SIG_MAX_SDP_SIZE], sub_sdp[P2P_SIG_MAX_SDP_SIZE];
    p2p_peer_get_local_description(pub_peer, pub_sdp, sizeof(pub_sdp));
    p2p_peer_get_local_description(sub_peer, sub_sdp, sizeof(sub_sdp));

    p2p_peer_set_remote_description(pub_peer, sub_sdp);
    p2p_peer_set_remote_description(sub_peer, pub_sdp);

    p2p_peer_gather_candidates(pub_peer);
    p2p_peer_gather_candidates(sub_peer);

    if (wait_flags(&g_pub_gathering_done, &g_sub_gathering_done, 5000) != 0) {
        fprintf(stderr, "FAIL: gathering timeout\n");
        return 1;
    }

    for (int i = 0; i < g_pub_candidate_count; i++)
        p2p_peer_add_remote_candidate(sub_peer, g_pub_candidates[i]);
    for (int i = 0; i < g_sub_candidate_count; i++)
        p2p_peer_add_remote_candidate(pub_peer, g_sub_candidates[i]);

    p2p_peer_set_remote_gathering_done(pub_peer);
    p2p_peer_set_remote_gathering_done(sub_peer);

    if (wait_flags(&g_pub_connected, &g_sub_connected, 10000) != 0) {
        fprintf(stderr, "FAIL: ICE connection timeout\n");
        return 1;
    }
    printf("[OK] ICE connected\n\n");

    /* ===== Phase 1: Send video, verify subscriber receives ===== */
    printf("--- Phase 1: verify video flows normally ---\n");
    g_sub_video_recv = 0;
    for (uint32_t i = 0; i < 30; i++) {
        send_video_frame(pub_peer, i);
        usleep(33000);
    }
    usleep(300000);
    int phase1_recv = g_sub_video_recv;
    printf("  sent=30, recv=%d\n", phase1_recv);
    if (phase1_recv <= 0) {
        fprintf(stderr, "FAIL: Phase 1 - no video received\n");
        p2p_engine_destroy(&pub_engine);
        p2p_engine_destroy(&sub_engine);
        return 1;
    }
    printf("[OK] Phase 1 passed (recv=%d)\n\n", phase1_recv);

    /* ===== Phase 2: Subscriber sends VIDEO_STOP, verify publisher stops ===== */
    printf("--- Phase 2: subscriber sends VIDEO_STOP ---\n");
    g_pub_ctrl_recv_type = 0;
    int rc = p2p_peer_send_video_stop(sub_peer);
    printf("  p2p_peer_send_video_stop returned %d\n", rc);
    usleep(500000);

    if (g_pub_ctrl_recv_type != P2P_FRAME_TYPE_VIDEO_STOP) {
        fprintf(stderr, "FAIL: publisher did not receive VIDEO_STOP (got type=0x%02x)\n",
                g_pub_ctrl_recv_type);
        p2p_engine_destroy(&pub_engine);
        p2p_engine_destroy(&sub_engine);
        return 1;
    }
    if (!pub_peer->video_paused) {
        fprintf(stderr, "FAIL: pub_peer->video_paused not set\n");
        p2p_engine_destroy(&pub_engine);
        p2p_engine_destroy(&sub_engine);
        return 1;
    }
    printf("[OK] Publisher received VIDEO_STOP, video_paused=%d\n", pub_peer->video_paused);

    /* Send more frames - should be skipped due to video_paused check */
    g_sub_video_recv = 0;
    for (uint32_t i = 30; i < 60; i++) {
        send_video_frame(pub_peer, i);
        usleep(33000);
    }
    usleep(300000);
    int phase2_recv = g_sub_video_recv;
    printf("  sent(attempted)=30, recv=%d (should be 0)\n", phase2_recv);
    if (phase2_recv != 0) {
        fprintf(stderr, "FAIL: Phase 2 - subscriber still receiving video after VIDEO_STOP (recv=%d)\n",
                phase2_recv);
        p2p_engine_destroy(&pub_engine);
        p2p_engine_destroy(&sub_engine);
        return 1;
    }
    printf("[OK] Phase 2 passed - no video after VIDEO_STOP\n\n");

    /* ===== Phase 3: Subscriber sends VIDEO_START, verify resume ===== */
    printf("--- Phase 3: subscriber sends VIDEO_START ---\n");
    g_pub_ctrl_recv_type = 0;
    rc = p2p_peer_send_video_start(sub_peer);
    printf("  p2p_peer_send_video_start returned %d\n", rc);
    usleep(500000);

    if (!g_pub_ctrl_recv_type || pub_peer->video_paused) {
        fprintf(stderr, "FAIL: VIDEO_START not processed (type=0x%02x paused=%d)\n",
                g_pub_ctrl_recv_type, pub_peer->video_paused);
        p2p_engine_destroy(&pub_engine);
        p2p_engine_destroy(&sub_engine);
        return 1;
    }

    g_sub_video_recv = 0;
    for (uint32_t i = 60; i < 90; i++) {
        send_video_frame(pub_peer, i);
        usleep(33000);
    }
    usleep(300000);
    int phase3_recv = g_sub_video_recv;
    printf("  sent=30, recv=%d\n", phase3_recv);
    if (phase3_recv <= 0) {
        fprintf(stderr, "FAIL: Phase 3 - video did not resume after VIDEO_START\n");
        p2p_engine_destroy(&pub_engine);
        p2p_engine_destroy(&sub_engine);
        return 1;
    }
    printf("[OK] Phase 3 passed - video resumed (recv=%d)\n\n", phase3_recv);

    /* Cleanup */
    p2p_engine_destroy(&pub_engine);
    p2p_engine_destroy(&sub_engine);

    printf("=== ALL TESTS PASSED ===\n");
    return 0;
}
