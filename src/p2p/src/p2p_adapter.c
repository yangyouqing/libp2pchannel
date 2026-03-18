#include "p2p_adapter.h"

#include <juice/juice.h>

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

/* Map juice_state_t -> p2p_ice_state_t */
static p2p_ice_state_t map_juice_state(juice_state_t js)
{
    switch (js) {
    case JUICE_STATE_GATHERING:    return P2P_ICE_STATE_GATHERING;
    case JUICE_STATE_CONNECTING:   return P2P_ICE_STATE_CONNECTING;
    case JUICE_STATE_CONNECTED:    return P2P_ICE_STATE_CONNECTED;
    case JUICE_STATE_COMPLETED:    return P2P_ICE_STATE_COMPLETED;
    case JUICE_STATE_FAILED:       return P2P_ICE_STATE_FAILED;
    case JUICE_STATE_DISCONNECTED: return P2P_ICE_STATE_DISCONNECTED;
    default:                       return P2P_ICE_STATE_DISCONNECTED;
    }
}

/* ---------- xquic transport callbacks ---------- */

static void schedule_continue_send(p2p_peer_ctx_t *peer)
{
    peer->needs_continue_send = 1;
    if (peer->engine) {
        p2p_mutex_lock(&peer->engine->timer_mutex);
        uint64_t retry_at = p2p_now_us() + 5000;
        if (peer->engine->next_wakeup_us == 0 ||
            retry_at < peer->engine->next_wakeup_us)
            peer->engine->next_wakeup_us = retry_at;
        p2p_cond_signal(&peer->engine->timer_cond);
        p2p_mutex_unlock(&peer->engine->timer_mutex);
    }
}

static ssize_t xqc_write_socket_cb(const unsigned char *buf, size_t size,
    const struct sockaddr *peer_addr, socklen_t peer_addrlen, void *conn_user_data)
{
    p2p_peer_ctx_t *peer = (p2p_peer_ctx_t *)conn_user_data;
    if (!peer || !peer->ice_agent)
        return XQC_SOCKET_EAGAIN;

    int ret = juice_send((juice_agent_t *)peer->ice_agent, (const char *)buf, size);
    if (ret == JUICE_ERR_SUCCESS)
        return (ssize_t)size;
    schedule_continue_send(peer);
    return XQC_SOCKET_EAGAIN;
}

