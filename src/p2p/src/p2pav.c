/*
 * p2pav.c -- libp2pav implementation: pure transport layer wrapping
 *            libjuice (ICE), xquic (QUIC), and signaling (HTTPS/SSE).
 */

#include "p2pav.h"
#include "p2p_adapter.h"
#include "p2p_signaling.h"
#include "p2p_platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ================================================================
 *  Logging infrastructure
 * ================================================================ */

static p2pav_log_level_t  g_log_level = P2PAV_LOG_WARN;
static p2pav_log_cb_t     g_log_cb    = NULL;
static void              *g_log_ud    = NULL;

#define LOG(lvl, tag, ...) do { \
    if ((lvl) <= g_log_level) { \
        char _logbuf[512]; \
        snprintf(_logbuf, sizeof(_logbuf), __VA_ARGS__); \
        if (g_log_cb) g_log_cb((lvl), (tag), _logbuf, g_log_ud); \
        else fprintf(stderr, "[p2pav][%s] %s\n", (tag), _logbuf); \
    } \
} while (0)

#define LOG_ERR(...)   LOG(P2PAV_LOG_ERROR, "ERR", __VA_ARGS__)
#define LOG_WARN(...)  LOG(P2PAV_LOG_WARN,  "WRN", __VA_ARGS__)
#define LOG_INFO(...)  LOG(P2PAV_LOG_INFO,  "INF", __VA_ARGS__)
#define LOG_DBG(...)   LOG(P2PAV_LOG_DEBUG, "DBG", __VA_ARGS__)

/* ================================================================
 *  Reassembly buffer (migrated from p2p_peer.c)
 * ================================================================ */

#define REASM_VIDEO_MAX_SIZE  (512 * 1024)
#define REASM_AUDIO_MAX_SIZE  (8 * 1024)
#define REASM_DATA_MAX_SIZE   (64 * 1024)

typedef struct {
    uint8_t   type;
    uint8_t   flags;
    uint32_t  seq;
    uint64_t  timestamp_us;
    uint32_t  total_len;
    uint32_t  received;
    uint32_t  max_size;
    int       active;
    uint8_t  *data;
} reasm_buf_t;

static int reasm_buf_init(reasm_buf_t *rb, uint32_t max_size)
{
    memset(rb, 0, sizeof(*rb));
    rb->max_size = max_size;
    rb->data = (uint8_t *)malloc(max_size);
    return rb->data ? 0 : -1;
}

static void reasm_buf_free(reasm_buf_t *rb)
{
    free(rb->data);
    rb->data = NULL;
    rb->max_size = 0;
}

/* ================================================================
 *  Per-peer state inside p2pav
 * ================================================================ */

typedef struct {
    reasm_buf_t  video_reasm;
    reasm_buf_t  audio_reasm;
    reasm_buf_t  data_reasm;
    uint32_t     last_video_seq;
    int          has_last_video_seq;
    int          waiting_for_keyframe;
    uint64_t     last_idr_req_us;
} p2pav_peer_state_t;

/* ================================================================
 *  Data channel bookkeeping
 * ================================================================ */

#define P2PAV_MAX_DATA_CHANNELS 8

typedef struct {
    int                          active;
    p2pav_data_channel_id_t      id;
    int                          peer_index;
    p2pav_reliability_t          reliability;
    char                         label[64];
} p2pav_data_channel_entry_t;

/* ================================================================
 *  Internal session structure
 * ================================================================ */

struct p2pav_session_s {
    /* Config copies */
    p2pav_session_config_t   cfg;
    char                     signaling_url[256];
    char                     auth_token[1024];
    char                     room_id[64];
    char                     peer_id[64];
    char                     stun_host[256];
    char                     turn_server[256];
    char                     turn_username[128];
    char                     turn_password[128];
    char                     ssl_cert[512];
    char                     ssl_key[512];
    int                      enable_tcp;

    /* Core */
    p2p_engine_t             engine;
    p2p_signaling_client_t   sig_client;
    int                      engine_started;
    int                      sig_connected;

    /* Session callbacks */
    p2pav_session_callbacks_t  session_cbs;
    void                      *session_ud;

    /* Video */
    p2pav_video_config_t       video_cfg;
    p2pav_on_video_frame_t     video_recv_cb;
    void                      *video_recv_ud;
    p2pav_on_keyframe_request_t  keyframe_req_cb;
    void                      *keyframe_req_ud;
    volatile int               video_muted;
    uint32_t                   video_seq;
    uint8_t                   *idr_cache;
    int                        idr_cache_size;
    p2p_mutex_t                idr_mutex;
    p2p_mutex_t                send_mutex;

    /* Audio */
    p2pav_audio_config_t       audio_cfg;
    p2pav_on_audio_frame_t     audio_recv_cb;
    void                      *audio_recv_ud;
    volatile int               audio_muted;
    uint32_t                   audio_seq;

    /* Data channels */
    p2pav_data_channel_entry_t data_channels[P2PAV_MAX_DATA_CHANNELS];
    int                        next_channel_id;
    p2pav_on_data_t            data_recv_cb;
    void                      *data_recv_ud;
    p2pav_on_data_channel_t    data_ch_cb;
    void                      *data_ch_ud;
    uint32_t                   data_seq;

    /* Per-peer reassembly */
    p2pav_peer_state_t         peer_states[P2PAV_MAX_PEERS];
    p2p_mutex_t                reasm_mutex;

    /* Timing */
    p2pav_timing_t             timing;
    int                        first_video_sent;
    int                        first_video_recvd;
    int                        first_audio_recvd;

    /* Net stats callback */
    p2pav_on_net_stats_t       stats_cb;
    void                      *stats_ud;
    int                        stats_interval_ms;
    p2p_thread_t               stats_thread;
    int                        stats_running;

    /* State */
    volatile int               running;
};

/* ================================================================
 *  Forward declarations for internal callbacks
 * ================================================================ */

static void internal_on_ice_state(p2p_peer_ctx_t *peer, juice_state_t state, void *ud);
static void internal_on_ice_candidate(p2p_peer_ctx_t *peer, const char *sdp, void *ud);
static void internal_on_ice_gathering_done(p2p_peer_ctx_t *peer, void *ud);
static void internal_on_data_recv(p2p_peer_ctx_t *peer, const p2p_frame_header_t *hdr,
                                   const uint8_t *payload, void *ud);

