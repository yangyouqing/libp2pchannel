#include "p2p_adapter.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

/* ---------- helpers ---------- */

uint64_t p2p_now_us(void)
{
    return p2p_monotonic_us();
}

static void make_virtual_addr(struct sockaddr_in *addr, uint16_t port)
{
    memset(addr, 0, sizeof(*addr));
    addr->sin_family = AF_INET;
    addr->sin_port = htons(port);
    addr->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
}

/* ---------- xquic transport callbacks ---------- */

static ssize_t xqc_write_socket_cb(const unsigned char *buf, size_t size,
    const struct sockaddr *peer_addr, socklen_t peer_addrlen, void *conn_user_data)
{
    p2p_peer_ctx_t *peer = (p2p_peer_ctx_t *)conn_user_data;
    if (!peer || !peer->ice_agent) return XQC_SOCKET_ERROR;

    int ret = juice_send(peer->ice_agent, (const char *)buf, size);
    if (ret == JUICE_ERR_SUCCESS)
        return (ssize_t)size;
    if (ret == JUICE_ERR_AGAIN)
        return XQC_SOCKET_EAGAIN;
    return XQC_SOCKET_ERROR;
}

static ssize_t xqc_pkt_filter_cb(const unsigned char *buf, size_t size,
    const struct sockaddr *peer_addr, socklen_t peer_addrlen, void *cb_user_data)
{
    p2p_peer_ctx_t *peer = (p2p_peer_ctx_t *)cb_user_data;
    if (!peer || !peer->ice_agent) return XQC_SOCKET_ERROR;

    int ret = juice_send(peer->ice_agent, (const char *)buf, size);
    if (ret == JUICE_ERR_SUCCESS)
        return (ssize_t)size;
    if (ret == JUICE_ERR_AGAIN)
        return XQC_SOCKET_EAGAIN;
    return XQC_SOCKET_ERROR;
}

static int xqc_server_accept_cb(xqc_engine_t *engine, xqc_connection_t *conn,
    const xqc_cid_t *cid, void *user_data)
{
    return 0;
}

static ssize_t xqc_stateless_reset_cb(const unsigned char *buf, size_t size,
    const struct sockaddr *peer_addr, socklen_t peer_addrlen,
    const struct sockaddr *local_addr, socklen_t local_addrlen,
    void *user_data)
{
    return (ssize_t)size;
}

static void xqc_set_event_timer_cb(xqc_usec_t wake_after, void *engine_user_data)
{
    p2p_engine_t *eng = (p2p_engine_t *)engine_user_data;
    uint64_t now = p2p_now_us();

    p2p_mutex_lock(&eng->timer_mutex);
    eng->next_wakeup_us = now + wake_after;
    p2p_cond_signal(&eng->timer_cond);
    p2p_mutex_unlock(&eng->timer_mutex);
}

static void xqc_log_write_cb(xqc_log_level_t lvl, const void *buf,
    size_t size, void *engine_user_data)
{
    const char *level_str[] = {"RPT", "FAT", "ERR", "WRN", "STA", "INF", "DBG"};
    if (lvl <= XQC_LOG_WARN)
        fprintf(stderr, "[xquic][%s] %.*s\n", level_str[lvl], (int)size, (const char *)buf);
}

static int xqc_conn_create_notify_cb(xqc_connection_t *conn,
    const xqc_cid_t *cid, void *conn_user_data, void *conn_proto_data)
{
    p2p_peer_ctx_t *peer = (p2p_peer_ctx_t *)conn_user_data;
    if (peer) {
        peer->xqc_conn = conn;
        xqc_conn_set_pkt_filter_callback(conn, xqc_pkt_filter_cb, peer);
    }
    return 0;
}

static int xqc_conn_close_notify_cb(xqc_connection_t *conn,
    const xqc_cid_t *cid, void *conn_user_data, void *conn_proto_data)
{
    p2p_peer_ctx_t *peer = (p2p_peer_ctx_t *)conn_user_data;
    if (peer && peer->engine && peer->engine->callbacks.on_peer_quic_closed)
        peer->engine->callbacks.on_peer_quic_closed(peer, peer->engine->user_data);
    return 0;
}