static ssize_t xqc_pkt_filter_cb(const unsigned char *buf, size_t size,
    const struct sockaddr *peer_addr, socklen_t peer_addrlen, void *cb_user_data)
{
    p2p_peer_ctx_t *peer = (p2p_peer_ctx_t *)cb_user_data;
    if (!peer || !peer->ice_agent)
        return XQC_SOCKET_EAGAIN;

    int ret = juice_send((juice_agent_t *)peer->ice_agent, (const char *)buf, size);
    if (ret == JUICE_ERR_SUCCESS)
        return (ssize_t)size;
    schedule_continue_send(peer);
    return XQC_SOCKET_EAGAIN;
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

static void xqc_save_token_cb(const unsigned char *token, uint32_t token_len,
    void *conn_user_data)
{
    (void)token; (void)token_len; (void)conn_user_data;
}

static void xqc_save_session_cb(const char *data, size_t data_len,
    void *conn_user_data)
{
    (void)data; (void)data_len; (void)conn_user_data;
}

static void xqc_save_tp_cb(const char *data, size_t data_len,
    void *conn_user_data)
{
    (void)data; (void)data_len; (void)conn_user_data;
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
    if (lvl > XQC_LOG_INFO)
        return;
    if (lvl == XQC_LOG_ERROR) {
        const char *s = (const char *)buf;
        if (memmem(s, size, "pkt_filter_cb", 13) || memmem(s, size, "write_socket", 12))
            return;
    }
    fprintf(stderr, "[xquic][%s] %.*s\n", level_str[lvl], (int)size, (const char *)buf);
}

static int xqc_conn_create_notify_cb(xqc_connection_t *conn,
    const xqc_cid_t *cid, void *conn_user_data, void *conn_proto_data)
{
    p2p_peer_ctx_t *peer = (p2p_peer_ctx_t *)conn_user_data;
    if (peer) {
        if (peer->xqc_conn && peer->xqc_conn != conn) {
            fprintf(stderr, "[p2p] new conn for %s, old conn will time out naturally\n",
                    peer->peer_id);
        }
        peer->ctrl_stream = NULL;
        peer->last_ice_recv_us = 0;
        peer->cid = *cid;
        __sync_synchronize();
        peer->xqc_conn = conn;
        xqc_conn_set_pkt_filter_callback(conn, xqc_pkt_filter_cb, peer);
        xqc_datagram_set_user_data(conn, peer);
    }
    return 0;
}

static int xqc_conn_close_notify_cb(xqc_connection_t *conn,
    const xqc_cid_t *cid, void *conn_user_data, void *conn_proto_data)
{
    p2p_peer_ctx_t *peer = (p2p_peer_ctx_t *)conn_user_data;
    if (!peer) return 0;

    if (peer->xqc_conn == conn) {
        /* Set state BEFORE nulling xqc_conn to prevent capture thread
         * from passing state check then using a stale conn pointer */
        int ice_alive = 0;
        if (peer->ice_agent) {
            juice_state_t js = juice_get_state((juice_agent_t *)peer->ice_agent);
            ice_alive = (js == JUICE_STATE_CONNECTED || js == JUICE_STATE_COMPLETED);
        }

        if (ice_alive) {
            peer->state = P2P_PEER_STATE_ICE_CONNECTED;
            __sync_synchronize();
            peer->xqc_conn = NULL;
            peer->ctrl_stream = NULL;
            peer->quic_restart_fail_count++;
            if (peer->engine && peer->engine->role == P2P_ROLE_SUBSCRIBER) {
                if (peer->quic_restart_fail_count > 3) {
                    fprintf(stderr, "[p2p] QUIC failed %d times with %s, escalating to ICE restart\n",
                            peer->quic_restart_fail_count, peer->peer_id);
                    peer->needs_ice_restart = 1;
                    peer->needs_quic_restart = 0;
                    peer->quic_restart_fail_count = 0;
                } else {
                    peer->needs_quic_restart = 1;
                    fprintf(stderr, "[p2p] QUIC closed with %s, ICE alive, will reconnect QUIC (attempt %d)\n",
                            peer->peer_id, peer->quic_restart_fail_count);
                }
            } else {
                fprintf(stderr, "[p2p] QUIC closed with %s (server waits for reconnect)\n",
                        peer->peer_id);
            }
        } else {
            peer->state = P2P_PEER_STATE_FAILED;
            __sync_synchronize();
            peer->xqc_conn = NULL;
            peer->ctrl_stream = NULL;
            peer->needs_quic_restart = 0;
            peer->needs_ice_restart = 1;
            fprintf(stderr, "[p2p] QUIC closed with %s, ICE dead, need full reconnect\n",
                    peer->peer_id);
        }
        if (peer->engine && peer->engine->callbacks.on_peer_quic_closed)
            peer->engine->callbacks.on_peer_quic_closed(peer, peer->engine->user_data);
    } else {
        fprintf(stderr, "[p2p] stale QUIC conn closed for %s (superseded)\n",
                peer->peer_id);
    }
    return 0;
}

static void xqc_conn_handshake_finished_cb(xqc_connection_t *conn,
    void *conn_user_data, void *conn_proto_data)
{
    p2p_peer_ctx_t *peer = (p2p_peer_ctx_t *)conn_user_data;
    if (!peer) return;
    peer->state = P2P_PEER_STATE_QUIC_CONNECTED;
    peer->last_ice_recv_us = p2p_now_us();
    peer->quic_restart_fail_count = 0;
    fprintf(stderr, "[p2p] QUIC handshake finished with peer %s\n", peer->peer_id);
    if (peer->engine && peer->engine->callbacks.on_peer_quic_connected)
        peer->engine->callbacks.on_peer_quic_connected(peer, peer->engine->user_data);
}

/* ---------- xquic STREAM callbacks (reliable control channel) ---------- */

static p2p_peer_ctx_t *stream_get_peer(xqc_stream_t *stream, void *strm_user_data)
{
    if (strm_user_data)
        return (p2p_peer_ctx_t *)strm_user_data;
    void *conn_ud = xqc_get_conn_user_data_by_stream(stream);
    return (p2p_peer_ctx_t *)conn_ud;
}

static xqc_int_t xqc_stream_read_notify_cb(xqc_stream_t *stream, void *strm_user_data)
{
    p2p_peer_ctx_t *peer = stream_get_peer(stream, strm_user_data);
    if (!peer || !peer->engine) return 0;

    if (!peer->ctrl_stream)
        peer->ctrl_stream = stream;

    unsigned char buf[64];
    uint8_t fin = 0;
    ssize_t nread;

    while ((nread = xqc_stream_recv(stream, buf, sizeof(buf), &fin)) > 0) {
        for (ssize_t i = 0; i < nread; i++) {
            uint8_t type = buf[i];
            if (peer->engine->callbacks.on_peer_data_recv) {
                p2p_frame_header_t ctrl_hdr;
                memset(&ctrl_hdr, 0, sizeof(ctrl_hdr));
                ctrl_hdr.type = type;
                peer->engine->callbacks.on_peer_data_recv(peer, &ctrl_hdr, NULL,
                                                           peer->engine->user_data);
            }
        }
    }
    return 0;
}

static xqc_int_t xqc_stream_write_notify_cb(xqc_stream_t *stream, void *strm_user_data)
{
    (void)stream; (void)strm_user_data;
    return 0;
}

static xqc_int_t xqc_stream_create_notify_cb(xqc_stream_t *stream, void *strm_user_data)
{
    p2p_peer_ctx_t *peer = stream_get_peer(stream, strm_user_data);
    if (peer && !peer->ctrl_stream) {
        peer->ctrl_stream = stream;
        fprintf(stderr, "[p2p] control stream created for peer %s\n",
                peer->peer_id);
    }
    return 0;
}

static xqc_int_t xqc_stream_close_notify_cb(xqc_stream_t *stream, void *strm_user_data)
{
    p2p_peer_ctx_t *peer = stream_get_peer(stream, strm_user_data);
    if (peer && peer->ctrl_stream == stream)
        peer->ctrl_stream = NULL;
    return 0;
}

/* ---------- xquic DATAGRAM callbacks ---------- */

static void on_dgram_read(xqc_connection_t *conn, void *user_data,
                           const void *data, size_t data_len, uint64_t unix_ts)
{
    p2p_peer_ctx_t *peer = (p2p_peer_ctx_t *)xqc_datagram_get_user_data(conn);
    if (!peer || !peer->engine) return;

    if (data_len < P2P_FRAME_HDR_SIZE) return;
    if (!peer->engine->callbacks.on_peer_data_recv) return;

    const p2p_frame_header_t *hdr = (const p2p_frame_header_t *)data;
    size_t payload_avail = data_len - P2P_FRAME_HDR_SIZE;

    if (hdr->frag_len > payload_avail) {
        fprintf(stderr, "[p2p-dgram] CORRUPT: frag_len=%u > payload_avail=%zu, dropping\n",
                hdr->frag_len, payload_avail);
        return;
    }

    const uint8_t *payload = (const uint8_t *)data + P2P_FRAME_HDR_SIZE;
    peer->engine->callbacks.on_peer_data_recv(peer, hdr, payload,
                                               peer->engine->user_data);
}

static void on_dgram_write(xqc_connection_t *conn, void *user_data)
{
    p2p_peer_ctx_t *peer = (p2p_peer_ctx_t *)xqc_datagram_get_user_data(conn);
    if (!peer) return;
    p2p_peer_flush_send_queue(peer);
}

static xqc_int_t on_dgram_lost(xqc_connection_t *conn,
    uint64_t dgram_id, void *user_data)
{
    (void)conn; (void)dgram_id; (void)user_data;
    return 0;
}

static void on_dgram_acked(xqc_connection_t *conn,
    uint64_t dgram_id, void *user_data)
{
    (void)conn; (void)dgram_id; (void)user_data;
}

static void on_dgram_mss_updated(xqc_connection_t *conn,
    size_t mss, void *user_data)
{
    fprintf(stderr, "[p2p] DATAGRAM MSS updated: %zu\n", mss);
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
        if (peer->state < P2P_PEER_STATE_ICE_CONNECTED)
            peer->state = P2P_PEER_STATE_ICE_CONNECTED;
        schedule_continue_send(peer);
        break;
    case JUICE_STATE_COMPLETED:
        if (peer->state < P2P_PEER_STATE_ICE_CONNECTED)
            peer->state = P2P_PEER_STATE_ICE_CONNECTED;
        /* Defer xqc_connect to engine thread (xquic is not thread-safe) */
        peer->needs_quic_start = 1;
        schedule_continue_send(peer);
        break;
    case JUICE_STATE_FAILED:
        peer->state = P2P_PEER_STATE_FAILED;
        break;
    case JUICE_STATE_DISCONNECTED:
        peer->state = P2P_PEER_STATE_CLOSED;
        break;
    default:
        break;
    }

    if (peer->engine && peer->engine->callbacks.on_peer_ice_state)
        peer->engine->callbacks.on_peer_ice_state(peer, map_juice_state(state),
                                                   peer->engine->user_data);
}

