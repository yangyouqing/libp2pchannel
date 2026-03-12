/*
 * test_av_continuity.c -- Local loopback AV continuity test.
 * No signaling server or JWT required.
 */
#include "p2p_adapter.h"
#include "p2p_signaling.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>

#define NUM_VIDEO_FRAMES   200
#define NUM_AUDIO_FRAMES   300
#define VIDEO_PAYLOAD_SIZE 2048
#define AUDIO_PAYLOAD_SIZE 160
#define MAX_CAND           10
#define MAX_CAND_LEN       256
#define TIMEOUT_SEC        15

static volatile int g_running = 1;
static void sighandler(int sig) { (void)sig; g_running = 0; }

static char     g_pub_cands[MAX_CAND][MAX_CAND_LEN];
static int      g_pub_cand_cnt = 0;
static volatile int g_pub_gathering_done = 0;

static char     g_sub_cands[MAX_CAND][MAX_CAND_LEN];
static int      g_sub_cand_cnt = 0;
static volatile int g_sub_gathering_done = 0;

static volatile int g_pub_quic_connected = 0;
static volatile int g_sub_quic_connected = 0;
static volatile int g_pub_ice_connected  = 0;
static volatile int g_sub_ice_connected  = 0;
static volatile int g_ice_failed         = 0;

static volatile int g_video_recv_count     = 0;
static volatile int g_audio_recv_count     = 0;
static volatile int g_video_order_errors   = 0;
static volatile uint32_t g_last_video_seq  = 0;
static volatile int g_first_video_received = 0;
static volatile int g_ctrl_idr_received    = 0;

static void pub_on_ice_state(p2p_peer_ctx_t *peer, p2p_ice_state_t state, void *ud)
{
    (void)peer; (void)ud;
    fprintf(stderr, "[PUB] ICE state: %d\n", state);
    if (state == P2P_ICE_STATE_CONNECTED || state == P2P_ICE_STATE_COMPLETED)
        g_pub_ice_connected = 1;
    if (state == P2P_ICE_STATE_FAILED) { g_ice_failed = 1; g_running = 0; }
}

static void pub_on_candidate(p2p_peer_ctx_t *peer, const char *sdp, void *ud)
{
    (void)peer; (void)ud;
    if (g_pub_cand_cnt < MAX_CAND)
        snprintf(g_pub_cands[g_pub_cand_cnt++], MAX_CAND_LEN, "%s", sdp);
}

static void pub_on_gathering_done(p2p_peer_ctx_t *peer, void *ud)
{ (void)peer; (void)ud; g_pub_gathering_done = 1; }

static void pub_on_quic_connected(p2p_peer_ctx_t *peer, void *ud)
{ (void)peer; (void)ud; fprintf(stderr, "[PUB] QUIC connected\n"); g_pub_quic_connected = 1; }

static void sub_on_ice_state(p2p_peer_ctx_t *peer, p2p_ice_state_t state, void *ud)
{
    (void)peer; (void)ud;
    fprintf(stderr, "[SUB] ICE state: %d\n", state);
    if (state == P2P_ICE_STATE_CONNECTED || state == P2P_ICE_STATE_COMPLETED)
        g_sub_ice_connected = 1;
    if (state == P2P_ICE_STATE_FAILED) { g_ice_failed = 1; g_running = 0; }
}

static void sub_on_candidate(p2p_peer_ctx_t *peer, const char *sdp, void *ud)
{
    (void)peer; (void)ud;
    if (g_sub_cand_cnt < MAX_CAND)
        snprintf(g_sub_cands[g_sub_cand_cnt++], MAX_CAND_LEN, "%s", sdp);
}

static void sub_on_gathering_done(p2p_peer_ctx_t *peer, void *ud)
{ (void)peer; (void)ud; g_sub_gathering_done = 1; }

static void sub_on_quic_connected(p2p_peer_ctx_t *peer, void *ud)
{ (void)peer; (void)ud; fprintf(stderr, "[SUB] QUIC connected\n"); g_sub_quic_connected = 1; }

static void sub_on_data_recv(p2p_peer_ctx_t *peer, const p2p_frame_header_t *hdr,
                             const uint8_t *payload, void *ud)
{
    (void)peer; (void)ud; (void)payload;
    switch (hdr->type) {
    case P2P_FRAME_TYPE_VIDEO:
        if (hdr->frag_offset == 0) {
            if (g_first_video_received && hdr->seq <= g_last_video_seq)
                g_video_order_errors++;
            g_last_video_seq = hdr->seq;
            g_first_video_received = 1;
            g_video_recv_count++;
        }
        break;
    case P2P_FRAME_TYPE_AUDIO:
        if (hdr->frag_offset == 0) g_audio_recv_count++;
        break;
    case P2P_FRAME_TYPE_IDR_REQ:
        g_ctrl_idr_received = 1;
        break;
    default: break;
    }
}