static void internal_on_sig_connected(p2p_signaling_client_t *c, void *ud);
static void internal_on_sig_disconnected(p2p_signaling_client_t *c, void *ud);
static void internal_on_sig_room_created(p2p_signaling_client_t *c, const char *rid, void *ud);
static void internal_on_sig_peer_joined(p2p_signaling_client_t *c, const char *pid, void *ud);
static void internal_on_sig_peer_left(p2p_signaling_client_t *c, const char *pid, void *ud);
static void internal_on_sig_ice_offer(p2p_signaling_client_t *c, const char *from, const char *sdp, void *ud);
static void internal_on_sig_ice_answer(p2p_signaling_client_t *c, const char *from, const char *sdp, void *ud);
static void internal_on_sig_ice_candidate(p2p_signaling_client_t *c, const char *from, const char *cand, void *ud);
static void internal_on_sig_gathering_done(p2p_signaling_client_t *c, const char *from, void *ud);
static void internal_on_sig_turn(p2p_signaling_client_t *c, const char *user, const char *pass,
                                  const char *server, uint16_t port, uint32_t ttl, void *ud);

/* ================================================================
 *  Version / Error / Log
 * ================================================================ */

#define P2PAV_STR_HELPER(x) #x
#define P2PAV_STR(x) P2PAV_STR_HELPER(x)

const char *p2pav_version_string(void)
{
    return P2PAV_STR(P2PAV_VERSION_MAJOR) "."
           P2PAV_STR(P2PAV_VERSION_MINOR) "."
           P2PAV_STR(P2PAV_VERSION_PATCH);
}

int p2pav_version_number(void)
{
    return (P2PAV_VERSION_MAJOR << 16) | (P2PAV_VERSION_MINOR << 8) | P2PAV_VERSION_PATCH;
}

const char *p2pav_error_string(p2pav_error_t err)
{
    switch (err) {
    case P2PAV_OK:                return "OK";
    case P2PAV_ERR_INVALID_PARAM: return "Invalid parameter";
    case P2PAV_ERR_NOT_CONNECTED: return "Not connected";
    case P2PAV_ERR_ROOM_FULL:     return "Room full";
    case P2PAV_ERR_AUTH_FAILED:   return "Authentication failed";
    case P2PAV_ERR_ICE_FAILED:    return "ICE failed";
    case P2PAV_ERR_QUIC_FAILED:   return "QUIC failed";
    case P2PAV_ERR_CODEC_FAILED:  return "Codec failed";
    case P2PAV_ERR_TIMEOUT:       return "Timeout";
    case P2PAV_ERR_PEER_NOT_FOUND:return "Peer not found";
    case P2PAV_ERR_ALLOC_FAILED:  return "Allocation failed";
    case P2PAV_ERR_CHANNEL_FULL:  return "Channel full";
    case P2PAV_ERR_INTERNAL:      return "Internal error";
    default:                      return "Unknown error";
    }
}

void p2pav_set_log_level(p2pav_log_level_t level)
{
    g_log_level = level;

    juice_log_level_t jl = JUICE_LOG_LEVEL_WARN;
    switch (level) {
    case P2PAV_LOG_NONE:  jl = JUICE_LOG_LEVEL_NONE;    break;
    case P2PAV_LOG_ERROR: jl = JUICE_LOG_LEVEL_ERROR;   break;
    case P2PAV_LOG_WARN:  jl = JUICE_LOG_LEVEL_WARN;    break;
    case P2PAV_LOG_INFO:  jl = JUICE_LOG_LEVEL_INFO;    break;
    case P2PAV_LOG_DEBUG: jl = JUICE_LOG_LEVEL_DEBUG;   break;
    case P2PAV_LOG_TRACE: jl = JUICE_LOG_LEVEL_VERBOSE; break;
    }
    juice_set_log_level(jl);
}

void p2pav_set_log_callback(p2pav_log_cb_t cb, void *user_data)
{
    g_log_cb = cb;
    g_log_ud = user_data;
}

/* ================================================================
 *  Session create / destroy
 * ================================================================ */

p2pav_session_t *p2pav_session_create(const p2pav_session_config_t *config)
{
    if (!config || !config->signaling_url || !config->room_id || !config->peer_id)
        return NULL;

    p2pav_session_t *s = calloc(1, sizeof(*s));
    if (!s) return NULL;

    s->cfg = *config;

    snprintf(s->signaling_url, sizeof(s->signaling_url), "%s",
             config->signaling_url);
    if (config->auth_token)
        snprintf(s->auth_token, sizeof(s->auth_token), "%s", config->auth_token);
    snprintf(s->room_id, sizeof(s->room_id), "%s", config->room_id);
    snprintf(s->peer_id, sizeof(s->peer_id), "%s", config->peer_id);

    if (config->stun_server)
        snprintf(s->stun_host, sizeof(s->stun_host), "%s", config->stun_server);
    if (config->turn_server)
        snprintf(s->turn_server, sizeof(s->turn_server), "%s", config->turn_server);
    if (config->turn_username)
        snprintf(s->turn_username, sizeof(s->turn_username), "%s", config->turn_username);
    if (config->turn_password)
        snprintf(s->turn_password, sizeof(s->turn_password), "%s", config->turn_password);
    if (config->ssl_cert_file)
        snprintf(s->ssl_cert, sizeof(s->ssl_cert), "%s", config->ssl_cert_file);
    if (config->ssl_key_file)
        snprintf(s->ssl_key, sizeof(s->ssl_key), "%s", config->ssl_key_file);

    s->enable_tcp = config->enable_tcp;

    p2p_mutex_init(&s->idr_mutex);
    p2p_mutex_init(&s->send_mutex);
    p2p_mutex_init(&s->reasm_mutex);

    for (int i = 0; i < P2PAV_MAX_PEERS; i++) {
        p2pav_peer_state_t *ps = &s->peer_states[i];
        if (reasm_buf_init(&ps->video_reasm, REASM_VIDEO_MAX_SIZE) != 0 ||
            reasm_buf_init(&ps->audio_reasm, REASM_AUDIO_MAX_SIZE) != 0 ||
            reasm_buf_init(&ps->data_reasm,  REASM_DATA_MAX_SIZE)  != 0) {
            for (int j = 0; j <= i; j++) {
                reasm_buf_free(&s->peer_states[j].video_reasm);
                reasm_buf_free(&s->peer_states[j].audio_reasm);
                reasm_buf_free(&s->peer_states[j].data_reasm);
            }
            p2p_mutex_destroy(&s->idr_mutex);
            p2p_mutex_destroy(&s->send_mutex);
            p2p_mutex_destroy(&s->reasm_mutex);
            free(s);
            return NULL;
        }
    }

    s->next_channel_id = 1;

    LOG_INFO("session created: role=%s room=%s peer=%s",
             config->role == P2PAV_ROLE_PUBLISHER ? "publisher" : "subscriber",
             s->room_id, s->peer_id);

    return s;
}