static void ice_on_candidate(juice_agent_t *agent, const char *sdp, void *user_ptr)
{
    p2p_peer_ctx_t *peer = (p2p_peer_ctx_t *)user_ptr;
    fprintf(stderr, "[p2p] local candidate: %s\n", sdp ? sdp : "(null)");
    if (peer && peer->engine && peer->engine->callbacks.on_peer_ice_candidate)
        peer->engine->callbacks.on_peer_ice_candidate(peer, sdp, peer->engine->user_data);
}

static void ice_on_gathering_done(juice_agent_t *agent, void *user_ptr)
{
    p2p_peer_ctx_t *peer = (p2p_peer_ctx_t *)user_ptr;
    if (peer && peer->engine && peer->engine->callbacks.on_peer_ice_gathering_done)
        peer->engine->callbacks.on_peer_ice_gathering_done(peer, peer->engine->user_data);
}

static void ice_on_recv(juice_agent_t *agent, const char *data, size_t size, void *user_ptr)
{
    p2p_peer_ctx_t *peer = (p2p_peer_ctx_t *)user_ptr;
    if (!peer || !peer->engine) return;

    uint64_t now = p2p_now_us();
    if (size > 0)
        peer->last_ice_recv_us = now;  /* any packet = peer alive (STUN, QUIC, etc.) */
    int ret = p2p_packet_queue_push(&peer->engine->recv_queue,
                                    (const uint8_t *)data, size, now, peer->index);
    if (ret < 0) {
        static int drop_cnt = 0;
        if (++drop_cnt % 500 == 1)
            fprintf(stderr, "[p2p] recv_queue FULL, dropped %d packets so far (peer=%s)\n",
                    drop_cnt, peer->peer_id);
    }
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
        if (peer->state < P2P_PEER_STATE_ICE_CONNECTING)
            continue;
        xqc_engine_packet_process(eng->xqc_engine, pkt.data, pkt.size,
            (struct sockaddr *)&peer->virtual_local_addr, sizeof(peer->virtual_local_addr),
            (struct sockaddr *)&peer->virtual_peer_addr, sizeof(peer->virtual_peer_addr),
            pkt.recv_time_us, peer);
        batch++;
    }
    if (batch > 0) {
        xqc_engine_finish_recv(eng->xqc_engine);
        xqc_engine_main_logic(eng->xqc_engine);
    }
}

static void process_continue_send(p2p_engine_t *eng)
{
    for (int i = 0; i < eng->peer_count; i++) {
        p2p_peer_ctx_t *peer = &eng->peers[i];
        if (peer->needs_continue_send) {
            peer->needs_continue_send = 0;
            if (peer->xqc_conn) {
                xqc_conn_continue_send_by_conn(peer->xqc_conn);
            } else if (peer->cid.cid_len > 0) {
                xqc_conn_continue_send(eng->xqc_engine, &peer->cid);
            }
        }
    }
}

static void process_quic_start(p2p_engine_t *eng)
{
    for (int i = 0; i < eng->peer_count; i++) {
        p2p_peer_ctx_t *peer = &eng->peers[i];
        if (!peer->needs_quic_start)
            continue;
        peer->needs_quic_start = 0;
        if (peer->state < P2P_PEER_STATE_ICE_CONNECTED)
            continue;
        if (p2p_peer_start_quic(peer) != 0)
            fprintf(stderr, "[p2p] QUIC start failed for %s\n", peer->peer_id);
    }
}