static void xqc_conn_handshake_finished_cb(xqc_connection_t *conn,
    void *conn_user_data, void *conn_proto_data)
{
    p2p_peer_ctx_t *peer = (p2p_peer_ctx_t *)conn_user_data;
    if (!peer) return;
    peer->state = P2P_PEER_STATE_QUIC_CONNECTED;
    if (peer->engine && peer->engine->callbacks.on_peer_quic_connected)
        peer->engine->callbacks.on_peer_quic_connected(peer, peer->engine->user_data);
}

/* ---------- libjuice ICE callbacks ---------- */

static void ice_on_state_changed(juice_agent_t *agent, juice_state_t state, void *user_ptr)
{
    p2p_peer_ctx_t *peer = (p2p_peer_ctx_t *)user_ptr;
    if (!peer) return;

    switch (state) {
    case JUICE_STATE_GATHERING:
        peer->state = P2P_PEER_STATE_ICE_GATHERING;
        break;
    case JUICE_STATE_CONNECTING:
        peer->state = P2P_PEER_STATE_ICE_CONNECTING;
        break;
    case JUICE_STATE_CONNECTED:
    case JUICE_STATE_COMPLETED:
        peer->state = P2P_PEER_STATE_ICE_CONNECTED;
        break;
    case JUICE_STATE_FAILED:
        peer->state = P2P_PEER_STATE_FAILED;
        break;
    default:
        break;
    }

    if (peer->engine && peer->engine->callbacks.on_peer_ice_state)
        peer->engine->callbacks.on_peer_ice_state(peer, state, peer->engine->user_data);
}

static void ice_on_candidate(juice_agent_t *agent, const char *sdp, void *user_ptr)
{
    p2p_peer_ctx_t *peer = (p2p_peer_ctx_t *)user_ptr;
    if (peer && peer->engine && peer->engine->callbacks.on_peer_ice_candidate)
        peer->engine->callbacks.on_peer_ice_candidate(peer, sdp, peer->engine->user_data);
}

static void ice_on_gathering_done(juice_agent_t *agent, void *user_ptr)
{
    p2p_peer_ctx_t *peer = (p2p_peer_ctx_t *)user_ptr;
    if (peer && peer->engine && peer->engine->callbacks.on_peer_ice_gathering_done)
        peer->engine->callbacks.on_peer_ice_gathering_done(peer, peer->engine->user_data);
}

static int is_p2p_frame(const char *data, size_t size)
{
    if (size < 1) return 0;
    uint8_t type = (uint8_t)data[0];
    if (type == P2P_FRAME_TYPE_IDR_REQ)
        return (size >= 1);
    if (size < P2P_FRAME_HDR_SIZE) return 0;
    return (type == P2P_FRAME_TYPE_VIDEO || type == P2P_FRAME_TYPE_AUDIO);
}

static void ice_on_recv(juice_agent_t *agent, const char *data, size_t size, void *user_ptr)
{
    p2p_peer_ctx_t *peer = (p2p_peer_ctx_t *)user_ptr;
    if (!peer || !peer->engine) return;

    /* Direct-framed AV packet -> dispatch via data_recv callback */
    if (is_p2p_frame(data, size) &&
        peer->engine->callbacks.on_peer_data_recv) {
        uint8_t type = (uint8_t)data[0];
        if (type == P2P_FRAME_TYPE_IDR_REQ) {
            p2p_frame_header_t idr_hdr;
            memset(&idr_hdr, 0, sizeof(idr_hdr));
            idr_hdr.type = P2P_FRAME_TYPE_IDR_REQ;
            peer->engine->callbacks.on_peer_data_recv(peer, &idr_hdr, NULL,
                                                       peer->engine->user_data);
            return;
        }
        const p2p_frame_header_t *hdr = (const p2p_frame_header_t *)data;
        const uint8_t *payload = (const uint8_t *)data + P2P_FRAME_HDR_SIZE;
        peer->engine->callbacks.on_peer_data_recv(peer, hdr, payload,
                                                   peer->engine->user_data);
        return;
    }

    /* QUIC packet -> enqueue for xquic engine thread */
    p2p_packet_queue_push(&peer->engine->recv_queue,
                          (const uint8_t *)data, size, p2p_now_us(), peer->index);
}