void p2pav_session_destroy(p2pav_session_t *s)
{
    if (!s) return;

    if (s->running)
        p2pav_session_stop(s);

    p2p_mutex_lock(&s->idr_mutex);
    free(s->idr_cache);
    s->idr_cache = NULL;
    p2p_mutex_unlock(&s->idr_mutex);

    for (int i = 0; i < P2PAV_MAX_PEERS; i++) {
        reasm_buf_free(&s->peer_states[i].video_reasm);
        reasm_buf_free(&s->peer_states[i].audio_reasm);
        reasm_buf_free(&s->peer_states[i].data_reasm);
    }

    p2p_mutex_destroy(&s->idr_mutex);
    p2p_mutex_destroy(&s->send_mutex);
    p2p_mutex_destroy(&s->reasm_mutex);

    LOG_INFO("session destroyed");
    free(s);
}

/* ================================================================
 *  Session start / stop
 * ================================================================ */

p2pav_error_t p2pav_session_start(p2pav_session_t *s)
{
    if (!s) return P2PAV_ERR_INVALID_PARAM;
    if (s->running) return P2PAV_OK;

    s->timing.session_start_us = p2p_now_us();

    /* Parse STUN host:port */
    char stun_host_only[256] = "";
    uint16_t stun_port = 3478;
    if (s->stun_host[0]) {
        snprintf(stun_host_only, sizeof(stun_host_only), "%s", s->stun_host);
        char *colon = strrchr(stun_host_only, ':');
        if (colon) {
            *colon = '\0';
            stun_port = (uint16_t)atoi(colon + 1);
        }
    }

    /* Parse TURN host:port */
    char turn_host_only[256] = "";
    uint16_t turn_port = 3478;
    if (s->turn_server[0]) {
        snprintf(turn_host_only, sizeof(turn_host_only), "%s", s->turn_server);
        char *colon = strrchr(turn_host_only, ':');
        if (colon) {
            *colon = '\0';
            turn_port = (uint16_t)atoi(colon + 1);
        }
    }

    p2p_role_t adapter_role = (s->cfg.role == P2PAV_ROLE_PUBLISHER)
                            ? P2P_ROLE_PUBLISHER : P2P_ROLE_SUBSCRIBER;

    p2p_engine_config_t ecfg = {
        .stun_server_host = stun_host_only[0] ? stun_host_only : NULL,
        .stun_server_port = stun_port,
        .turn_server_host = turn_host_only[0] ? turn_host_only : NULL,
        .turn_server_port = turn_port,
        .turn_username    = s->turn_username[0] ? s->turn_username : NULL,
        .turn_password    = s->turn_password[0] ? s->turn_password : NULL,
        .ssl_cert_file    = s->ssl_cert[0] ? s->ssl_cert : NULL,
        .ssl_key_file     = s->ssl_key[0]  ? s->ssl_key  : NULL,
        .enable_tcp       = s->enable_tcp,
        .role = adapter_role,
        .callbacks = {
            .on_peer_ice_state          = internal_on_ice_state,
            .on_peer_ice_candidate      = internal_on_ice_candidate,
            .on_peer_ice_gathering_done = internal_on_ice_gathering_done,
            .on_peer_data_recv          = internal_on_data_recv,
        },
        .user_data = s,
    };

    if (p2p_engine_init(&s->engine, &ecfg) != 0) {
        LOG_ERR("engine init failed");
        return P2PAV_ERR_INTERNAL;
    }
    if (p2p_engine_start(&s->engine) != 0) {
        LOG_ERR("engine start failed");
        p2p_engine_destroy(&s->engine);
        return P2PAV_ERR_INTERNAL;
    }
    s->engine_started = 1;

    /* Connect signaling */
    p2p_signaling_config_t scfg = {
        .server_url = s->signaling_url,
        .peer_id    = s->peer_id,
        .token      = s->auth_token[0] ? s->auth_token : NULL,
        .callbacks  = {
            .on_connected      = internal_on_sig_connected,
            .on_disconnected   = internal_on_sig_disconnected,
            .on_room_created   = internal_on_sig_room_created,
            .on_peer_joined    = internal_on_sig_peer_joined,
            .on_peer_left      = internal_on_sig_peer_left,
            .on_ice_offer      = internal_on_sig_ice_offer,
            .on_ice_answer     = internal_on_sig_ice_answer,
            .on_ice_candidate  = internal_on_sig_ice_candidate,
            .on_gathering_done = internal_on_sig_gathering_done,
            .on_turn_credentials = internal_on_sig_turn,
        },
        .user_data = s,
    };

    if (p2p_signaling_connect(&s->sig_client, &scfg) != 0) {
        LOG_ERR("signaling connect failed");
        p2p_engine_stop(&s->engine);
        p2p_engine_destroy(&s->engine);
        s->engine_started = 0;
        return P2PAV_ERR_NOT_CONNECTED;
    }

    s->running = 1;
    LOG_INFO("session started");
    return P2PAV_OK;
}

void p2pav_session_stop(p2pav_session_t *s)
{
    if (!s || !s->running) return;
    s->running = 0;

    if (s->stats_running) {
        s->stats_running = 0;
        p2p_thread_join(s->stats_thread);
    }

    p2p_signaling_disconnect(&s->sig_client);
    s->sig_connected = 0;

    if (s->engine_started) {
        p2p_engine_stop(&s->engine);
        p2p_engine_destroy(&s->engine);
        s->engine_started = 0;
    }

    LOG_INFO("session stopped");
}

void p2pav_session_set_callbacks(p2pav_session_t *s,
                                  const p2pav_session_callbacks_t *cb,
                                  void *user_data)
{
    if (!s || !cb) return;
    s->session_cbs = *cb;
    s->session_ud  = user_data;
}

p2pav_error_t p2pav_session_kick_peer(p2pav_session_t *s, const char *peer_id)
{
    if (!s || !peer_id) return P2PAV_ERR_INVALID_PARAM;
    p2p_peer_ctx_t *peer = p2p_engine_find_peer(&s->engine, peer_id);
    if (!peer) return P2PAV_ERR_PEER_NOT_FOUND;
    p2p_engine_remove_peer(&s->engine, peer->index);
    return P2PAV_OK;
}

int p2pav_session_get_peer_count(p2pav_session_t *s)
{
    return s ? s->engine.peer_count : 0;
}

/* ================================================================
 *  Video API
 * ================================================================ */

p2pav_error_t p2pav_video_set_config(p2pav_session_t *s,
                                      const p2pav_video_config_t *config)
{
    if (!s || !config) return P2PAV_ERR_INVALID_PARAM;
    s->video_cfg = *config;
    return P2PAV_OK;
}