static void process_quic_restart(p2p_engine_t *eng)
{
    for (int i = 0; i < eng->peer_count; i++) {
        p2p_peer_ctx_t *peer = &eng->peers[i];

        if (peer->needs_ice_restart) {
            peer->needs_ice_restart = 0;
            peer->needs_quic_restart = 0;
            fprintf(stderr, "[p2p] ICE dead for %s, requesting full reconnect via signaling\n",
                    peer->peer_id);
            if (eng->callbacks.on_peer_ice_restart_needed)
                eng->callbacks.on_peer_ice_restart_needed(peer, eng->user_data);
            continue;
        }

        if (peer->needs_quic_restart && peer->state == P2P_PEER_STATE_ICE_CONNECTED) {
            /* Double-check ICE is truly alive before attempting QUIC */
            if (peer->ice_agent) {
                juice_state_t js = juice_get_state((juice_agent_t *)peer->ice_agent);
                if (js != JUICE_STATE_CONNECTED && js != JUICE_STATE_COMPLETED) {
                    fprintf(stderr, "[p2p] ICE not connected (state=%d) for %s, skip QUIC restart\n",
                            (int)js, peer->peer_id);
                    peer->needs_quic_restart = 0;
                    peer->needs_ice_restart = 1;
                    continue;
                }
            }
            peer->needs_quic_restart = 0;
            fprintf(stderr, "[p2p] attempting QUIC reconnect with %s\n", peer->peer_id);
            if (p2p_peer_start_quic(peer) != 0)
                fprintf(stderr, "[p2p] QUIC reconnect failed for %s\n", peer->peer_id);
        }
    }
}

#define P2P_MANUAL_IDLE_TIMEOUT_US  (60ULL * 1000000)  /* 60s: avoid false timeout when sub has no packet loss */

static void process_manual_idle_check(p2p_engine_t *eng)
{
    if (eng->role != P2P_ROLE_PUBLISHER) return;
    uint64_t now = p2p_now_us();
    static uint64_t last_dbg = 0;
    int do_dbg = (now - last_dbg > 3000000);
    if (do_dbg) last_dbg = now;
    for (int i = 0; i < eng->peer_count; i++) {
        p2p_peer_ctx_t *peer = &eng->peers[i];
        if (do_dbg && peer->peer_id[0])
            fprintf(stderr, "[idle-dbg] %s state=%d conn=%p last=%llu now=%llu\n",
                    peer->peer_id, peer->state, (void*)peer->xqc_conn,
                    (unsigned long long)peer->last_ice_recv_us, (unsigned long long)now);
        if (peer->state != P2P_PEER_STATE_QUIC_CONNECTED || !peer->xqc_conn)
            continue;
        uint64_t last = peer->last_ice_recv_us;
        if (last == 0 || last > now)
            continue;
        uint64_t elapsed = now - last;
        if (elapsed > P2P_MANUAL_IDLE_TIMEOUT_US) {
            fprintf(stderr, "[p2p] manual idle timeout for %s (no data for %llums), closing\n",
                    peer->peer_id, (unsigned long long)(elapsed / 1000));
            xqc_conn_close(eng->xqc_engine, &peer->cid);
            peer->xqc_conn = NULL;
            peer->ctrl_stream = NULL;
            peer->state = P2P_PEER_STATE_ICE_CONNECTED;
            peer->last_ice_recv_us = 0;
        }
    }
}