/* ---------- xquic engine thread ---------- */

static void process_recv_queue(p2p_engine_t *eng)
{
    p2p_packet_t pkt;
    int batch = 0;
    while (p2p_packet_queue_pop(&eng->recv_queue, &pkt) == 0) {
        if (pkt.agent_idx < 0 || pkt.agent_idx >= P2P_MAX_SUBSCRIBERS)
            continue;
        p2p_peer_ctx_t *peer = &eng->peers[pkt.agent_idx];
        xqc_engine_packet_process(eng->xqc_engine, pkt.data, pkt.size,
            (struct sockaddr *)&peer->virtual_local_addr, sizeof(peer->virtual_local_addr),
            (struct sockaddr *)&peer->virtual_peer_addr, sizeof(peer->virtual_peer_addr),
            pkt.recv_time_us, peer);
        batch++;
    }
    if (batch > 0)
        xqc_engine_finish_recv(eng->xqc_engine);
}

static void *engine_thread_func(void *arg)
{
    p2p_engine_t *eng = (p2p_engine_t *)arg;
    while (eng->engine_running) {
        process_recv_queue(eng);

        p2p_mutex_lock(&eng->timer_mutex);
        uint64_t now = p2p_now_us();
        if (eng->next_wakeup_us > 0 && now >= eng->next_wakeup_us) {
            eng->next_wakeup_us = 0;
            p2p_mutex_unlock(&eng->timer_mutex);
            xqc_engine_main_logic(eng->xqc_engine);
            continue;
        }

        uint64_t sleep_us = 5000;
        if (eng->next_wakeup_us > 0 && eng->next_wakeup_us > now) {
            uint64_t delta = eng->next_wakeup_us - now;
            if (delta < sleep_us) sleep_us = delta;
        }
        p2p_cond_timedwait_us(&eng->timer_cond, &eng->timer_mutex, sleep_us);
        p2p_mutex_unlock(&eng->timer_mutex);
    }
    return NULL;
}

/* ---------- public API ---------- */