static int wait_both(volatile int *a, volatile int *b, int timeout_ms)
{
    int elapsed = 0;
    while ((!*a || !*b) && elapsed < timeout_ms && g_running) {
        usleep(50000);
        elapsed += 50;
    }
    return (*a && *b) ? 0 : -1;
}

static void reset_globals(void)
{
    g_pub_cand_cnt = g_sub_cand_cnt = 0;
    g_pub_gathering_done = g_sub_gathering_done = 0;
    g_pub_quic_connected = g_sub_quic_connected = 0;
    g_pub_ice_connected = g_sub_ice_connected = 0;
    g_ice_failed = 0;
    g_video_recv_count = g_audio_recv_count = 0;
    g_video_order_errors = 0;
    g_last_video_seq = 0;
    g_first_video_received = 0;
    g_ctrl_idr_received = 0;
}

static int setup_loopback(p2p_engine_t *pub_eng, p2p_engine_t *sub_eng,
                          p2p_engine_config_t *pub_cfg, p2p_engine_config_t *sub_cfg,
                          p2p_peer_ctx_t **out_pp, p2p_peer_ctx_t **out_sp)
{
    if (p2p_engine_init(pub_eng, pub_cfg) != 0) return -1;
    if (p2p_engine_start(pub_eng) != 0) {
        p2p_engine_destroy(pub_eng); return -1;
    }
    if (p2p_engine_init(sub_eng, sub_cfg) != 0) {
        p2p_engine_stop(pub_eng); p2p_engine_destroy(pub_eng); return -1;
    }
    if (p2p_engine_start(sub_eng) != 0) {
        p2p_engine_destroy(sub_eng);
        p2p_engine_stop(pub_eng); p2p_engine_destroy(pub_eng); return -1;
    }

    *out_pp = p2p_engine_add_peer(pub_eng, "subscriber-1");
    *out_sp = p2p_engine_add_peer(sub_eng, "publisher-1");
    if (!*out_pp || !*out_sp) return -2;

    char ps[P2P_SIG_MAX_SDP_SIZE], ss[P2P_SIG_MAX_SDP_SIZE];
    p2p_peer_get_local_description(*out_pp, ps, sizeof(ps));
    p2p_peer_get_local_description(*out_sp, ss, sizeof(ss));
    p2p_peer_set_remote_description(*out_pp, ss);
    p2p_peer_set_remote_description(*out_sp, ps);

    p2p_peer_gather_candidates(*out_pp);
    p2p_peer_gather_candidates(*out_sp);
    if (wait_both(&g_pub_gathering_done, &g_sub_gathering_done, 5000) != 0)
        return -3;

    for (int i = 0; i < g_pub_cand_cnt; i++)
        p2p_peer_add_remote_candidate(*out_sp, g_pub_cands[i]);
    for (int i = 0; i < g_sub_cand_cnt; i++)
        p2p_peer_add_remote_candidate(*out_pp, g_sub_cands[i]);
    p2p_peer_set_remote_gathering_done(*out_pp);
    p2p_peer_set_remote_gathering_done(*out_sp);

    if (wait_both(&g_pub_quic_connected, &g_sub_quic_connected, TIMEOUT_SEC * 1000) != 0)
        return -4;

    usleep(200000);
    return 0;
}

static void teardown_loopback(p2p_engine_t *pub_eng, p2p_engine_t *sub_eng)
{
    p2p_engine_stop(pub_eng);
    p2p_engine_stop(sub_eng);
    p2p_engine_destroy(pub_eng);
    p2p_engine_destroy(sub_eng);
}

static int test_jitter_buffer(void)
{
    printf("[TC1] jitter_buffer      ");
    fflush(stdout);

    p2p_video_jitter_buf_t jb;
    p2p_video_jitter_init(&jb, 0);

    int w = 8, h = 4, lsy = w, lsu = w/2, lsv = w/2;
    int ysz = lsy*h, usz = lsu*(h/2), vsz = lsv*(h/2);
    uint8_t *y = (uint8_t*)malloc(ysz);
    uint8_t *u = (uint8_t*)malloc(usz);
    uint8_t *v = (uint8_t*)malloc(vsz);

    int push_n = 5;
    for (int i = 0; i < push_n; i++) {
        memset(y, 0x10+i, ysz); memset(u, 0x80+i, usz); memset(v, 0x90+i, vsz);
        p2p_video_jitter_push(&jb, y, u, v, lsy, lsu, lsv, w, h, (uint64_t)(i+1)*33333);
    }

    int pop_n = 0, order_ok = 1;
    uint64_t prev = 0;
    p2p_jitter_frame_t out;
    while (p2p_video_jitter_pop(&jb, &out)) {
        if (!out.valid) { order_ok = 0; break; }
        if (out.timestamp_us <= prev && pop_n > 0) order_ok = 0;
        prev = out.timestamp_us;
        free(out.y); free(out.u); free(out.v);
        pop_n++;
    }

    free(y); free(u); free(v);
    p2p_video_jitter_destroy(&jb);

    int pass = (pop_n == push_n && order_ok);
    printf("%s  (pushed=%d popped=%d order=%d)\n", pass?"PASS":"FAIL", push_n, pop_n, order_ok);
    return pass ? 0 : 1;
}