static void *engine_thread_func(void *arg)
{
    p2p_engine_t *eng = (p2p_engine_t *)arg;
    uint64_t last_main_logic_us = 0;
    while (eng->engine_running) {
        static uint64_t loop_dbg = 0;
        uint64_t loop_now = p2p_now_us();
        if (loop_now - loop_dbg > 5000000) {
            loop_dbg = loop_now;
            fprintf(stderr, "[loop-dbg] alive t=%llu\n", (unsigned long long)loop_now);
        }
        process_recv_queue(eng);
        process_quic_start(eng);
        process_continue_send(eng);
        process_manual_idle_check(eng);
        process_quic_restart(eng);

        /* Flush queued datagrams (enqueued by capture thread) */
        for (int i = 0; i < P2P_MAX_SUBSCRIBERS; i++) {
            p2p_peer_ctx_t *p = &eng->peers[i];
            if (p->xqc_conn && p->state == P2P_PEER_STATE_QUIC_CONNECTED
                && p->send_queue.count > 0)
                p2p_peer_flush_send_queue(p);
        }

        /* Deferred peer removal (safe point after all iteration) */
        for (int i = 0; i < P2P_MAX_SUBSCRIBERS; i++) {
            if (eng->peers[i].needs_removal) {
                eng->peers[i].needs_removal = 0;
                fprintf(stderr, "[p2p] deferred removal of peer %s (idx %d)\n",
                        eng->peers[i].peer_id, i);
                p2p_engine_remove_peer(eng, i);
            }
        }

        p2p_mutex_lock(&eng->timer_mutex);
        uint64_t now = p2p_now_us();

        int run_main = 0;
        if (eng->next_wakeup_us > 0 && now >= eng->next_wakeup_us) {
            eng->next_wakeup_us = 0;
            run_main = 1;
        }
        if (now - last_main_logic_us >= 5000) {
            run_main = 1;
        }
        if (run_main) {
            p2p_mutex_unlock(&eng->timer_mutex);
            last_main_logic_us = now;
            xqc_engine_main_logic(eng->xqc_engine);
            continue;
        }

        uint64_t sleep_us = 2000;
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

    eng->enable_tcp = config->enable_tcp;

    if (p2p_packet_queue_init(&eng->recv_queue, P2P_PKT_QUEUE_CAP) != 0)
        return -1;

    p2p_mutex_init(&eng->timer_mutex);
    p2p_cond_init(&eng->timer_cond);

    for (int i = 0; i < P2P_MAX_SUBSCRIBERS; i++) {
        eng->peers[i].index = i;
        eng->peers[i].engine = eng;
        eng->peers[i].state = P2P_PEER_STATE_IDLE;
        make_virtual_addr(&eng->peers[i].virtual_local_addr, P2P_VIRTUAL_PORT_BASE);
        make_virtual_addr(&eng->peers[i].virtual_peer_addr, P2P_VIRTUAL_PORT_BASE + 1 + i);
    }

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
    engine_cbs.monotonic_ts = p2p_monotonic_us;
    engine_cbs.realtime_ts  = p2p_monotonic_us;
    engine_cbs.log_callbacks.xqc_log_write_err = xqc_log_write_cb;
    engine_cbs.log_callbacks.xqc_log_write_stat = xqc_log_write_cb;

    xqc_transport_callbacks_t tcbs;
    memset(&tcbs, 0, sizeof(tcbs));
    tcbs.write_socket = xqc_write_socket_cb;
    tcbs.server_accept = xqc_server_accept_cb;
    tcbs.stateless_reset = xqc_stateless_reset_cb;
    tcbs.save_token = xqc_save_token_cb;
    tcbs.save_session_cb = xqc_save_session_cb;
    tcbs.save_tp_cb = xqc_save_tp_cb;

    eng->xqc_engine = xqc_engine_create(engine_type, &xqc_cfg, &ssl_cfg,
                                         &engine_cbs, &tcbs, eng);
    if (!eng->xqc_engine) {
        fprintf(stderr, "p2p_engine_init: xqc_engine_create failed\n");
        return -1;
    }

    /* Register ALPN with connection + stream + datagram callbacks */
    xqc_conn_callbacks_t conn_cbs;
    memset(&conn_cbs, 0, sizeof(conn_cbs));
    conn_cbs.conn_create_notify = xqc_conn_create_notify_cb;
    conn_cbs.conn_close_notify = xqc_conn_close_notify_cb;
    conn_cbs.conn_handshake_finished = xqc_conn_handshake_finished_cb;

    xqc_app_proto_callbacks_t ap_cbs;
    memset(&ap_cbs, 0, sizeof(ap_cbs));
    ap_cbs.conn_cbs = conn_cbs;

    ap_cbs.stream_cbs.stream_read_notify   = xqc_stream_read_notify_cb;
    ap_cbs.stream_cbs.stream_write_notify  = xqc_stream_write_notify_cb;
    ap_cbs.stream_cbs.stream_create_notify = xqc_stream_create_notify_cb;
    ap_cbs.stream_cbs.stream_close_notify  = xqc_stream_close_notify_cb;

    ap_cbs.dgram_cbs.datagram_read_notify        = on_dgram_read;
    ap_cbs.dgram_cbs.datagram_write_notify       = on_dgram_write;
    ap_cbs.dgram_cbs.datagram_lost_notify        = on_dgram_lost;
    ap_cbs.dgram_cbs.datagram_acked_notify       = on_dgram_acked;
    ap_cbs.dgram_cbs.datagram_mss_updated_notify = on_dgram_mss_updated;

    if (xqc_engine_register_alpn(eng->xqc_engine, "p2p-av", 6, &ap_cbs, eng) != 0) {
        fprintf(stderr, "p2p_engine_init: xqc_engine_register_alpn failed\n");
        xqc_engine_destroy(eng->xqc_engine);
        eng->xqc_engine = NULL;
        return -1;
    }

    if (config->role == P2P_ROLE_PUBLISHER) {
        xqc_conn_settings_t srv_settings;
        memset(&srv_settings, 0, sizeof(srv_settings));
        srv_settings.max_udp_payload_size = P2P_ICE_MTU;
        srv_settings.max_datagram_frame_size = 65535;
        srv_settings.pacing_on = 1;
        srv_settings.cong_ctrl_callback = xqc_bbr_cb;
        srv_settings.cc_params.customize_on = 1;
        srv_settings.cc_params.init_cwnd = 32;
        srv_settings.cc_params.min_cwnd = 4;
        srv_settings.idle_time_out = 120000;
        srv_settings.init_idle_time_out = 120000;
        srv_settings.sndq_packets_used_max = 200000;
        xqc_server_set_conn_settings(eng->xqc_engine, &srv_settings);
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
            juice_destroy((juice_agent_t *)eng->peers[i].ice_agent);
            eng->peers[i].ice_agent = NULL;
        }
        p2p_mutex_destroy(&eng->peers[i].send_queue.mutex);
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
    jcfg.concurrency_mode = eng->enable_tcp ? JUICE_CONCURRENCY_MODE_POLL
                                            : JUICE_CONCURRENCY_MODE_THREAD;

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

    if (eng->enable_tcp) {
        juice_ice_tcp_mode_t tcp_mode = (eng->role == P2P_ROLE_PUBLISHER)
                                        ? JUICE_ICE_TCP_MODE_PASSIVE
                                        : JUICE_ICE_TCP_MODE_ACTIVE;
        juice_set_ice_tcp_mode((juice_agent_t *)peer->ice_agent, tcp_mode);
        fprintf(stderr, "p2p_engine_add_peer: ICE-TCP %s for peer %s\n",
                tcp_mode == JUICE_ICE_TCP_MODE_PASSIVE ? "passive" : "active", peer_id);
    }

    peer->created_us = p2p_now_us();
    eng->peer_count++;
    return peer;
}

void p2p_engine_remove_peer(p2p_engine_t *eng, int peer_index)
{
    if (!eng || peer_index < 0 || peer_index >= P2P_MAX_SUBSCRIBERS) return;
    p2p_peer_ctx_t *peer = &eng->peers[peer_index];

    peer->ctrl_stream = NULL;

    if (peer->xqc_conn && eng->xqc_engine)
        xqc_conn_close(eng->xqc_engine, &peer->cid);
    peer->xqc_conn = NULL;

    if (peer->ice_agent) {
        juice_destroy((juice_agent_t *)peer->ice_agent);
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
    return juice_gather_candidates((juice_agent_t *)peer->ice_agent);
}

int p2p_peer_set_remote_description(p2p_peer_ctx_t *peer, const char *sdp)
{
    if (!peer || !peer->ice_agent || !sdp) return -1;
    return juice_set_remote_description((juice_agent_t *)peer->ice_agent, sdp);
}

int p2p_peer_add_remote_candidate(p2p_peer_ctx_t *peer, const char *sdp)
{
    if (!peer || !peer->ice_agent || !sdp) return -1;
    fprintf(stderr, "[p2p] adding remote candidate: %s\n", sdp);
    int ret = juice_add_remote_candidate((juice_agent_t *)peer->ice_agent, sdp);
    fprintf(stderr, "[p2p] add_remote_candidate returned %d\n", ret);
    return ret;
}

int p2p_peer_set_remote_gathering_done(p2p_peer_ctx_t *peer)
{
    if (!peer || !peer->ice_agent) return -1;
    return juice_set_remote_gathering_done((juice_agent_t *)peer->ice_agent);
}

int p2p_peer_get_local_description(p2p_peer_ctx_t *peer, char *buf, size_t size)
{
    if (!peer || !peer->ice_agent || !buf) return -1;
    return juice_get_local_description((juice_agent_t *)peer->ice_agent, buf, size);
}

/* ---- ICE info query ---- */

int p2p_get_peer_ice_info(p2p_peer_ctx_t *peer, p2p_ice_info_t *info)
{
    if (!peer || !info) return -1;
    memset(info, 0, sizeof(*info));
    if (!peer->ice_agent) return -1;
    juice_agent_t *agent = (juice_agent_t *)peer->ice_agent;
    juice_get_selected_candidates(agent,
        info->local_cand, sizeof(info->local_cand),
        info->remote_cand, sizeof(info->remote_cand));
    juice_get_selected_addresses(agent,
        info->local_addr, sizeof(info->local_addr),
        info->remote_addr, sizeof(info->remote_addr));
    return 0;
}

/* ---- AV data via xquic DATAGRAM ---- */

int p2p_peer_flush_send_queue(p2p_peer_ctx_t *peer)
{
    if (!peer || !peer->xqc_conn) return -1;
    p2p_dgram_send_queue_t *q = &peer->send_queue;
    p2p_mutex_lock(&q->mutex);
    while (q->count > 0) {
        if (!peer->xqc_conn) {
            p2p_mutex_unlock(&q->mutex);
            return -1;
        }
        p2p_dgram_entry_t *e = &q->entries[q->head];
        uint64_t dgram_id = 0;
        xqc_int_t ret = xqc_datagram_send(peer->xqc_conn, e->data, e->len,
                                           &dgram_id, (xqc_data_qos_level_t)e->qos);
        if (ret == -XQC_EAGAIN) {
            p2p_mutex_unlock(&q->mutex);
            return 0;
        }
        if (ret < 0) {
            /* drop on error */
        }
        q->head = (q->head + 1) % P2P_SEND_QUEUE_CAP;
        q->count--;
    }
    p2p_mutex_unlock(&q->mutex);
    return 1;
}

static int enqueue_dgram(p2p_peer_ctx_t *peer, const uint8_t *data, uint16_t len, int qos)
{
    p2p_dgram_send_queue_t *q = &peer->send_queue;
    p2p_mutex_lock(&q->mutex);

    if (!peer->xqc_conn || peer->state != P2P_PEER_STATE_QUIC_CONNECTED) {
        p2p_mutex_unlock(&q->mutex);
        return -1;
    }

    /* Queue is full: drop oldest */
    if (q->count >= P2P_SEND_QUEUE_CAP) {
        q->head = (q->head + 1) % P2P_SEND_QUEUE_CAP;
        q->count--;
    }
    int idx = (q->head + q->count) % P2P_SEND_QUEUE_CAP;
    memcpy(q->entries[idx].data, data, len);
    q->entries[idx].len = len;
    q->entries[idx].qos = qos;
    q->count++;

    p2p_mutex_unlock(&q->mutex);
    return 1;
}

int p2p_peer_send_data_via_quic(p2p_peer_ctx_t *peer, uint8_t type, uint8_t flags,
                                uint32_t seq, uint64_t timestamp_us,
                                const uint8_t *payload, uint32_t payload_len)
{
    if (!peer) return -1;
    if (peer->state != P2P_PEER_STATE_QUIC_CONNECTED) return -1;
    __sync_synchronize();
    xqc_connection_t *conn = peer->xqc_conn;
    if (!conn) return -1;

    size_t mss = xqc_datagram_get_mss(conn);
    if (mss == 0) {
        /* Peer doesn't support DATAGRAM, fall back to legacy */
        return p2p_peer_send_data(peer, type, flags, seq, timestamp_us, payload, payload_len);
    }

    uint16_t max_frag = (mss > P2P_FRAME_HDR_SIZE)
                      ? (uint16_t)(mss - P2P_FRAME_HDR_SIZE) : 0;
    if (max_frag == 0) return -1;

    /* Limit fragment size to our buffer */
    if (max_frag > P2P_FRAME_MAX_FRAG) max_frag = P2P_FRAME_MAX_FRAG;

    int qos = (type == P2P_FRAME_TYPE_VIDEO && (flags & P2P_FRAME_FLAG_KEY))
              ? XQC_DATA_QOS_HIGHEST
              : (type == P2P_FRAME_TYPE_AUDIO)
                ? XQC_DATA_QOS_HIGH
                : XQC_DATA_QOS_MEDIUM;

    uint8_t buf[P2P_ICE_MTU];
    uint32_t offset = 0;

    while (offset < payload_len) {
        uint32_t remaining = payload_len - offset;
        uint16_t frag_len = (remaining > max_frag) ? max_frag : (uint16_t)remaining;

        p2p_frame_header_t *hdr = (p2p_frame_header_t *)buf;
        hdr->type = type;
        hdr->flags = (offset == 0) ? flags : 0;
        hdr->seq = seq;
        hdr->timestamp_us = timestamp_us;
        hdr->total_len = payload_len;
        hdr->frag_offset = offset;
        hdr->frag_len = frag_len;
        memcpy(buf + P2P_FRAME_HDR_SIZE, payload + offset, frag_len);

        int ret = enqueue_dgram(peer, buf, (uint16_t)(P2P_FRAME_HDR_SIZE + frag_len), qos);
        if (ret < 0) return -1;

        offset += frag_len;
    }

    /* Kick the xquic engine to process sends */
    if (peer->engine && peer->engine->xqc_engine) {
        p2p_mutex_lock(&peer->engine->timer_mutex);
        peer->engine->next_wakeup_us = p2p_now_us();
        p2p_cond_signal(&peer->engine->timer_cond);
        p2p_mutex_unlock(&peer->engine->timer_mutex);
    }

    return 0;
}

/* ---- Legacy direct framing (bypass xquic) ---- */

int p2p_peer_send_data(p2p_peer_ctx_t *peer, uint8_t type, uint8_t flags,
                       uint32_t seq, uint64_t timestamp_us,
                       const uint8_t *payload, uint32_t payload_len)
{
    if (!peer || !peer->ice_agent) return -1;
    if (peer->state < P2P_PEER_STATE_ICE_CONNECTED ||
        peer->state >= P2P_PEER_STATE_FAILED) return -1;

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

        int ret = juice_send((juice_agent_t *)peer->ice_agent,
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

/* ---- Control messages via xquic reliable STREAM ---- */

static int p2p_peer_send_control_reliable(p2p_peer_ctx_t *peer, uint8_t type)
{
    if (!peer) return -1;
    xqc_connection_t *conn = peer->xqc_conn;
    if (!conn || peer->state < P2P_PEER_STATE_QUIC_CONNECTED ||
        peer->state >= P2P_PEER_STATE_FAILED) return -1;

    /* Create control stream on first use */
    if (!peer->ctrl_stream) {
        peer->ctrl_stream = xqc_stream_create(peer->engine->xqc_engine,
                                               &peer->cid, NULL, peer);
        if (!peer->ctrl_stream) {
            fprintf(stderr, "[p2p] failed to create control stream for %s\n", peer->peer_id);
            return -1;
        }
    }

    unsigned char msg = type;
    ssize_t sent = xqc_stream_send(peer->ctrl_stream, &msg, 1, 0);
    if (sent < 0) {
        fprintf(stderr, "[p2p] xqc_stream_send control %d failed: %zd\n", type, sent);
        return -1;
    }

    /* Wake engine to flush */
    if (peer->engine) {
        p2p_mutex_lock(&peer->engine->timer_mutex);
        peer->engine->next_wakeup_us = p2p_now_us();
        p2p_cond_signal(&peer->engine->timer_cond);
        p2p_mutex_unlock(&peer->engine->timer_mutex);
    }

    return 0;
}

int p2p_peer_send_idr_request(p2p_peer_ctx_t *peer)
{
    return p2p_peer_send_control_reliable(peer, P2P_FRAME_TYPE_IDR_REQ);
}

int p2p_peer_send_video_stop(p2p_peer_ctx_t *peer)
{ return p2p_peer_send_control_reliable(peer, P2P_FRAME_TYPE_VIDEO_STOP); }

int p2p_peer_send_video_start(p2p_peer_ctx_t *peer)
{ return p2p_peer_send_control_reliable(peer, P2P_FRAME_TYPE_VIDEO_START); }

int p2p_peer_send_audio_stop(p2p_peer_ctx_t *peer)
{ return p2p_peer_send_control_reliable(peer, P2P_FRAME_TYPE_AUDIO_STOP); }

int p2p_peer_send_audio_start(p2p_peer_ctx_t *peer)
{ return p2p_peer_send_control_reliable(peer, P2P_FRAME_TYPE_AUDIO_START); }

int p2p_peer_send_ping(p2p_peer_ctx_t *peer)
{ return p2p_peer_send_control_reliable(peer, P2P_FRAME_TYPE_PING); }

/* ---- QUIC connection ---- */

int p2p_peer_start_quic(p2p_peer_ctx_t *peer)
{
    if (!peer || !peer->engine || !peer->engine->xqc_engine) return -1;
    if (peer->state >= P2P_PEER_STATE_QUIC_HANDSHAKING) return 0;
    p2p_engine_t *eng = peer->engine;

    /* Initialize send queue */
    memset(&peer->send_queue, 0, sizeof(peer->send_queue));
    p2p_mutex_init(&peer->send_queue.mutex);

    if (eng->role == P2P_ROLE_SUBSCRIBER) {
        peer->state = P2P_PEER_STATE_QUIC_HANDSHAKING;

        xqc_conn_settings_t settings;
        memset(&settings, 0, sizeof(settings));
        settings.max_udp_payload_size = P2P_ICE_MTU;
        settings.max_datagram_frame_size = 65535;
        settings.pacing_on = 1;
        settings.cong_ctrl_callback = xqc_bbr_cb;
        settings.cc_params.customize_on = 1;
        settings.cc_params.init_cwnd = 32;
        settings.cc_params.min_cwnd = 4;
        settings.idle_time_out = 15000;
        settings.init_idle_time_out = 3000;
        settings.sndq_packets_used_max = 200000;

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
        fprintf(stderr, "[p2p] QUIC connect initiated to peer %s (BBR+pacing+datagram)\n",
                peer->peer_id);
    }

    return 0;
}

/* ---- Video Jitter Buffer ---- */

void p2p_video_jitter_init(p2p_video_jitter_buf_t *jb, uint64_t target_delay_us)
{
    if (!jb) return;
    memset(jb, 0, sizeof(*jb));
    jb->target_delay_us = target_delay_us;
    p2p_mutex_init(&jb->mutex);
}

void p2p_video_jitter_destroy(p2p_video_jitter_buf_t *jb)
{
    if (!jb) return;
    for (int i = 0; i < P2P_VIDEO_JITTER_SIZE; i++) {
        free(jb->frames[i].y);
        free(jb->frames[i].u);
        free(jb->frames[i].v);
        jb->frames[i].y = jb->frames[i].u = jb->frames[i].v = NULL;
    }
    p2p_mutex_destroy(&jb->mutex);
}

void p2p_video_jitter_push(p2p_video_jitter_buf_t *jb,
                           uint8_t *y, uint8_t *u, uint8_t *v,
                           int lsy, int lsu, int lsv, int w, int h,
                           uint64_t timestamp_us)
{
    if (!jb) return;
    p2p_mutex_lock(&jb->mutex);

    if (jb->count >= P2P_VIDEO_JITTER_SIZE) {
        p2p_jitter_frame_t *old = &jb->frames[jb->read_idx];
        free(old->y); free(old->u); free(old->v);
        old->y = old->u = old->v = NULL;
        old->valid = 0;
        jb->read_idx = (jb->read_idx + 1) % P2P_VIDEO_JITTER_SIZE;
        jb->count--;
    }

    p2p_jitter_frame_t *f = &jb->frames[jb->write_idx];
    int y_size = lsy * h;
    int uv_h = h / 2;
    int u_size = lsu * uv_h;
    int v_size = lsv * uv_h;

    f->y = (uint8_t *)realloc(f->y, y_size);
    f->u = (uint8_t *)realloc(f->u, u_size);
    f->v = (uint8_t *)realloc(f->v, v_size);
    if (f->y) memcpy(f->y, y, y_size);
    if (f->u) memcpy(f->u, u, u_size);
    if (f->v) memcpy(f->v, v, v_size);
    f->lsy = lsy; f->lsu = lsu; f->lsv = lsv;
    f->w = w; f->h = h;
    f->timestamp_us = timestamp_us;
    f->valid = 1;

    jb->write_idx = (jb->write_idx + 1) % P2P_VIDEO_JITTER_SIZE;
    jb->count++;

    p2p_mutex_unlock(&jb->mutex);
}

int p2p_video_jitter_pop(p2p_video_jitter_buf_t *jb, p2p_jitter_frame_t *out)
{
    if (!jb || !out) return 0;
    p2p_mutex_lock(&jb->mutex);

    if (jb->count == 0) {
        p2p_mutex_unlock(&jb->mutex);
        return 0;
    }

    uint64_t now = p2p_now_us();
    p2p_jitter_frame_t *f = &jb->frames[jb->read_idx];

    if (jb->count < 2 && f->timestamp_us > 0 &&
        (now - f->timestamp_us) < jb->target_delay_us) {
        p2p_mutex_unlock(&jb->mutex);
        return 0;
    }

    *out = *f;
    f->y = f->u = f->v = NULL;
    f->valid = 0;
    jb->read_idx = (jb->read_idx + 1) % P2P_VIDEO_JITTER_SIZE;
    jb->count--;

    p2p_mutex_unlock(&jb->mutex);
    return 1;
}

/* ---- Logging ---- */

void p2p_adapter_set_log_level(int level)
{
    juice_log_level_t jl = JUICE_LOG_LEVEL_WARN;
    switch (level) {
    case 0: jl = JUICE_LOG_LEVEL_NONE;    break;
    case 1: jl = JUICE_LOG_LEVEL_ERROR;   break;
    case 2: jl = JUICE_LOG_LEVEL_WARN;    break;
    case 3: jl = JUICE_LOG_LEVEL_INFO;    break;
    case 4: jl = JUICE_LOG_LEVEL_DEBUG;   break;
    case 5: jl = JUICE_LOG_LEVEL_VERBOSE; break;
    }
    juice_set_log_level(jl);
}
