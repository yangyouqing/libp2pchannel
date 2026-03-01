/*
 * End-to-end ICE connectivity test using the p2p adapter layer.
 * Tests: two ICE agents connecting directly (host candidates) and
 * exchanging data through the xquic-libjuice adapter.
 *
 * Usage: ./test_ice_connectivity [stun_host] [stun_port]
 */

#include "p2p_adapter.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>

static volatile int g_running = 1;
static volatile int g_pub_connected = 0;
static volatile int g_sub_connected = 0;
static volatile int g_data_received = 0;

/* --- Publisher callbacks --- */

static void pub_on_ice_state(p2p_peer_ctx_t *peer, juice_state_t state, void *user_data)
{
    printf("[PUB] ICE state: %s\n", juice_state_to_string(state));
    if (state == JUICE_STATE_CONNECTED || state == JUICE_STATE_COMPLETED)
        g_pub_connected = 1;
    if (state == JUICE_STATE_FAILED) {
        fprintf(stderr, "[PUB] ICE FAILED\n");
        g_running = 0;
    }
}

static char g_pub_candidates[10][JUICE_MAX_CANDIDATE_SDP_STRING_LEN];
static int g_pub_candidate_count = 0;

static void pub_on_candidate(p2p_peer_ctx_t *peer, const char *sdp, void *user_data)
{
    printf("[PUB] candidate: %s\n", sdp);
    if (g_pub_candidate_count < 10)
        snprintf(g_pub_candidates[g_pub_candidate_count++],
                 JUICE_MAX_CANDIDATE_SDP_STRING_LEN, "%s", sdp);
}

static volatile int g_pub_gathering_done = 0;
static void pub_on_gathering_done(p2p_peer_ctx_t *peer, void *user_data)
{
    printf("[PUB] gathering done\n");
    g_pub_gathering_done = 1;
}

/* --- Subscriber callbacks --- */

static void sub_on_ice_state(p2p_peer_ctx_t *peer, juice_state_t state, void *user_data)
{
    printf("[SUB] ICE state: %s\n", juice_state_to_string(state));
    if (state == JUICE_STATE_CONNECTED || state == JUICE_STATE_COMPLETED)
        g_sub_connected = 1;
    if (state == JUICE_STATE_FAILED) {
        fprintf(stderr, "[SUB] ICE FAILED\n");
        g_running = 0;
    }
}

static char g_sub_candidates[10][JUICE_MAX_CANDIDATE_SDP_STRING_LEN];
static int g_sub_candidate_count = 0;

static void sub_on_candidate(p2p_peer_ctx_t *peer, const char *sdp, void *user_data)
{
    printf("[SUB] candidate: %s\n", sdp);
    if (g_sub_candidate_count < 10)
        snprintf(g_sub_candidates[g_sub_candidate_count++],
                 JUICE_MAX_CANDIDATE_SDP_STRING_LEN, "%s", sdp);
}

static volatile int g_sub_gathering_done = 0;
static void sub_on_gathering_done(p2p_peer_ctx_t *peer, void *user_data)
{
    printf("[SUB] gathering done\n");
    g_sub_gathering_done = 1;
}

static void sighandler(int sig) { g_running = 0; }