static int test_jitter_overflow(void)
{
    printf("[TC2] jitter_overflow    ");
    fflush(stdout);

    p2p_video_jitter_buf_t jb;
    p2p_video_jitter_init(&jb, 0);

    int w = 4, h = 2, lsy = w, lsu = w/2, lsv = w/2;
    int ysz = lsy*h, usz = lsu*(h/2), vsz = lsv*(h/2);
    uint8_t *y = (uint8_t*)malloc(ysz);
    uint8_t *u = (uint8_t*)malloc(usz);
    uint8_t *v = (uint8_t*)malloc(vsz);

    int total = P2P_VIDEO_JITTER_SIZE + 4;
    for (int i = 0; i < total; i++) {
        memset(y, i, ysz); memset(u, i, usz); memset(v, i, vsz);
        p2p_video_jitter_push(&jb, y, u, v, lsy, lsu, lsv, w, h, (uint64_t)(i+1)*33333);
    }

    int pop_n = 0;
    p2p_jitter_frame_t out;
    while (p2p_video_jitter_pop(&jb, &out)) {
        free(out.y); free(out.u); free(out.v);
        pop_n++;
    }

    free(y); free(u); free(v);
    p2p_video_jitter_destroy(&jb);

    int pass = (pop_n == P2P_VIDEO_JITTER_SIZE);
    printf("%s  (pushed=%d popped=%d max=%d)\n", pass?"PASS":"FAIL", total, pop_n, P2P_VIDEO_JITTER_SIZE);
    return pass ? 0 : 1;
}

static int test_av_loopback(void)
{
    printf("[TC3] av_loopback       ");
    fflush(stdout);
    reset_globals();

    p2p_engine_t pe, se;
    p2p_engine_config_t pc, sc;
    memset(&pc, 0, sizeof(pc));
    pc.role = P2P_ROLE_PUBLISHER;
    pc.callbacks.on_peer_ice_state = pub_on_ice_state;
    pc.callbacks.on_peer_ice_candidate = pub_on_candidate;
    pc.callbacks.on_peer_ice_gathering_done = pub_on_gathering_done;
    pc.callbacks.on_peer_quic_connected = pub_on_quic_connected;

    memset(&sc, 0, sizeof(sc));
    sc.role = P2P_ROLE_SUBSCRIBER;
    sc.callbacks.on_peer_ice_state = sub_on_ice_state;
    sc.callbacks.on_peer_ice_candidate = sub_on_candidate;
    sc.callbacks.on_peer_ice_gathering_done = sub_on_gathering_done;
    sc.callbacks.on_peer_quic_connected = sub_on_quic_connected;
    sc.callbacks.on_peer_data_recv = sub_on_data_recv;

    p2p_peer_ctx_t *pp, *sp;
    int r = setup_loopback(&pe, &se, &pc, &sc, &pp, &sp);
    if (r != 0) {
        printf("FAIL  (setup=%d ice=%d/%d quic=%d/%d)\n", r,
               g_pub_ice_connected, g_sub_ice_connected,
               g_pub_quic_connected, g_sub_quic_connected);
        if (r <= -2) teardown_loopback(&pe, &se);
        return 1;
    }

    fprintf(stderr, "[TEST] QUIC connected, sending AV...\n");

    uint8_t vp[VIDEO_PAYLOAD_SIZE];
    memset(vp, 0x42, sizeof(vp));
    for (int i = 0; i < NUM_VIDEO_FRAMES && g_running; i++) {
        uint8_t fl = (i % 30 == 0) ? P2P_FRAME_FLAG_KEY : 0;
        p2p_peer_send_data_via_quic(pp, P2P_FRAME_TYPE_VIDEO, fl,
            (uint32_t)(i+1), (uint64_t)(i+1)*33333, vp, sizeof(vp));
        usleep(33000);
    }

    uint8_t ap2[AUDIO_PAYLOAD_SIZE];
    memset(ap2, 0x11, sizeof(ap2));
    for (int i = 0; i < NUM_AUDIO_FRAMES && g_running; i++) {
        p2p_peer_send_data_via_quic(pp, P2P_FRAME_TYPE_AUDIO, 0,
            (uint32_t)(i+1), (uint64_t)(i+1)*20000, ap2, sizeof(ap2));
        usleep(20000);
    }

    p2p_peer_send_idr_request(sp);
    usleep(2000000);

    int vc = g_video_recv_count, ac = g_audio_recv_count;
    int vo = g_video_order_errors, idr = g_ctrl_idr_received;
    float vrat = (float)vc / NUM_VIDEO_FRAMES;
    float arat = (float)ac / NUM_AUDIO_FRAMES;
    int pass = (vrat >= 0.90f && arat >= 0.90f && vo == 0);

    printf("%s  (v=%d/%d=%.0f%% a=%d/%d=%.0f%% ord_err=%d idr=%d)\n",
           pass?"PASS":"FAIL", vc, NUM_VIDEO_FRAMES, vrat*100,
           ac, NUM_AUDIO_FRAMES, arat*100, vo, idr);

    teardown_loopback(&pe, &se);
    return pass ? 0 : 1;
}

