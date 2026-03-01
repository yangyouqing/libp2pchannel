#include "p2p_publisher.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- adapter callbacks ---- */

static void pub_on_ice_state(p2p_peer_ctx_t *peer, juice_state_t state, void *user_data)
{
    p2p_publisher_t *pub = (p2p_publisher_t *)user_data;
    fprintf(stderr, "[pub] peer %s ICE state: %s\n",
            peer->peer_id, juice_state_to_string(state));

    if (state == JUICE_STATE_CONNECTED || state == JUICE_STATE_COMPLETED) {
        /* ICE connected – start QUIC handshake.
         * Publisher acts as QUIC "server", so just wait for the subscriber
         * to initiate the QUIC connection.  No action needed here. */
    }
}

static void pub_on_ice_candidate(p2p_peer_ctx_t *peer, const char *sdp, void *user_data)
{
    p2p_publisher_t *pub = (p2p_publisher_t *)user_data;
    p2p_signaling_send_ice_candidate(&pub->signaling, peer->peer_id, sdp);
}

static void pub_on_ice_gathering_done(p2p_peer_ctx_t *peer, void *user_data)
{
    p2p_publisher_t *pub = (p2p_publisher_t *)user_data;
    p2p_signaling_send_gathering_done(&pub->signaling, peer->peer_id);
}

static void pub_on_quic_connected(p2p_peer_ctx_t *peer, void *user_data)
{
    p2p_publisher_t *pub = (p2p_publisher_t *)user_data;
    fprintf(stderr, "[pub] QUIC connected with subscriber %s\n", peer->peer_id);

    /* Send cached IDR frame for fast first-frame delivery */
    pthread_mutex_lock(&pub->idr_mutex);
    if (pub->idr_cache && pub->idr_cache_size > 0) {
        /* The cached IDR is sent directly via the ICE channel for immediate display */
        juice_send(peer->ice_agent, (const char *)pub->idr_cache, pub->idr_cache_size);
    }
    pthread_mutex_unlock(&pub->idr_mutex);
}

static void pub_on_quic_closed(p2p_peer_ctx_t *peer, void *user_data)
{
    p2p_publisher_t *pub = (p2p_publisher_t *)user_data;
    fprintf(stderr, "[pub] QUIC closed with subscriber %s\n", peer->peer_id);
}

/* ---- signaling callbacks ---- */

static void pub_sig_connected(p2p_signaling_client_t *c, void *user_data)
{
    p2p_publisher_t *pub = (p2p_publisher_t *)user_data;
    fprintf(stderr, "[pub] signaling connected, creating room %s\n", pub->room_id);
    p2p_signaling_create_room(c, pub->room_id);
}

static void pub_sig_disconnected(p2p_signaling_client_t *c, void *user_data)
{
    fprintf(stderr, "[pub] signaling disconnected\n");
}

static void pub_sig_room_created(p2p_signaling_client_t *c, const char *room_id, void *user_data)
{
    fprintf(stderr, "[pub] room created: %s\n", room_id);
}

static void pub_sig_peer_joined(p2p_signaling_client_t *c, const char *peer_id, void *user_data)
{
    p2p_publisher_t *pub = (p2p_publisher_t *)user_data;
    fprintf(stderr, "[pub] subscriber joined: %s\n", peer_id);

    /* Create a new peer context for this subscriber */
    p2p_peer_ctx_t *peer = p2p_engine_add_peer(&pub->engine, peer_id);
    if (!peer) {
        fprintf(stderr, "[pub] cannot add peer %s (max subscribers reached?)\n", peer_id);
        return;
    }

    /* Get local ICE description and send as offer */
    char sdp[JUICE_MAX_SDP_STRING_LEN];
    p2p_peer_get_local_description(peer, sdp, sizeof(sdp));
    p2p_signaling_send_ice_offer(c, peer_id, sdp);

    /* Start candidate gathering */
    p2p_peer_gather_candidates(peer);
}

static void pub_sig_peer_left(p2p_signaling_client_t *c, const char *peer_id, void *user_data)
{
    p2p_publisher_t *pub = (p2p_publisher_t *)user_data;
    p2p_peer_ctx_t *peer = p2p_engine_find_peer(&pub->engine, peer_id);
    if (peer) {
        fprintf(stderr, "[pub] subscriber left: %s\n", peer_id);
        p2p_engine_remove_peer(&pub->engine, peer->index);
    }
}

static void pub_sig_ice_answer(p2p_signaling_client_t *c, const char *from,
                                const char *sdp, void *user_data)
{
    p2p_publisher_t *pub = (p2p_publisher_t *)user_data;
    p2p_peer_ctx_t *peer = p2p_engine_find_peer(&pub->engine, from);
    if (peer)
        p2p_peer_set_remote_description(peer, sdp);
}

static void pub_sig_ice_candidate(p2p_signaling_client_t *c, const char *from,
                                   const char *candidate, void *user_data)
{
    p2p_publisher_t *pub = (p2p_publisher_t *)user_data;
    p2p_peer_ctx_t *peer = p2p_engine_find_peer(&pub->engine, from);
    if (peer)
        p2p_peer_add_remote_candidate(peer, candidate);
}

static void pub_sig_gathering_done(p2p_signaling_client_t *c, const char *from, void *user_data)
{
    p2p_publisher_t *pub = (p2p_publisher_t *)user_data;
    p2p_peer_ctx_t *peer = p2p_engine_find_peer(&pub->engine, from);
    if (peer)
        p2p_peer_set_remote_gathering_done(peer);
}

