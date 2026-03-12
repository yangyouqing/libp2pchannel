#include "p2p_subscriber.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- adapter callbacks ---- */

static void sub_on_ice_state(p2p_peer_ctx_t *peer, p2p_ice_state_t state, void *user_data)
{
    p2p_subscriber_t *sub = (p2p_subscriber_t *)user_data;
    fprintf(stderr, "[sub] ICE state with publisher: %d\n", state);

    if (state == P2P_ICE_STATE_CONNECTED || state == P2P_ICE_STATE_COMPLETED) {
        fprintf(stderr, "[sub] ICE connected, QUIC auto-started\n");
    }
}

static void sub_on_ice_candidate(p2p_peer_ctx_t *peer, const char *sdp, void *user_data)
{
    p2p_subscriber_t *sub = (p2p_subscriber_t *)user_data;
    p2p_signaling_send_ice_candidate(&sub->signaling, sub->publisher_peer_id, sdp);
}

static void sub_on_ice_gathering_done(p2p_peer_ctx_t *peer, void *user_data)
{
    p2p_subscriber_t *sub = (p2p_subscriber_t *)user_data;
    p2p_signaling_send_gathering_done(&sub->signaling, sub->publisher_peer_id);
}

static void sub_on_quic_connected(p2p_peer_ctx_t *peer, void *user_data)
{
    p2p_subscriber_t *sub = (p2p_subscriber_t *)user_data;
    fprintf(stderr, "[sub] QUIC connected with publisher %s\n", peer->peer_id);
}

static void sub_on_quic_closed(p2p_peer_ctx_t *peer, void *user_data)
{
    fprintf(stderr, "[sub] QUIC closed with publisher\n");
}

/* ---- signaling callbacks ---- */

static void sub_sig_connected(p2p_signaling_client_t *c, void *user_data)
{
    p2p_subscriber_t *sub = (p2p_subscriber_t *)user_data;
    fprintf(stderr, "[sub] signaling connected, joining room %s\n", sub->room_id);
    p2p_signaling_join_room(c, sub->room_id);
}

static void sub_sig_disconnected(p2p_signaling_client_t *c, void *user_data)
{
    fprintf(stderr, "[sub] signaling disconnected\n");
}

static void sub_sig_ice_offer(p2p_signaling_client_t *c, const char *from,
                               const char *sdp, void *user_data)
{
    p2p_subscriber_t *sub = (p2p_subscriber_t *)user_data;
    snprintf(sub->publisher_peer_id, sizeof(sub->publisher_peer_id), "%s", from);

    /* Create peer context for publisher */
    p2p_peer_ctx_t *peer = p2p_engine_find_peer(&sub->engine, from);
    if (!peer) {
        peer = p2p_engine_add_peer(&sub->engine, from);
        if (!peer) {
            fprintf(stderr, "[sub] failed to add publisher peer\n");
            return;
        }
    }

    /* Set remote description (offer) */
    p2p_peer_set_remote_description(peer, sdp);

    /* Get local description (answer) and send back */
    char answer[P2P_SIG_MAX_SDP_SIZE];
    p2p_peer_get_local_description(peer, answer, sizeof(answer));
    p2p_signaling_send_ice_answer(c, from, answer);

    /* Start candidate gathering */
    p2p_peer_gather_candidates(peer);
}

static void sub_sig_ice_candidate(p2p_signaling_client_t *c, const char *from,
                                   const char *candidate, void *user_data)
{
    p2p_subscriber_t *sub = (p2p_subscriber_t *)user_data;
    p2p_peer_ctx_t *peer = p2p_engine_find_peer(&sub->engine, from);
    if (peer)
        p2p_peer_add_remote_candidate(peer, candidate);
}

static void sub_sig_gathering_done(p2p_signaling_client_t *c, const char *from, void *user_data)
{
    p2p_subscriber_t *sub = (p2p_subscriber_t *)user_data;
    p2p_peer_ctx_t *peer = p2p_engine_find_peer(&sub->engine, from);
    if (peer)
        p2p_peer_set_remote_gathering_done(peer);
}

static void sub_sig_turn_creds(p2p_signaling_client_t *c,
                                const char *username, const char *password,
                                const char *server, uint16_t port,
                                uint32_t ttl, void *user_data)
{
    p2p_subscriber_t *sub = (p2p_subscriber_t *)user_data;
    fprintf(stderr, "[sub] received TURN credentials: %s@%s:%u ttl=%u\n",
            username, server, port, ttl);
    /* Update TURN config in engine for subsequent peers */
    snprintf(sub->engine.turn_host, sizeof(sub->engine.turn_host), "%s", server);
    sub->engine.turn_port = port;
    snprintf(sub->engine.turn_username, sizeof(sub->engine.turn_username), "%s", username);
    snprintf(sub->engine.turn_password, sizeof(sub->engine.turn_password), "%s", password);
}

/* ---- public API ---- */

int p2p_subscriber_init(p2p_subscriber_t *sub, const p2p_subscriber_config_t *config)
{
    if (!sub || !config) return -1;
    memset(sub, 0, sizeof(*sub));

    snprintf(sub->room_id, sizeof(sub->room_id), "%s",
             config->room_id ? config->room_id : "default");
    sub->on_video = config->on_video;
    sub->on_audio = config->on_audio;
    sub->app_user_data = config->user_data;

    p2p_engine_config_t ecfg;
    memset(&ecfg, 0, sizeof(ecfg));
    ecfg.role = P2P_ROLE_SUBSCRIBER;
    ecfg.stun_server_host = config->stun_host;
    ecfg.stun_server_port = config->stun_port;
    ecfg.turn_server_host = config->turn_host;
    ecfg.turn_server_port = config->turn_port;
    ecfg.turn_username = config->turn_username;
    ecfg.turn_password = config->turn_password;
    ecfg.callbacks.on_peer_ice_state = sub_on_ice_state;
    ecfg.callbacks.on_peer_ice_candidate = sub_on_ice_candidate;
    ecfg.callbacks.on_peer_ice_gathering_done = sub_on_ice_gathering_done;
    ecfg.callbacks.on_peer_quic_connected = sub_on_quic_connected;
    ecfg.callbacks.on_peer_quic_closed = sub_on_quic_closed;
    ecfg.user_data = sub;

    if (p2p_engine_init(&sub->engine, &ecfg) != 0)
        return -1;

    return 0;
}

int p2p_subscriber_start(p2p_subscriber_t *sub)
{
    if (!sub) return -1;

    if (p2p_engine_start(&sub->engine) != 0) return -1;
    sub->running = 1;

    /* Signaling connect is deferred until the application sets the correct URL */

    return 0;
}

void p2p_subscriber_stop(p2p_subscriber_t *sub)
{
    if (!sub) return;
    sub->running = 0;
    p2p_signaling_disconnect(&sub->signaling);
    p2p_engine_stop(&sub->engine);
}

void p2p_subscriber_destroy(p2p_subscriber_t *sub)
{
    if (!sub) return;
    p2p_engine_destroy(&sub->engine);
}