static void send_to_all_peers_internal(p2pav_session_t *s,
                                        uint8_t type, uint8_t flags,
                                        uint32_t seq, uint64_t ts,
                                        const uint8_t *data, uint32_t len)
{
    for (int i = 0; i < P2PAV_MAX_PEERS; i++) {
        p2p_peer_ctx_t *peer = &s->engine.peers[i];
        if (peer->state >= P2P_PEER_STATE_ICE_CONNECTED && peer->ice_agent)
            p2p_peer_send_data(peer, type, flags, seq, ts, data, len);
    }
}

p2pav_error_t p2pav_video_send(p2pav_session_t *s,
                                const p2pav_video_frame_t *frame)
{
    if (!s || !frame || !frame->data) return P2PAV_ERR_INVALID_PARAM;
    if (!s->running) return P2PAV_ERR_NOT_CONNECTED;
    if (s->video_muted) return P2PAV_OK;

    uint8_t flags = frame->is_keyframe ? P2P_FRAME_FLAG_KEY : 0;

    /* Cache IDR for fast first-frame delivery to new subscribers */
    if (frame->is_keyframe) {
        p2p_mutex_lock(&s->idr_mutex);
        uint8_t *tmp = (uint8_t *)realloc(s->idr_cache, frame->size);
        if (tmp) {
            s->idr_cache = tmp;
            memcpy(s->idr_cache, frame->data, frame->size);
            s->idr_cache_size = frame->size;
        } else {
            LOG_ERR("IDR cache realloc failed (%d bytes)", (int)frame->size);
        }
        p2p_mutex_unlock(&s->idr_mutex);
    }

    if (!s->first_video_sent) {
        s->timing.first_video_sent_us = p2p_now_us();
        s->first_video_sent = 1;
    }

    p2p_mutex_lock(&s->send_mutex);
    uint32_t seq = s->video_seq++;
    send_to_all_peers_internal(s, P2P_FRAME_TYPE_VIDEO, flags,
                                seq, frame->timestamp_us,
                                frame->data, (uint32_t)frame->size);
    p2p_mutex_unlock(&s->send_mutex);
    return P2PAV_OK;
}

void p2pav_video_mute(p2pav_session_t *s, int mute)
{
    if (s) s->video_muted = mute;
}

void p2pav_video_set_recv_callback(p2pav_session_t *s,
                                    p2pav_on_video_frame_t cb,
                                    void *user_data)
{
    if (!s) return;
    s->video_recv_cb = cb;
    s->video_recv_ud = user_data;
}

p2pav_error_t p2pav_video_request_keyframe(p2pav_session_t *s,
                                            const char *from_peer)
{
    if (!s) return P2PAV_ERR_INVALID_PARAM;
    if (from_peer) {
        p2p_peer_ctx_t *peer = p2p_engine_find_peer(&s->engine, from_peer);
        if (!peer) return P2PAV_ERR_PEER_NOT_FOUND;
        p2p_peer_send_idr_request(peer);
    } else {
        for (int i = 0; i < P2PAV_MAX_PEERS; i++) {
            p2p_peer_ctx_t *p = &s->engine.peers[i];
            if (p->state >= P2P_PEER_STATE_ICE_CONNECTED && p->ice_agent)
                p2p_peer_send_idr_request(p);
        }
    }
    return P2PAV_OK;
}

void p2pav_video_set_keyframe_request_callback(p2pav_session_t *s,
                                                p2pav_on_keyframe_request_t cb,
                                                void *user_data)
{
    if (!s) return;
    s->keyframe_req_cb = cb;
    s->keyframe_req_ud = user_data;
}

p2pav_error_t p2pav_video_set_bitrate(p2pav_session_t *s,
                                       p2pav_bitrate_mode_t mode,
                                       int bitrate_bps)
{
    if (!s) return P2PAV_ERR_INVALID_PARAM;
    s->video_cfg.bitrate_mode = mode;
    s->video_cfg.bitrate_bps  = bitrate_bps;
    return P2PAV_OK;
}

/* ================================================================
 *  Audio API
 * ================================================================ */

p2pav_error_t p2pav_audio_set_config(p2pav_session_t *s,
                                      const p2pav_audio_config_t *config)
{
    if (!s || !config) return P2PAV_ERR_INVALID_PARAM;
    s->audio_cfg = *config;
    return P2PAV_OK;
}

p2pav_error_t p2pav_audio_send(p2pav_session_t *s,
                                const p2pav_audio_frame_t *frame)
{
    if (!s || !frame || !frame->data) return P2PAV_ERR_INVALID_PARAM;
    if (!s->running) return P2PAV_ERR_NOT_CONNECTED;
    if (s->audio_muted) return P2PAV_OK;

    p2p_mutex_lock(&s->send_mutex);
    uint32_t seq = s->audio_seq++;
    send_to_all_peers_internal(s, P2P_FRAME_TYPE_AUDIO, 0,
                                seq, frame->timestamp_us,
                                frame->data, (uint32_t)frame->size);
    p2p_mutex_unlock(&s->send_mutex);
    return P2PAV_OK;
}

void p2pav_audio_mute(p2pav_session_t *s, int mute)
{
    if (s) s->audio_muted = mute;
}

void p2pav_audio_set_recv_callback(p2pav_session_t *s,
                                    p2pav_on_audio_frame_t cb,
                                    void *user_data)
{
    if (!s) return;
    s->audio_recv_cb = cb;
    s->audio_recv_ud = user_data;
}

/* ================================================================
 *  Data channel API
 * ================================================================ */

p2pav_data_channel_id_t p2pav_data_open(p2pav_session_t *s,
                                         const char *peer_id,
                                         const p2pav_data_channel_config_t *config)
{
    if (!s || !peer_id || !config) return -1;

    p2p_peer_ctx_t *peer = p2p_engine_find_peer(&s->engine, peer_id);
    if (!peer) return -1;

    for (int i = 0; i < P2PAV_MAX_DATA_CHANNELS; i++) {
        if (!s->data_channels[i].active) {
            p2pav_data_channel_entry_t *ch = &s->data_channels[i];
            ch->active      = 1;
            ch->id          = s->next_channel_id++;
            ch->peer_index  = peer->index;
            ch->reliability = config->reliability;
            if (config->label)
                snprintf(ch->label, sizeof(ch->label), "%s", config->label);

            if (s->data_ch_cb)
                s->data_ch_cb(s, peer_id, ch->id, 1, s->data_ch_ud);

            LOG_DBG("data channel %d opened to %s", ch->id, peer_id);
            return ch->id;
        }
    }
    return -1;
}

void p2pav_data_close(p2pav_session_t *s, p2pav_data_channel_id_t ch_id)
{
    if (!s) return;
    for (int i = 0; i < P2PAV_MAX_DATA_CHANNELS; i++) {
        if (s->data_channels[i].active && s->data_channels[i].id == ch_id) {
            s->data_channels[i].active = 0;
            LOG_DBG("data channel %d closed", ch_id);
            return;
        }
    }
}