/* ---- public API ---- */

int p2p_publisher_init(p2p_publisher_t *pub, const p2p_publisher_config_t *config)
{
    if (!pub || !config) return -1;
    memset(pub, 0, sizeof(*pub));

    snprintf(pub->room_id, sizeof(pub->room_id), "%s",
             config->room_id ? config->room_id : "default");

    /* Allocate IDR cache */
    pub->idr_cache = malloc(P2P_PUB_MAX_IDR_CACHE_SIZE);
    if (!pub->idr_cache) return -1;
    pub->idr_cache_size = 0;
    pthread_mutex_init(&pub->idr_mutex, NULL);

    /* Initialize P2P engine as publisher (QUIC server) */
    p2p_engine_config_t ecfg;
    memset(&ecfg, 0, sizeof(ecfg));
    ecfg.role = P2P_ROLE_PUBLISHER;
    ecfg.stun_server_host = config->stun_host;
    ecfg.stun_server_port = config->stun_port;
    ecfg.turn_server_host = config->turn_host;
    ecfg.turn_server_port = config->turn_port;
    ecfg.turn_username = config->turn_username;
    ecfg.turn_password = config->turn_password;
    ecfg.ssl_cert_file = config->ssl_cert_file;
    ecfg.ssl_key_file = config->ssl_key_file;
    ecfg.callbacks.on_peer_ice_state = pub_on_ice_state;
    ecfg.callbacks.on_peer_ice_candidate = pub_on_ice_candidate;
    ecfg.callbacks.on_peer_ice_gathering_done = pub_on_ice_gathering_done;
    ecfg.callbacks.on_peer_quic_connected = pub_on_quic_connected;
    ecfg.callbacks.on_peer_quic_closed = pub_on_quic_closed;
    ecfg.user_data = pub;

    if (p2p_engine_init(&pub->engine, &ecfg) != 0)
        return -1;

    return 0;
}

int p2p_publisher_start(p2p_publisher_t *pub)
{
    if (!pub) return -1;

    /* Start the P2P engine thread */
    if (p2p_engine_start(&pub->engine) != 0) return -1;
    pub->running = 1;

    /* Connect to signaling server */
    p2p_signaling_config_t scfg;
    memset(&scfg, 0, sizeof(scfg));
    scfg.server_url = pub->engine.stun_host; /* reused field; real impl passes signaling URL */
    scfg.peer_id = pub->engine.turn_username; /* placeholder */
    scfg.callbacks.on_connected = pub_sig_connected;
    scfg.callbacks.on_disconnected = pub_sig_disconnected;
    scfg.callbacks.on_room_created = pub_sig_room_created;
    scfg.callbacks.on_peer_joined = pub_sig_peer_joined;
    scfg.callbacks.on_peer_left = pub_sig_peer_left;
    scfg.callbacks.on_ice_answer = pub_sig_ice_answer;
    scfg.callbacks.on_ice_candidate = pub_sig_ice_candidate;
    scfg.callbacks.on_gathering_done = pub_sig_gathering_done;
    scfg.user_data = pub;

    /* Note: actual connect to signaling deferred until user calls with correct URL */

    return 0;
}

void p2p_publisher_stop(p2p_publisher_t *pub)
{
    if (!pub) return;
    pub->running = 0;
    p2p_signaling_disconnect(&pub->signaling);
    p2p_engine_stop(&pub->engine);
}

void p2p_publisher_destroy(p2p_publisher_t *pub)
{
    if (!pub) return;
    p2p_engine_destroy(&pub->engine);
    free(pub->idr_cache);
    pub->idr_cache = NULL;
    pthread_mutex_destroy(&pub->idr_mutex);
}

int p2p_publisher_send_video(p2p_publisher_t *pub, const uint8_t *data,
                              size_t size, uint64_t timestamp_us, int is_keyframe)
{
    if (!pub || !data || size == 0) return -1;

    /* Cache IDR frame for fast first-frame delivery to new subscribers */
    if (is_keyframe && size <= P2P_PUB_MAX_IDR_CACHE_SIZE) {
        pthread_mutex_lock(&pub->idr_mutex);
        memcpy(pub->idr_cache, data, size);
        pub->idr_cache_size = size;
        pub->idr_timestamp_us = timestamp_us;
        pthread_mutex_unlock(&pub->idr_mutex);
    }

    /* Send to all connected subscribers */
    for (int i = 0; i < P2P_MAX_SUBSCRIBERS; i++) {
        p2p_peer_ctx_t *peer = &pub->engine.peers[i];
        if (peer->state == P2P_PEER_STATE_QUIC_CONNECTED && peer->ice_agent) {
            juice_send(peer->ice_agent, (const char *)data, size);
        }
    }
    return 0;
}

int p2p_publisher_send_audio(p2p_publisher_t *pub, const uint8_t *data,
                              size_t size, uint64_t timestamp_us)
{
    if (!pub || !data || size == 0) return -1;

    for (int i = 0; i < P2P_MAX_SUBSCRIBERS; i++) {
        p2p_peer_ctx_t *peer = &pub->engine.peers[i];
        if (peer->state == P2P_PEER_STATE_QUIC_CONNECTED && peer->ice_agent) {
            juice_send(peer->ice_agent, (const char *)data, size);
        }
    }
    return 0;
}

int p2p_publisher_get_subscriber_count(p2p_publisher_t *pub)
{
    if (!pub) return 0;
    return pub->engine.peer_count;
}