static volatile int g_mv = 0, g_ma = 0;

static void mixed_recv(p2p_peer_ctx_t *peer, const p2p_frame_header_t *hdr,
                       const uint8_t *payload, void *ud)
{
    (void)peer; (void)ud; (void)payload;
    if (hdr->type == P2P_FRAME_TYPE_VIDEO && hdr->frag_offset == 0) g_mv++;
    else if (hdr->type == P2P_FRAME_TYPE_AUDIO && hdr->frag_offset == 0) g_ma++;
}

static int test_mixed_av(void)
{
    printf("[TC4] mixed_av           ");
    fflush(stdout);
    reset_globals();
    g_mv = g_ma = 0;

    p2p_engine_t pe, se;
    p2p_engine_config_t pc, sc;
    memset(&pc, 0, sizeof(pc));
    pc.role = P2P_ROLE_PUBLISHER;
    pc.callbacks.on_peer_ice_state = pub_on_ice_state;
    pc.callbacks.on_peer_ice_candidate = pub_on_candidate;
    pc.callbacks.on_peer_ice_gathering_done = pub_on_gathering_done;
    pc.callbacks.on_peer_quic_connected = pub_on_quic_connected;

    memset(&sc, 0, sizeof(sc));
    sc.role = P2P_ROLE_SUBSCRIBER;
    sc.callbacks.on_peer_ice_state = sub_on_ice_state;
    sc.callbacks.on_peer_ice_candidate = sub_on_candidate;
    sc.callbacks.on_peer_ice_gathering_done = sub_on_gathering_done;
    sc.callbacks.on_peer_quic_connected = sub_on_quic_connected;
    sc.callbacks.on_peer_data_recv = mixed_recv;

    p2p_peer_ctx_t *pp, *sp;
    int r = setup_loopback(&pe, &se, &pc, &sc, &pp, &sp);
    if (r != 0) {
        printf("FAIL  (setup=%d)\n", r);
        if (r <= -2) teardown_loopback(&pe, &se);
        return 1;
    }

    uint8_t vb[1024], ab[AUDIO_PAYLOAD_SIZE];
    memset(vb, 0x55, sizeof(vb));
    memset(ab, 0xAA, sizeof(ab));

    int nv = 150, na = 150;
    for (int i = 0; i < nv && g_running; i++) {
        uint8_t fl = (i%30==0) ? P2P_FRAME_FLAG_KEY : 0;
        p2p_peer_send_data_via_quic(pp, P2P_FRAME_TYPE_VIDEO, fl,
            (uint32_t)(i+1), (uint64_t)(i+1)*33333, vb, sizeof(vb));
        if (i < na)
            p2p_peer_send_data_via_quic(pp, P2P_FRAME_TYPE_AUDIO, 0,
                (uint32_t)(i+1), (uint64_t)(i+1)*20000, ab, sizeof(ab));
        usleep(20000);
    }

    usleep(2000000);

    float vr = (float)g_mv / nv, ar = (float)g_ma / na;
    int pass = (vr >= 0.90f && ar >= 0.90f);
    printf("%s  (v=%d/%d=%.0f%% a=%d/%d=%.0f%%)\n",
           pass?"PASS":"FAIL", g_mv, nv, vr*100, g_ma, na, ar*100);

    teardown_loopback(&pe, &se);
    return pass ? 0 : 1;
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    signal(SIGINT, sighandler);
    signal(SIGTERM, sighandler);
    p2p_adapter_set_log_level(2);

    printf("=== AV Continuity Test Suite ===\n\n");

    int passed = 0, total = 4;
    if (test_jitter_buffer() == 0)   passed++;
    if (test_jitter_overflow() == 0) passed++;
    if (test_av_loopback() == 0)     passed++;
    if (test_mixed_av() == 0)        passed++;

    printf("\n%d/%d PASSED\n", passed, total);
    return (passed == total) ? 0 : 1;
}