int p2p_engine_init(p2p_engine_t *eng, const p2p_engine_config_t *config)
{
    if (!eng || !config) return -1;
    memset(eng, 0, sizeof(*eng));

    eng->role = config->role;
    eng->callbacks = config->callbacks;
    eng->user_data = config->user_data;

    if (config->stun_server_host)
        snprintf(eng->stun_host, sizeof(eng->stun_host), "%s", config->stun_server_host);
    eng->stun_port = config->stun_server_port ? config->stun_server_port : 3478;

    if (config->turn_server_host)
        snprintf(eng->turn_host, sizeof(eng->turn_host), "%s", config->turn_server_host);
    eng->turn_port = config->turn_server_port ? config->turn_server_port : 3478;
    if (config->turn_username)
        snprintf(eng->turn_username, sizeof(eng->turn_username), "%s", config->turn_username);
    if (config->turn_password)
        snprintf(eng->turn_password, sizeof(eng->turn_password), "%s", config->turn_password);

    if (config->ssl_cert_file)
        snprintf(eng->ssl_cert_file, sizeof(eng->ssl_cert_file), "%s", config->ssl_cert_file);
    if (config->ssl_key_file)
        snprintf(eng->ssl_key_file, sizeof(eng->ssl_key_file), "%s", config->ssl_key_file);

    /* Init receive queue */
    if (p2p_packet_queue_init(&eng->recv_queue, P2P_PKT_QUEUE_CAP) != 0)
        return -1;

    p2p_mutex_init(&eng->timer_mutex);
    p2p_cond_init(&eng->timer_cond);

    /* Init peer slots */
    for (int i = 0; i < P2P_MAX_SUBSCRIBERS; i++) {
        eng->peers[i].index = i;
        eng->peers[i].engine = eng;
        eng->peers[i].state = P2P_PEER_STATE_IDLE;
        make_virtual_addr(&eng->peers[i].virtual_local_addr, P2P_VIRTUAL_PORT_BASE);
        make_virtual_addr(&eng->peers[i].virtual_peer_addr, P2P_VIRTUAL_PORT_BASE + 1 + i);
    }

    /* Create xquic engine */
    xqc_engine_type_t engine_type = (config->role == P2P_ROLE_PUBLISHER)
        ? XQC_ENGINE_SERVER : XQC_ENGINE_CLIENT;

    xqc_config_t xqc_cfg;
    if (xqc_engine_get_default_config(&xqc_cfg, engine_type) < 0)
        return -1;
    xqc_cfg.cfg_log_level = XQC_LOG_WARN;

    xqc_engine_ssl_config_t ssl_cfg;
    memset(&ssl_cfg, 0, sizeof(ssl_cfg));
    if (config->ssl_cert_file)
        ssl_cfg.cert_file = (char *)config->ssl_cert_file;
    if (config->ssl_key_file)
        ssl_cfg.private_key_file = (char *)config->ssl_key_file;

    xqc_engine_callback_t engine_cbs;
    memset(&engine_cbs, 0, sizeof(engine_cbs));
    engine_cbs.set_event_timer = xqc_set_event_timer_cb;
    engine_cbs.log_callbacks.xqc_log_write_err = xqc_log_write_cb;
    engine_cbs.log_callbacks.xqc_log_write_stat = xqc_log_write_cb;

    xqc_transport_callbacks_t tcbs;
    memset(&tcbs, 0, sizeof(tcbs));
    tcbs.write_socket = xqc_write_socket_cb;
    tcbs.server_accept = xqc_server_accept_cb;
    tcbs.stateless_reset = xqc_stateless_reset_cb;

    eng->xqc_engine = xqc_engine_create(engine_type, &xqc_cfg, &ssl_cfg,
                                         &engine_cbs, &tcbs, eng);
    if (!eng->xqc_engine) {
        fprintf(stderr, "p2p_engine_init: xqc_engine_create failed\n");
        return -1;
    }

    /* Register QUIC ALPN with connection callbacks */
    xqc_conn_callbacks_t conn_cbs;
    memset(&conn_cbs, 0, sizeof(conn_cbs));
    conn_cbs.conn_create_notify = xqc_conn_create_notify_cb;
    conn_cbs.conn_close_notify = xqc_conn_close_notify_cb;
    conn_cbs.conn_handshake_finished = xqc_conn_handshake_finished_cb;

    xqc_app_proto_callbacks_t ap_cbs;
    memset(&ap_cbs, 0, sizeof(ap_cbs));
    ap_cbs.conn_cbs = conn_cbs;

    if (xqc_engine_register_alpn(eng->xqc_engine, "p2p-av", 6, &ap_cbs, eng) != 0) {
        fprintf(stderr, "p2p_engine_init: xqc_engine_register_alpn failed\n");
        xqc_engine_destroy(eng->xqc_engine);
        eng->xqc_engine = NULL;
        return -1;
    }

    return 0;
}

int p2p_engine_start(p2p_engine_t *eng)
{
    if (!eng) return -1;
    eng->engine_running = 1;
    if (p2p_thread_create(&eng->engine_thread, engine_thread_func, eng) != 0) {
        eng->engine_running = 0;
        return -1;
    }
    return 0;
}

void p2p_engine_stop(p2p_engine_t *eng)
{
    if (!eng) return;
    eng->engine_running = 0;
    p2p_mutex_lock(&eng->timer_mutex);
    p2p_cond_signal(&eng->timer_cond);
    p2p_mutex_unlock(&eng->timer_mutex);
    p2p_thread_join(eng->engine_thread);
}