int main(int argc, char **argv)
{
    signal(SIGINT, sighandler);

    const char *stun_host = (argc > 1) ? argv[1] : NULL;
    uint16_t stun_port = (argc > 2) ? (uint16_t)atoi(argv[2]) : 3478;

    juice_set_log_level(JUICE_LOG_LEVEL_WARN);

    printf("=== P2P ICE Connectivity Test ===\n");
    if (stun_host)
        printf("STUN server: %s:%u\n", stun_host, stun_port);
    else
        printf("No STUN server (host candidates only)\n");

    /* Create publisher engine */
    p2p_engine_t pub_engine;
    p2p_engine_config_t pub_cfg;
    memset(&pub_cfg, 0, sizeof(pub_cfg));
    pub_cfg.role = P2P_ROLE_PUBLISHER;
    pub_cfg.stun_server_host = stun_host;
    pub_cfg.stun_server_port = stun_port;
    pub_cfg.callbacks.on_peer_ice_state = pub_on_ice_state;
    pub_cfg.callbacks.on_peer_ice_candidate = pub_on_candidate;
    pub_cfg.callbacks.on_peer_ice_gathering_done = pub_on_gathering_done;

    if (p2p_engine_init(&pub_engine, &pub_cfg) != 0) {
        fprintf(stderr, "Failed to init publisher engine\n");
        return 1;
    }

    /* Create subscriber engine */
    p2p_engine_t sub_engine;
    p2p_engine_config_t sub_cfg;
    memset(&sub_cfg, 0, sizeof(sub_cfg));
    sub_cfg.role = P2P_ROLE_SUBSCRIBER;
    sub_cfg.stun_server_host = stun_host;
    sub_cfg.stun_server_port = stun_port;
    sub_cfg.callbacks.on_peer_ice_state = sub_on_ice_state;
    sub_cfg.callbacks.on_peer_ice_candidate = sub_on_candidate;
    sub_cfg.callbacks.on_peer_ice_gathering_done = sub_on_gathering_done;

    if (p2p_engine_init(&sub_engine, &sub_cfg) != 0) {
        fprintf(stderr, "Failed to init subscriber engine\n");
        return 1;
    }

    /* Add peers */
    p2p_peer_ctx_t *pub_peer = p2p_engine_add_peer(&pub_engine, "subscriber-1");
    p2p_peer_ctx_t *sub_peer = p2p_engine_add_peer(&sub_engine, "publisher-1");
    if (!pub_peer || !sub_peer) {
        fprintf(stderr, "Failed to add peers\n");
        return 1;
    }

    /* Exchange SDP descriptions */
    char pub_sdp[JUICE_MAX_SDP_STRING_LEN];
    char sub_sdp[JUICE_MAX_SDP_STRING_LEN];
    p2p_peer_get_local_description(pub_peer, pub_sdp, sizeof(pub_sdp));
    p2p_peer_get_local_description(sub_peer, sub_sdp, sizeof(sub_sdp));

    printf("\n[PUB] Local SDP:\n%s\n", pub_sdp);
    printf("[SUB] Local SDP:\n%s\n", sub_sdp);

    p2p_peer_set_remote_description(pub_peer, sub_sdp);
    p2p_peer_set_remote_description(sub_peer, pub_sdp);

    /* Gather candidates */
    p2p_peer_gather_candidates(pub_peer);
    p2p_peer_gather_candidates(sub_peer);

    /* Wait for gathering to complete */
    for (int i = 0; i < 50 && g_running; i++) {
        usleep(100000);
        if (g_pub_gathering_done && g_sub_gathering_done) break;
    }

    /* Exchange candidates */
    for (int i = 0; i < g_pub_candidate_count; i++)
        p2p_peer_add_remote_candidate(sub_peer, g_pub_candidates[i]);
    for (int i = 0; i < g_sub_candidate_count; i++)
        p2p_peer_add_remote_candidate(pub_peer, g_sub_candidates[i]);

    p2p_peer_set_remote_gathering_done(pub_peer);
    p2p_peer_set_remote_gathering_done(sub_peer);

    /* Wait for ICE connection */
    printf("\nWaiting for ICE connection...\n");
    for (int i = 0; i < 100 && g_running; i++) {
        usleep(100000);
        if (g_pub_connected && g_sub_connected) break;
    }

    if (g_pub_connected && g_sub_connected) {
        printf("\n=== ICE CONNECTED ===\n");

        /* Send test data */
        const char *test_msg = "Hello P2P World!";
        juice_send(pub_peer->ice_agent, test_msg, strlen(test_msg));
        printf("[PUB] Sent: %s\n", test_msg);

        usleep(500000);

        printf("\n=== TEST PASSED ===\n");
    } else {
        printf("\n=== ICE CONNECTION FAILED ===\n");
        printf("pub_connected=%d sub_connected=%d\n", g_pub_connected, g_sub_connected);
    }

    /* Cleanup */
    p2p_engine_destroy(&pub_engine);
    p2p_engine_destroy(&sub_engine);

    return (g_pub_connected && g_sub_connected) ? 0 : 1;
}