p2pav_error_t p2pav_data_send(p2pav_session_t *s,
                               p2pav_data_channel_id_t ch_id,
                               const void *data, size_t len)
{
    if (!s || !data || len == 0) return P2PAV_ERR_INVALID_PARAM;
    if (!s->running) return P2PAV_ERR_NOT_CONNECTED;

    p2pav_data_channel_entry_t *ch = NULL;
    for (int i = 0; i < P2PAV_MAX_DATA_CHANNELS; i++) {
        if (s->data_channels[i].active && s->data_channels[i].id == ch_id) {
            ch = &s->data_channels[i];
            break;
        }
    }
    if (!ch) return P2PAV_ERR_INVALID_PARAM;

    if (ch->peer_index < 0 || ch->peer_index >= P2PAV_MAX_PEERS)
        return P2PAV_ERR_PEER_NOT_FOUND;

    p2p_peer_ctx_t *peer = &s->engine.peers[ch->peer_index];
    if (peer->state < P2P_PEER_STATE_ICE_CONNECTED || !peer->ice_agent)
        return P2PAV_ERR_NOT_CONNECTED;

    p2p_mutex_lock(&s->send_mutex);
    uint32_t seq = s->data_seq++;
    int ret = p2p_peer_send_data(peer, P2P_FRAME_TYPE_DATA, 0,
                                  seq, p2p_now_us(),
                                  (const uint8_t *)data, (uint32_t)len);
    p2p_mutex_unlock(&s->send_mutex);
    return (ret == 0) ? P2PAV_OK : P2PAV_ERR_INTERNAL;
}

void p2pav_data_set_recv_callback(p2pav_session_t *s,
                                   p2pav_on_data_t cb,
                                   void *user_data)
{
    if (!s) return;
    s->data_recv_cb = cb;
    s->data_recv_ud = user_data;
}

void p2pav_data_set_channel_callback(p2pav_session_t *s,
                                      p2pav_on_data_channel_t cb,
                                      void *user_data)
{
    if (!s) return;
    s->data_ch_cb = cb;
    s->data_ch_ud = user_data;
}

/* ================================================================
 *  Diagnostics API
 * ================================================================ */

p2pav_error_t p2pav_get_net_stats(p2pav_session_t *s,
                                   const char *peer_id,
                                   p2pav_net_stats_t *stats)
{
    if (!s || !stats) return P2PAV_ERR_INVALID_PARAM;
    memset(stats, 0, sizeof(*stats));

    p2p_peer_ctx_t *peer = peer_id ? p2p_engine_find_peer(&s->engine, peer_id) : NULL;
    if (peer_id && !peer) return P2PAV_ERR_PEER_NOT_FOUND;

    /* If no specific peer, pick the first connected one */
    if (!peer) {
        for (int i = 0; i < P2PAV_MAX_PEERS; i++) {
            if (s->engine.peers[i].state >= P2P_PEER_STATE_ICE_CONNECTED &&
                s->engine.peers[i].ice_agent) {
                peer = &s->engine.peers[i];
                break;
            }
        }
    }
    if (!peer) return P2PAV_ERR_NOT_CONNECTED;

    /* ICE candidate info for path type detection */
    char local_cand[256] = "", remote_cand[256] = "";
    if (peer->ice_agent) {
        juice_get_selected_candidates(peer->ice_agent,
                                       local_cand, sizeof(local_cand),
                                       remote_cand, sizeof(remote_cand));
        juice_get_selected_addresses(peer->ice_agent,
                                      stats->local_addr, sizeof(stats->local_addr),
                                      stats->remote_addr, sizeof(stats->remote_addr));
    }

    /* Determine path type from candidate type string */
    stats->path_type = P2PAV_PATH_UNKNOWN;
    if (local_cand[0]) {
        /* libjuice candidate format: "a=candidate:... typ host/srflx/prflx/relay ..." */
        const char *typ = strstr(local_cand, "typ ");
        if (typ) {
            typ += 4;
            if (strncmp(typ, "relay", 5) == 0) {
                stats->path_type = P2PAV_PATH_RELAY;
                snprintf(stats->ice_type, sizeof(stats->ice_type), "relay");
            } else {
                stats->path_type = P2PAV_PATH_DIRECT;
                if (strncmp(typ, "host", 4) == 0)
                    snprintf(stats->ice_type, sizeof(stats->ice_type), "host");
                else if (strncmp(typ, "srflx", 5) == 0)
                    snprintf(stats->ice_type, sizeof(stats->ice_type), "srflx");
                else if (strncmp(typ, "prflx", 5) == 0)
                    snprintf(stats->ice_type, sizeof(stats->ice_type), "prflx");
            }
        }
    }

    /* xquic connection stats */
    if (s->engine.xqc_engine && peer->xqc_conn) {
        xqc_conn_stats_t xstats = xqc_conn_get_stats(s->engine.xqc_engine, &peer->cid);
        stats->rtt_us              = (uint32_t)xstats.srtt;
        stats->min_rtt_us          = (uint32_t)xstats.min_rtt;
        stats->total_packets_sent  = xstats.send_count;
        stats->total_packets_recv  = xstats.recv_count;
        stats->total_packets_lost  = xstats.lost_count;
        stats->bytes_in_flight     = (uint32_t)xstats.inflight_bytes;
        if (xstats.send_count > 0)
            stats->packet_loss_rate = (float)xstats.lost_count / (float)xstats.send_count;
    }

    return P2PAV_OK;
}

p2pav_error_t p2pav_get_timing(p2pav_session_t *s, p2pav_timing_t *timing)
{
    if (!s || !timing) return P2PAV_ERR_INVALID_PARAM;
    *timing = s->timing;
    return P2PAV_OK;
}