void p2p_engine_destroy(p2p_engine_t *eng)
{
    if (!eng) return;

    for (int i = 0; i < P2P_MAX_SUBSCRIBERS; i++) {
        if (eng->peers[i].ice_agent) {
            juice_destroy(eng->peers[i].ice_agent);
            eng->peers[i].ice_agent = NULL;
        }
    }

    if (eng->xqc_engine) {
        xqc_engine_destroy(eng->xqc_engine);
        eng->xqc_engine = NULL;
    }

    p2p_packet_queue_destroy(&eng->recv_queue);
    p2p_mutex_destroy(&eng->timer_mutex);
    p2p_cond_destroy(&eng->timer_cond);
}

p2p_peer_ctx_t *p2p_engine_add_peer(p2p_engine_t *eng, const char *peer_id)
{
    if (!eng || !peer_id) return NULL;
    if (eng->peer_count >= P2P_MAX_SUBSCRIBERS) return NULL;

    int idx = -1;
    for (int i = 0; i < P2P_MAX_SUBSCRIBERS; i++) {
        if (eng->peers[i].state == P2P_PEER_STATE_IDLE &&
            eng->peers[i].ice_agent == NULL) {
            idx = i;
            break;
        }
    }
    if (idx < 0) return NULL;

    p2p_peer_ctx_t *peer = &eng->peers[idx];
    snprintf(peer->peer_id, sizeof(peer->peer_id), "%s", peer_id);

    juice_config_t jcfg;
    memset(&jcfg, 0, sizeof(jcfg));
    jcfg.concurrency_mode = JUICE_CONCURRENCY_MODE_THREAD;

    if (eng->stun_host[0]) {
        jcfg.stun_server_host = eng->stun_host;
        jcfg.stun_server_port = eng->stun_port;
    }

    juice_turn_server_t turn;
    if (eng->turn_host[0]) {
        memset(&turn, 0, sizeof(turn));
        turn.host = eng->turn_host;
        turn.port = eng->turn_port;
        turn.username = eng->turn_username;
        turn.password = eng->turn_password;
        jcfg.turn_servers = &turn;
        jcfg.turn_servers_count = 1;
    }

    jcfg.cb_state_changed = ice_on_state_changed;
    jcfg.cb_candidate = ice_on_candidate;
    jcfg.cb_gathering_done = ice_on_gathering_done;
    jcfg.cb_recv = ice_on_recv;
    jcfg.user_ptr = peer;

    peer->ice_agent = juice_create(&jcfg);
    if (!peer->ice_agent) {
        fprintf(stderr, "p2p_engine_add_peer: juice_create failed\n");
        return NULL;
    }

    eng->peer_count++;
    return peer;
}

void p2p_engine_remove_peer(p2p_engine_t *eng, int peer_index)
{
    if (!eng || peer_index < 0 || peer_index >= P2P_MAX_SUBSCRIBERS) return;
    p2p_peer_ctx_t *peer = &eng->peers[peer_index];

    if (peer->xqc_conn && eng->xqc_engine)
        xqc_conn_close(eng->xqc_engine, &peer->cid);
    peer->xqc_conn = NULL;

    if (peer->ice_agent) {
        juice_destroy(peer->ice_agent);
        peer->ice_agent = NULL;
    }

    peer->state = P2P_PEER_STATE_IDLE;
    memset(peer->peer_id, 0, sizeof(peer->peer_id));
    if (eng->peer_count > 0) eng->peer_count--;
}

p2p_peer_ctx_t *p2p_engine_find_peer(p2p_engine_t *eng, const char *peer_id)
{
    if (!eng || !peer_id) return NULL;
    for (int i = 0; i < P2P_MAX_SUBSCRIBERS; i++) {
        if (eng->peers[i].ice_agent && strcmp(eng->peers[i].peer_id, peer_id) == 0)
            return &eng->peers[i];
    }
    return NULL;
}

/* ---- ICE operations ---- */

int p2p_peer_gather_candidates(p2p_peer_ctx_t *peer)
{
    if (!peer || !peer->ice_agent) return -1;
    return juice_gather_candidates(peer->ice_agent);
}

int p2p_peer_set_remote_description(p2p_peer_ctx_t *peer, const char *sdp)
{
    if (!peer || !peer->ice_agent || !sdp) return -1;
    return juice_set_remote_description(peer->ice_agent, sdp);
}

int p2p_peer_add_remote_candidate(p2p_peer_ctx_t *peer, const char *sdp)
{
    if (!peer || !peer->ice_agent || !sdp) return -1;
    return juice_add_remote_candidate(peer->ice_agent, sdp);
}

int p2p_peer_set_remote_gathering_done(p2p_peer_ctx_t *peer)
{
    if (!peer || !peer->ice_agent) return -1;
    return juice_set_remote_gathering_done(peer->ice_agent);
}

int p2p_peer_get_local_description(p2p_peer_ctx_t *peer, char *buf, size_t size)
{
    if (!peer || !peer->ice_agent || !buf) return -1;
    return juice_get_local_description(peer->ice_agent, buf, size);
}

/* ---- Direct framing ---- */

int p2p_peer_send_data(p2p_peer_ctx_t *peer, uint8_t type, uint8_t flags,
                       uint32_t seq, uint64_t timestamp_us,
                       const uint8_t *payload, uint32_t payload_len)
{
    if (!peer || !peer->ice_agent) return -1;
    if (peer->state < P2P_PEER_STATE_ICE_CONNECTED) return -1;

    uint8_t buf[P2P_ICE_MTU];
    uint32_t offset = 0;

    while (offset < payload_len) {
        uint32_t remaining = payload_len - offset;
        uint16_t frag_len = (remaining > P2P_FRAME_MAX_FRAG)
                          ? P2P_FRAME_MAX_FRAG : (uint16_t)remaining;

        p2p_frame_header_t *hdr = (p2p_frame_header_t *)buf;
        hdr->type = type;
        hdr->flags = (offset == 0) ? flags : 0;
        hdr->seq = seq;
        hdr->timestamp_us = timestamp_us;
        hdr->total_len = payload_len;
        hdr->frag_offset = offset;
        hdr->frag_len = frag_len;

        memcpy(buf + P2P_FRAME_HDR_SIZE, payload + offset, frag_len);

        int ret = juice_send(peer->ice_agent,
                             (const char *)buf,
                             P2P_FRAME_HDR_SIZE + frag_len);
        if (ret != JUICE_ERR_SUCCESS) {
            fprintf(stderr, "[TX-ERR] juice_send failed seq=%u frag_off=%u ret=%d\n",
                    seq, offset, ret);
            return -1;
        }

        offset += frag_len;
    }
    return 0;
}

/* Send an IDR request to the remote peer (1-byte packet) */
int p2p_peer_send_idr_request(p2p_peer_ctx_t *peer)
{
    if (!peer || !peer->ice_agent) return -1;
    if (peer->state < P2P_PEER_STATE_ICE_CONNECTED) return -1;
    uint8_t msg = P2P_FRAME_TYPE_IDR_REQ;
    return juice_send(peer->ice_agent, (const char *)&msg, 1);
}

/* ---- QUIC connection ---- */

int p2p_peer_start_quic(p2p_peer_ctx_t *peer)
{
    if (!peer || !peer->engine || !peer->engine->xqc_engine) return -1;
    p2p_engine_t *eng = peer->engine;

    if (eng->role == P2P_ROLE_SUBSCRIBER) {
        peer->state = P2P_PEER_STATE_QUIC_HANDSHAKING;

        xqc_conn_settings_t settings;
        memset(&settings, 0, sizeof(settings));
        settings.max_udp_payload_size = P2P_ICE_MTU;

        xqc_conn_ssl_config_t conn_ssl;
        memset(&conn_ssl, 0, sizeof(conn_ssl));

        const xqc_cid_t *cid = xqc_connect(eng->xqc_engine, &settings,
            NULL, 0, "p2p-server", 0, &conn_ssl,
            (struct sockaddr *)&peer->virtual_peer_addr,
            sizeof(peer->virtual_peer_addr),
            "p2p-av", peer);
        if (!cid) {
            peer->state = P2P_PEER_STATE_FAILED;
            return -1;
        }
        peer->cid = *cid;
    }

    return 0;
}