p2pav_error_t p2pav_get_ice_candidates(p2pav_session_t *s,
                                        const char *peer_id,
                                        p2pav_ice_candidates_t *local,
                                        p2pav_ice_candidates_t *remote)
{
    if (!s) return P2PAV_ERR_INVALID_PARAM;

    p2p_peer_ctx_t *peer = peer_id ? p2p_engine_find_peer(&s->engine, peer_id) : NULL;
    if (peer_id && !peer) return P2PAV_ERR_PEER_NOT_FOUND;

    if (!peer) {
        for (int i = 0; i < P2PAV_MAX_PEERS; i++) {
            if (s->engine.peers[i].ice_agent) {
                peer = &s->engine.peers[i];
                break;
            }
        }
    }
    if (!peer || !peer->ice_agent) return P2PAV_ERR_NOT_CONNECTED;

    char local_sel[256] = "", remote_sel[256] = "";
    juice_get_selected_candidates(peer->ice_agent,
                                   local_sel, sizeof(local_sel),
                                   remote_sel, sizeof(remote_sel));

    if (local) {
        memset(local, 0, sizeof(*local));
        if (local_sel[0]) {
            local->count = 1;
            const char *typ = strstr(local_sel, "typ ");
            if (typ) {
                typ += 4;
                char t[16] = "";
                sscanf(typ, "%15s", t);
                snprintf(local->candidates[0].type, sizeof(local->candidates[0].type), "%s", t);
            }
            char addr[64] = "";
            juice_get_selected_addresses(peer->ice_agent, addr, sizeof(addr), NULL, 0);
            char *colon = strrchr(addr, ':');
            if (colon) {
                *colon = '\0';
                local->candidates[0].port = atoi(colon + 1);
            }
            snprintf(local->candidates[0].address, sizeof(local->candidates[0].address), "%s", addr);
        }
    }

    if (remote) {
        memset(remote, 0, sizeof(*remote));
        if (remote_sel[0]) {
            remote->count = 1;
            const char *typ = strstr(remote_sel, "typ ");
            if (typ) {
                typ += 4;
                char t[16] = "";
                sscanf(typ, "%15s", t);
                snprintf(remote->candidates[0].type, sizeof(remote->candidates[0].type), "%s", t);
            }
            char addr[64] = "";
            juice_get_selected_addresses(peer->ice_agent, NULL, 0, addr, sizeof(addr));
            char *colon = strrchr(addr, ':');
            if (colon) {
                *colon = '\0';
                remote->candidates[0].port = atoi(colon + 1);
            }
            snprintf(remote->candidates[0].address, sizeof(remote->candidates[0].address), "%s", addr);
        }
    }

    return P2PAV_OK;
}

/* Net stats periodic callback thread */
static void *stats_thread_func(void *arg)
{
    p2pav_session_t *s = (p2pav_session_t *)arg;
    while (s->stats_running && s->running) {
        p2p_sleep_ms(s->stats_interval_ms);
        if (!s->stats_running || !s->running) break;

        for (int i = 0; i < P2PAV_MAX_PEERS; i++) {
            p2p_peer_ctx_t *peer = &s->engine.peers[i];
            if (peer->state >= P2P_PEER_STATE_ICE_CONNECTED && peer->ice_agent) {
                p2pav_net_stats_t stats;
                if (p2pav_get_net_stats(s, peer->peer_id, &stats) == P2PAV_OK)
                    s->stats_cb(s, peer->peer_id, &stats, s->stats_ud);
            }
        }
    }
    return NULL;
}

void p2pav_set_net_stats_callback(p2pav_session_t *s,
                                   p2pav_on_net_stats_t cb,
                                   int interval_ms,
                                   void *user_data)
{
    if (!s) return;

    if (s->stats_running) {
        s->stats_running = 0;
        p2p_thread_join(s->stats_thread);
    }

    s->stats_cb = cb;
    s->stats_ud = user_data;
    s->stats_interval_ms = interval_ms > 0 ? interval_ms : 1000;

    if (cb && s->running) {
        s->stats_running = 1;
        p2p_thread_create(&s->stats_thread, stats_thread_func, s);
    }
}

/* ================================================================
 *  Internal: data receive handler (reassembly + dispatch)
 * ================================================================ */

static void internal_on_data_recv(p2p_peer_ctx_t *peer, const p2p_frame_header_t *hdr,
                                   const uint8_t *payload, void *ud)
{
    p2pav_session_t *s = (p2pav_session_t *)ud;
    if (!s) return;

    /* IDR request from subscriber -> fire keyframe_req callback */
    if (hdr->type == P2P_FRAME_TYPE_IDR_REQ) {
        if (s->keyframe_req_cb)
            s->keyframe_req_cb(s, peer->peer_id, s->keyframe_req_ud);
        return;
    }

    int pidx = peer->index;
    if (pidx < 0 || pidx >= P2PAV_MAX_PEERS) return;

    p2pav_peer_state_t *ps = &s->peer_states[pidx];

    p2p_mutex_lock(&s->reasm_mutex);

    reasm_buf_t *rb = NULL;
    if (hdr->type == P2P_FRAME_TYPE_VIDEO)      rb = &ps->video_reasm;
    else if (hdr->type == P2P_FRAME_TYPE_AUDIO)  rb = &ps->audio_reasm;
    else if (hdr->type == P2P_FRAME_TYPE_DATA)   rb = &ps->data_reasm;
    else {
        p2p_mutex_unlock(&s->reasm_mutex);
        return;
    }

    /* Video-specific: sequence gap detection & IDR request */
    if (hdr->type == P2P_FRAME_TYPE_VIDEO) {
        if (ps->has_last_video_seq && hdr->frag_offset == 0) {
            uint32_t expected = ps->last_video_seq + 1;
            if (hdr->seq > expected) {
                LOG_WARN("LOST %u video frames (expected seq=%u got=%u)",
                         hdr->seq - expected, expected, hdr->seq);
                uint64_t now = p2p_now_us();
                if (now - ps->last_idr_req_us >= 200000) {
                    ps->last_idr_req_us = now;
                    ps->waiting_for_keyframe = 1;
                    p2p_peer_send_idr_request(peer);
                }
            }
        }

        if (rb->active && hdr->seq != rb->seq && rb->received < rb->total_len) {
            uint64_t now = p2p_now_us();
            if (now - ps->last_idr_req_us >= 200000) {
                ps->last_idr_req_us = now;
                ps->waiting_for_keyframe = 1;
                p2p_peer_send_idr_request(peer);
            }
        }

        if (hdr->frag_offset == 0) {
            ps->last_video_seq = hdr->seq;
            ps->has_last_video_seq = 1;
        }

        /* Drop non-keyframes when waiting for IDR */
        if (ps->waiting_for_keyframe) {
            if (hdr->frag_offset == 0 && (hdr->flags & P2P_FRAME_FLAG_KEY)) {
                ps->waiting_for_keyframe = 0;
            } else if (hdr->frag_offset == 0) {
                p2p_mutex_unlock(&s->reasm_mutex);
                return;
            }
        }
    }

    /* New frame or different sequence -> reset */
    if (!rb->active || rb->seq != hdr->seq || rb->type != hdr->type) {
        if (rb->active && hdr->type == rb->type && hdr->seq < rb->seq) {
            p2p_mutex_unlock(&s->reasm_mutex);
            return;
        }
        rb->type         = hdr->type;
        rb->flags        = hdr->flags;
        rb->seq          = hdr->seq;
        rb->timestamp_us = hdr->timestamp_us;
        rb->total_len    = hdr->total_len;
        rb->received     = 0;
        rb->active       = 1;
        if (rb->total_len > rb->max_size) {
            LOG_WARN("frame too large (%u > %u), dropping", rb->total_len, rb->max_size);
            rb->active = 0;
            p2p_mutex_unlock(&s->reasm_mutex);
            return;
        }
    }

    /* Bounds check */
    uint32_t frag_end = hdr->frag_offset + hdr->frag_len;
    if (hdr->frag_offset >= rb->max_size || frag_end > rb->max_size ||
        frag_end > rb->total_len) {
        p2p_mutex_unlock(&s->reasm_mutex);
        return;
    }

    memcpy(rb->data + hdr->frag_offset, payload, hdr->frag_len);
    rb->received += hdr->frag_len;
    if (hdr->frag_offset == 0)
        rb->flags = hdr->flags;

    /* Frame complete -> copy data, release lock, then dispatch to user callback */
    if (rb->received >= rb->total_len) {
        uint8_t frame_type = rb->type;
        uint8_t frame_flags = rb->flags;
        uint32_t total_len = rb->total_len;
        uint64_t ts = rb->timestamp_us;

        uint8_t *frame_copy = (uint8_t *)malloc(total_len);
        if (!frame_copy) {
            LOG_ERR("OOM copying completed frame (%u bytes)", total_len);
            rb->active = 0;
            rb->received = 0;
            p2p_mutex_unlock(&s->reasm_mutex);
            return;
        }
        memcpy(frame_copy, rb->data, total_len);

        /* Also capture the data channel id while still holding the lock */
        p2pav_data_channel_id_t ch_id = 0;
        if (frame_type == P2P_FRAME_TYPE_DATA) {
            for (int i = 0; i < P2PAV_MAX_DATA_CHANNELS; i++) {
                if (s->data_channels[i].active &&
                    s->data_channels[i].peer_index == pidx) {
                    ch_id = s->data_channels[i].id;
                    break;
                }
            }
        }

        rb->active   = 0;
        rb->received = 0;

        p2p_mutex_unlock(&s->reasm_mutex);

        /* Dispatch to user callbacks outside the lock */
        if (frame_type == P2P_FRAME_TYPE_VIDEO && s->video_recv_cb) {
            if (!s->first_video_recvd) {
                s->timing.first_video_recv_us = p2p_now_us();
                s->first_video_recvd = 1;
            }
            p2pav_video_frame_t vf = {
                .data         = frame_copy,
                .size         = (int)total_len,
                .timestamp_us = ts,
                .is_keyframe  = (frame_flags & P2P_FRAME_FLAG_KEY) ? 1 : 0,
            };
            s->video_recv_cb(s, peer->peer_id, &vf, s->video_recv_ud);
        } else if (frame_type == P2P_FRAME_TYPE_AUDIO && s->audio_recv_cb) {
            if (!s->first_audio_recvd) {
                s->timing.first_audio_recv_us = p2p_now_us();
                s->first_audio_recvd = 1;
            }
            p2pav_audio_frame_t af = {
                .data         = frame_copy,
                .size         = (int)total_len,
                .timestamp_us = ts,
            };
            s->audio_recv_cb(s, peer->peer_id, &af, s->audio_recv_ud);
        } else if (frame_type == P2P_FRAME_TYPE_DATA && s->data_recv_cb) {
            s->data_recv_cb(s, peer->peer_id, ch_id,
                            frame_copy, total_len, s->data_recv_ud);
        }

        free(frame_copy);
        return;
    }

    p2p_mutex_unlock(&s->reasm_mutex);
}

/* ================================================================
 *  Internal: ICE callbacks
 * ================================================================ */

static void internal_on_ice_state(p2p_peer_ctx_t *peer, juice_state_t state, void *ud)
{
    p2pav_session_t *s = (p2pav_session_t *)ud;
    if (!s) return;

    LOG_INFO("peer %s ICE state -> %d", peer->peer_id, state);

    if (state == JUICE_STATE_CONNECTED || state == JUICE_STATE_COMPLETED) {
        s->timing.ice_connected_us = p2p_now_us();

        /* Send cached IDR to newly connected subscriber */
        if (s->cfg.role == P2PAV_ROLE_PUBLISHER) {
            p2p_mutex_lock(&s->send_mutex);
            p2p_mutex_lock(&s->idr_mutex);
            if (s->idr_cache && s->idr_cache_size > 0) {
                uint32_t seq = s->video_seq++;
                p2p_peer_send_data(peer, P2P_FRAME_TYPE_VIDEO, P2P_FRAME_FLAG_KEY,
                                   seq, p2p_now_us(),
                                   s->idr_cache, s->idr_cache_size);
                LOG_DBG("sent cached IDR (%d bytes) to %s",
                        s->idr_cache_size, peer->peer_id);
            }
            p2p_mutex_unlock(&s->idr_mutex);
            p2p_mutex_unlock(&s->send_mutex);
        }

        if (s->session_cbs.on_peer_ready)
            s->session_cbs.on_peer_ready(s, peer->peer_id, s->session_ud);
    }

    if (state == JUICE_STATE_FAILED) {
        if (s->session_cbs.on_error)
            s->session_cbs.on_error(s, P2PAV_ERR_ICE_FAILED,
                                     peer->peer_id, s->session_ud);
    }
}

static void internal_on_ice_candidate(p2p_peer_ctx_t *peer, const char *sdp, void *ud)
{
    p2pav_session_t *s = (p2pav_session_t *)ud;
    if (!s) return;
    p2p_signaling_send_ice_candidate(&s->sig_client, peer->peer_id, sdp);
}

static void internal_on_ice_gathering_done(p2p_peer_ctx_t *peer, void *ud)
{
    p2pav_session_t *s = (p2pav_session_t *)ud;
    if (!s) return;
    s->timing.ice_gathering_done_us = p2p_now_us();

    char sdp[P2P_SIG_MAX_SDP_SIZE];

    if (peer->offer_pending) {
        peer->offer_pending = 0;
        if (p2p_peer_get_local_description(peer, sdp, sizeof(sdp)) == 0) {
            LOG_INFO("sending ICE offer to '%s' (post-gathering)", peer->peer_id);
            p2p_signaling_send_ice_offer(&s->sig_client, peer->peer_id, sdp);
        }
    }

    p2p_signaling_send_gathering_done(&s->sig_client, peer->peer_id);
}

/* ================================================================
 *  Internal: Signaling callbacks
 * ================================================================ */

static void internal_on_sig_connected(p2p_signaling_client_t *c, void *ud)
{
    p2pav_session_t *s = (p2pav_session_t *)ud;
    s->sig_connected = 1;
    s->timing.signaling_connected_us = p2p_now_us();
    LOG_INFO("signaling connected");

    /* Auto create or join room based on role */
    if (s->cfg.role == P2PAV_ROLE_PUBLISHER)
        p2p_signaling_create_room(&s->sig_client, s->room_id);
    else
        p2p_signaling_join_room(&s->sig_client, s->room_id);

    if (s->session_cbs.on_connected)
        s->session_cbs.on_connected(s, s->session_ud);
}

static void internal_on_sig_disconnected(p2p_signaling_client_t *c, void *ud)
{
    p2pav_session_t *s = (p2pav_session_t *)ud;
    s->sig_connected = 0;
    LOG_WARN("signaling disconnected");
    if (s->session_cbs.on_disconnected)
        s->session_cbs.on_disconnected(s, P2PAV_ERR_NOT_CONNECTED, s->session_ud);
}

static void internal_on_sig_room_created(p2p_signaling_client_t *c, const char *rid, void *ud)
{
    p2pav_session_t *s = (p2pav_session_t *)ud;
    s->timing.room_joined_us = p2p_now_us();
    LOG_INFO("room '%s' ready", rid);
}

static void internal_on_sig_peer_joined(p2p_signaling_client_t *c, const char *pid, void *ud)
{
    p2pav_session_t *s = (p2pav_session_t *)ud;
    LOG_INFO("peer '%s' joined", pid);

    if (s->session_cbs.on_peer_joined)
        s->session_cbs.on_peer_joined(s, pid, s->session_ud);

    /* Publisher: initiate ICE exchange with new subscriber.
     * Delay sending the offer until gathering is done so the SDP
     * contains all candidates (including TCP passive).  Trickle-ICE
     * candidate delivery via SSE is unreliable when the remote peer
     * has not yet created its agent. */
    if (s->cfg.role == P2PAV_ROLE_PUBLISHER) {
        p2p_peer_ctx_t *peer = p2p_engine_add_peer(&s->engine, pid);
        if (!peer) {
            LOG_ERR("cannot add peer (max %d)", P2PAV_MAX_PEERS);
            if (s->session_cbs.on_error)
                s->session_cbs.on_error(s, P2PAV_ERR_ROOM_FULL, pid, s->session_ud);
            return;
        }
        peer->offer_pending = 1;

        s->timing.ice_gathering_start_us = p2p_now_us();
        p2p_peer_gather_candidates(peer);
    }
}

static void internal_on_sig_peer_left(p2p_signaling_client_t *c, const char *pid, void *ud)
{
    p2pav_session_t *s = (p2pav_session_t *)ud;
    LOG_INFO("peer '%s' left", pid);

    p2p_peer_ctx_t *peer = p2p_engine_find_peer(&s->engine, pid);
    if (peer)
        p2p_engine_remove_peer(&s->engine, peer->index);

    if (s->session_cbs.on_peer_left)
        s->session_cbs.on_peer_left(s, pid, s->session_ud);
}

static void internal_on_sig_ice_offer(p2p_signaling_client_t *c,
                                       const char *from, const char *sdp, void *ud)
{
    p2pav_session_t *s = (p2pav_session_t *)ud;
    LOG_DBG("got ICE offer from '%s'", from);

    p2p_peer_ctx_t *peer = p2p_engine_find_peer(&s->engine, from);
    if (!peer) {
        peer = p2p_engine_add_peer(&s->engine, from);
        if (!peer) {
            LOG_ERR("cannot add peer");
            return;
        }
    }

    p2p_peer_set_remote_description(peer, sdp);

    /* Send the answer (with ufrag/pwd) BEFORE gathering so the remote
     * peer can authenticate incoming TCP STUN requests.  The synchronous
     * HTTP POST ensures the signaling server has the answer before we
     * start TCP connections via gather_candidates(). */
    char local_sdp[P2P_SIG_MAX_SDP_SIZE];
    if (p2p_peer_get_local_description(peer, local_sdp, sizeof(local_sdp)) == 0) {
        LOG_INFO("sending ICE answer to '%s' (pre-gathering)", from);
        p2p_signaling_send_ice_answer(&s->sig_client, from, local_sdp);
    }

    s->timing.ice_gathering_start_us = p2p_now_us();
    p2p_peer_gather_candidates(peer);
}

static void internal_on_sig_ice_answer(p2p_signaling_client_t *c,
                                        const char *from, const char *sdp, void *ud)
{
    p2pav_session_t *s = (p2pav_session_t *)ud;
    LOG_INFO("got ICE answer from '%s'", from);
    p2p_peer_ctx_t *peer = p2p_engine_find_peer(&s->engine, from);
    if (peer)
        p2p_peer_set_remote_description(peer, sdp);
    else
        LOG_WARN("no peer found for answer from '%s'", from);
}

static void internal_on_sig_ice_candidate(p2p_signaling_client_t *c,
                                           const char *from, const char *cand, void *ud)
{
    p2pav_session_t *s = (p2pav_session_t *)ud;
    p2p_peer_ctx_t *peer = p2p_engine_find_peer(&s->engine, from);
    if (peer)
        p2p_peer_add_remote_candidate(peer, cand);
}

static void internal_on_sig_gathering_done(p2p_signaling_client_t *c,
                                            const char *from, void *ud)
{
    p2pav_session_t *s = (p2pav_session_t *)ud;
    p2p_peer_ctx_t *peer = p2p_engine_find_peer(&s->engine, from);
    if (peer)
        p2p_peer_set_remote_gathering_done(peer);
}

static void internal_on_sig_turn(p2p_signaling_client_t *c,
                                  const char *user, const char *pass,
                                  const char *server, uint16_t port,
                                  uint32_t ttl, void *ud)
{
    p2pav_session_t *s = (p2pav_session_t *)ud;

    /* When enable_tcp is set we're in an explicit TCP-only / UDP-blocked
     * scenario.  Skip TURN credentials because TURN allocation uses UDP
     * and will only add a ~25-second timeout to gathering. */
    if (s->enable_tcp) {
        LOG_INFO("TURN credentials skipped (ICE-TCP mode): %s:%d",
                 server ? server : "", port);
        return;
    }

    if (server && server[0]) {
        snprintf(s->engine.turn_host, sizeof(s->engine.turn_host), "%s", server);
        s->engine.turn_port = port;
    }
    if (user)
        snprintf(s->engine.turn_username, sizeof(s->engine.turn_username), "%s", user);
    if (pass)
        snprintf(s->engine.turn_password, sizeof(s->engine.turn_password), "%s", pass);
    LOG_INFO("TURN credentials received: %s:%d", server ? server : "", port);
}
