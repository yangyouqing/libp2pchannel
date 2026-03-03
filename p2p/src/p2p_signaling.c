#include "p2p_signaling.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

/*
 * Minimal JSON serialization / deserialization.
 * Uses hand-written helpers to avoid external dependencies.
 */

static void json_escape(const char *src, char *dst, size_t dst_sz)
{
    size_t j = 0;
    for (size_t i = 0; src[i] && j + 6 < dst_sz; i++) {
        switch (src[i]) {
        case '"':  dst[j++] = '\\'; dst[j++] = '"';  break;
        case '\\': dst[j++] = '\\'; dst[j++] = '\\'; break;
        case '\n': dst[j++] = '\\'; dst[j++] = 'n';  break;
        case '\r': dst[j++] = '\\'; dst[j++] = 'r';  break;
        case '\t': dst[j++] = '\\'; dst[j++] = 't';  break;
        default:
            if ((unsigned char)src[i] < 0x20) {
                j += snprintf(dst + j, dst_sz - j, "\\u%04x", (unsigned char)src[i]);
            } else {
                dst[j++] = src[i];
            }
            break;
        }
    }
    dst[j] = '\0';
}

static int json_get_string(const char *json, const char *key, char *dst, size_t dst_sz)
{
    char pattern[128];
    snprintf(pattern, sizeof(pattern), "\"%s\":\"", key);
    const char *p = strstr(json, pattern);
    if (!p) { dst[0] = '\0'; return -1; }
    p += strlen(pattern);
    size_t i = 0;
    while (*p && *p != '"' && i + 1 < dst_sz) {
        if (*p == '\\' && *(p + 1)) {
            p++;
            switch (*p) {
            case 'n':  dst[i++] = '\n'; break;
            case 'r':  dst[i++] = '\r'; break;
            case 't':  dst[i++] = '\t'; break;
            case '"':  dst[i++] = '"';  break;
            case '\\': dst[i++] = '\\'; break;
            default:   dst[i++] = *p;   break;
            }
            p++;
            continue;
        }
        dst[i++] = *p++;
    }
    dst[i] = '\0';
    return 0;
}

static int json_get_int(const char *json, const char *key)
{
    char pattern[128];
    snprintf(pattern, sizeof(pattern), "\"%s\":", key);
    const char *p = strstr(json, pattern);
    if (!p) return 0;
    p += strlen(pattern);
    return atoi(p);
}

/* Legacy serialization kept for backward compatibility */
int p2p_sig_message_to_json(const p2p_sig_message_t *msg, char *buf, size_t size)
{
    if (!msg || !buf || size == 0) return -1;

    char esc_sdp[P2P_SIG_MAX_SDP_SIZE * 2];
    char esc_cand[P2P_SIG_MAX_SDP_SIZE * 2];
    json_escape(msg->sdp, esc_sdp, sizeof(esc_sdp));
    json_escape(msg->candidate, esc_cand, sizeof(esc_cand));

    int n = snprintf(buf, size,
        "{\"type\":%d,"
        "\"from\":\"%s\","
        "\"to\":\"%s\","
        "\"room\":\"%s\","
        "\"sdp\":\"%s\","
        "\"candidate\":\"%s\","
        "\"turn_username\":\"%s\","
        "\"turn_password\":\"%s\","
        "\"turn_server\":\"%s\","
        "\"turn_port\":%u,"
        "\"turn_ttl\":%u}",
        (int)msg->type,
        msg->from_peer, msg->to_peer, msg->room_id,
        esc_sdp, esc_cand,
        msg->turn_username, msg->turn_password,
        msg->turn_server, (unsigned)msg->turn_port, (unsigned)msg->turn_ttl);
    return (n > 0 && (size_t)n < size) ? 0 : -1;
}

int p2p_sig_message_from_json(const char *json, size_t len, p2p_sig_message_t *msg)
{
    if (!json || !msg) return -1;
    (void)len;
    memset(msg, 0, sizeof(*msg));

    msg->type = (p2p_sig_msg_type_t)json_get_int(json, "type");
    json_get_string(json, "from", msg->from_peer, sizeof(msg->from_peer));
    json_get_string(json, "to", msg->to_peer, sizeof(msg->to_peer));
    json_get_string(json, "room", msg->room_id, sizeof(msg->room_id));
    json_get_string(json, "sdp", msg->sdp, sizeof(msg->sdp));
    json_get_string(json, "candidate", msg->candidate, sizeof(msg->candidate));
    json_get_string(json, "turn_username", msg->turn_username, sizeof(msg->turn_username));
    json_get_string(json, "turn_password", msg->turn_password, sizeof(msg->turn_password));
    json_get_string(json, "turn_server", msg->turn_server, sizeof(msg->turn_server));
    msg->turn_port = (uint16_t)json_get_int(json, "turn_port");
    msg->turn_ttl = (uint32_t)json_get_int(json, "turn_ttl");
    return 0;
}

/* ------------------------------------------------------------------ */
/*  HTTPS signaling transport                                         */
/* ------------------------------------------------------------------ */

/* Map string event type from SSE to internal message type enum */
static p2p_sig_msg_type_t map_event_type(const char *evt)
{
    if (strcmp(evt, "room_created")     == 0) return P2P_SIG_MSG_ROOM_INFO;
    if (strcmp(evt, "peer_joined")      == 0) return P2P_SIG_MSG_PEER_JOINED;
    if (strcmp(evt, "peer_left")        == 0) return P2P_SIG_MSG_PEER_LEFT;
    if (strcmp(evt, "ice_offer")        == 0) return P2P_SIG_MSG_ICE_OFFER;
    if (strcmp(evt, "ice_answer")       == 0) return P2P_SIG_MSG_ICE_ANSWER;
    if (strcmp(evt, "ice_candidate")    == 0) return P2P_SIG_MSG_ICE_CANDIDATE;
    if (strcmp(evt, "gathering_done")   == 0) return P2P_SIG_MSG_GATHERING_DONE;
    if (strcmp(evt, "turn_credentials") == 0) return P2P_SIG_MSG_TURN_CREDENTIALS;
    if (strcmp(evt, "full_offer")       == 0) return P2P_SIG_MSG_FULL_OFFER;
    if (strcmp(evt, "full_answer")      == 0) return P2P_SIG_MSG_FULL_ANSWER;
    if (strcmp(evt, "error")            == 0) return P2P_SIG_MSG_ERROR;
    return P2P_SIG_MSG_HEARTBEAT;
}

/* Parse candidates JSON array: ["cand1","cand2",...] */
static int parse_candidates_array(const char *json, const char *key,
                                  char out[][P2P_SIG_MAX_CAND_SIZE], int max_count)
{
    char pattern[128];
    snprintf(pattern, sizeof(pattern), "\"%s\":[", key);
    const char *p = strstr(json, pattern);
    if (!p) return 0;
    p += strlen(pattern);

    int count = 0;
    while (*p && *p != ']' && count < max_count) {
        while (*p == ' ' || *p == ',') p++;
        if (*p == '"') {
            p++;
            size_t i = 0;
            while (*p && *p != '"' && i + 1 < P2P_SIG_MAX_CAND_SIZE) {
                if (*p == '\\' && *(p+1)) {
                    p++;
                    switch (*p) {
                    case 'n':  out[count][i++] = '\n'; break;
                    case '\\': out[count][i++] = '\\'; break;
                    case '"':  out[count][i++] = '"';  break;
                    default:   out[count][i++] = *p;   break;
                    }
                    p++;
                } else {
                    out[count][i++] = *p++;
                }
            }
            out[count][i] = '\0';
            if (*p == '"') p++;
            count++;
        } else {
            break;
        }
    }
    return count;
}

/* Build a JSON POST body for signaling */
static int build_signal_json(p2p_signaling_client_t *c,
                             const char *type, const char *to_peer,
                             const char *room_id, const char *sdp,
                             const char **candidates, int cand_count,
                             char *buf, size_t buf_sz)
{
    char esc_sdp[P2P_SIG_MAX_SDP_SIZE * 2];
    if (sdp) json_escape(sdp, esc_sdp, sizeof(esc_sdp));
    else     esc_sdp[0] = '\0';

    int off = snprintf(buf, buf_sz,
        "{\"type\":\"%s\",\"peer_id\":\"%s\",\"token\":\"%s\"",
        type, c->peer_id, c->token);
    if (off < 0) return -1;

    if (to_peer && to_peer[0]) {
        off += snprintf(buf + off, buf_sz - off, ",\"to\":\"%s\"", to_peer);
    }
    if (room_id && room_id[0]) {
        off += snprintf(buf + off, buf_sz - off, ",\"room_id\":\"%s\"", room_id);
    }
    if (sdp && sdp[0]) {
        off += snprintf(buf + off, buf_sz - off, ",\"sdp\":\"%s\"", esc_sdp);
    }
    if (candidates && cand_count > 0) {
        off += snprintf(buf + off, buf_sz - off, ",\"candidates\":[");
        for (int i = 0; i < cand_count; i++) {
            char esc[P2P_SIG_MAX_CAND_SIZE * 2];
            json_escape(candidates[i], esc, sizeof(esc));
            off += snprintf(buf + off, buf_sz - off, "%s\"%s\"",
                            i > 0 ? "," : "", esc);
        }
        off += snprintf(buf + off, buf_sz - off, "]");
    }
    off += snprintf(buf + off, buf_sz - off, "}");
    return (off > 0 && (size_t)off < buf_sz) ? 0 : -1;
}

/* POST a signaling message and return the response body */
static int post_signal(p2p_signaling_client_t *c, const char *json_body,
                       char *resp, size_t resp_sz)
{
    p2p_mutex_lock(&c->post_mutex);
    char host_hdr[280];
    snprintf(host_hdr, sizeof(host_hdr), "%s:%u", c->server_host, c->server_port);

    int status = p2p_https_post(c->post_conn, host_hdr,
                                "/v1/signal", json_body,
                                resp, resp_sz);
    p2p_mutex_unlock(&c->post_mutex);
    return status;
}

/* Dispatch a full_offer or full_answer from SSE event data.
 * Triggers on_ice_offer/answer + on_ice_candidate*N + on_gathering_done. */
static void dispatch_full_ice(p2p_signaling_client_t *c,
                              p2p_sig_msg_type_t type,
                              const char *event_data)
{
    char from[P2P_SIG_MAX_PEER_ID] = {0};
    char sdp[P2P_SIG_MAX_SDP_SIZE] = {0};
    json_get_string(event_data, "from", from, sizeof(from));
    json_get_string(event_data, "sdp", sdp, sizeof(sdp));

    char cands[P2P_SIG_MAX_CANDIDATES][P2P_SIG_MAX_CAND_SIZE];
    memset(cands, 0, sizeof(cands));
    int ncand = parse_candidates_array(event_data, "candidates",
                                       cands, P2P_SIG_MAX_CANDIDATES);

    if (type == P2P_SIG_MSG_FULL_OFFER) {
        if (c->callbacks.on_ice_offer)
            c->callbacks.on_ice_offer(c, from, sdp, c->user_data);
    } else {
        if (c->callbacks.on_ice_answer)
            c->callbacks.on_ice_answer(c, from, sdp, c->user_data);
    }

    for (int i = 0; i < ncand; i++) {
        if (c->callbacks.on_ice_candidate)
            c->callbacks.on_ice_candidate(c, from, cands[i], c->user_data);
    }

    if (c->callbacks.on_gathering_done)
        c->callbacks.on_gathering_done(c, from, c->user_data);
}

/* Dispatch TURN credentials embedded in a join_room response */
static void dispatch_turn_from_response(p2p_signaling_client_t *c,
                                        const char *resp)
{
    /* Look for "turn" object in response */
    const char *tp = strstr(resp, "\"turn\":{");
    if (!tp) return;

    char username[128] = {0}, password[128] = {0}, server[256] = {0};
    uint16_t port = 0;
    uint32_t ttl = 0;

    json_get_string(tp, "username", username, sizeof(username));
    json_get_string(tp, "password", password, sizeof(password));
    json_get_string(tp, "server", server, sizeof(server));
    port = (uint16_t)json_get_int(tp, "port");
    ttl  = (uint32_t)json_get_int(tp, "ttl");

    if (username[0] && c->callbacks.on_turn_credentials)
        c->callbacks.on_turn_credentials(c, username, password,
                                         server, port, ttl, c->user_data);
}

/* SSE reader thread */
static void *sse_thread_func(void *arg)
{
    p2p_signaling_client_t *c = (p2p_signaling_client_t *)arg;
    char event_type[128];
    char event_data[P2P_SIG_MAX_MSG_SIZE];

    while (c->running) {
        if (p2p_sse_read_event(c->sse_conn,
                               event_type, sizeof(event_type),
                               event_data, sizeof(event_data)) != 0) {
            if (c->running && c->callbacks.on_disconnected)
                c->callbacks.on_disconnected(c, c->user_data);
            break;
        }

        p2p_sig_msg_type_t type = map_event_type(event_type);

        switch (type) {
        case P2P_SIG_MSG_ROOM_INFO:
        {
            char room[P2P_SIG_MAX_ROOM_ID] = {0};
            json_get_string(event_data, "room_id", room, sizeof(room));
            if (c->callbacks.on_room_created)
                c->callbacks.on_room_created(c, room, c->user_data);
            break;
        }
        case P2P_SIG_MSG_PEER_JOINED:
        {
            char peer[P2P_SIG_MAX_PEER_ID] = {0};
            json_get_string(event_data, "peer_id", peer, sizeof(peer));
            if (c->callbacks.on_peer_joined)
                c->callbacks.on_peer_joined(c, peer, c->user_data);
            break;
        }
        case P2P_SIG_MSG_PEER_LEFT:
        {
            char peer[P2P_SIG_MAX_PEER_ID] = {0};
            json_get_string(event_data, "peer_id", peer, sizeof(peer));
            if (c->callbacks.on_peer_left)
                c->callbacks.on_peer_left(c, peer, c->user_data);
            break;
        }
        case P2P_SIG_MSG_ICE_OFFER:
        {
            char from[P2P_SIG_MAX_PEER_ID] = {0};
            char sdp[P2P_SIG_MAX_SDP_SIZE] = {0};
            json_get_string(event_data, "from", from, sizeof(from));
            json_get_string(event_data, "sdp", sdp, sizeof(sdp));
            if (c->callbacks.on_ice_offer)
                c->callbacks.on_ice_offer(c, from, sdp, c->user_data);
            break;
        }
        case P2P_SIG_MSG_ICE_ANSWER:
        {
            char from[P2P_SIG_MAX_PEER_ID] = {0};
            char sdp[P2P_SIG_MAX_SDP_SIZE] = {0};
            json_get_string(event_data, "from", from, sizeof(from));
            json_get_string(event_data, "sdp", sdp, sizeof(sdp));
            if (c->callbacks.on_ice_answer)
                c->callbacks.on_ice_answer(c, from, sdp, c->user_data);
            break;
        }
        case P2P_SIG_MSG_ICE_CANDIDATE:
        {
            char from[P2P_SIG_MAX_PEER_ID] = {0};
            char cand[P2P_SIG_MAX_SDP_SIZE] = {0};
            json_get_string(event_data, "from", from, sizeof(from));
            json_get_string(event_data, "candidate", cand, sizeof(cand));
            if (c->callbacks.on_ice_candidate)
                c->callbacks.on_ice_candidate(c, from, cand, c->user_data);
            break;
        }
        case P2P_SIG_MSG_GATHERING_DONE:
        {
            char from[P2P_SIG_MAX_PEER_ID] = {0};
            json_get_string(event_data, "from", from, sizeof(from));
            if (c->callbacks.on_gathering_done)
                c->callbacks.on_gathering_done(c, from, c->user_data);
            break;
        }
        case P2P_SIG_MSG_TURN_CREDENTIALS:
        {
            char username[128] = {0}, password[128] = {0}, server[256] = {0};
            json_get_string(event_data, "username", username, sizeof(username));
            json_get_string(event_data, "password", password, sizeof(password));
            json_get_string(event_data, "server", server, sizeof(server));
            uint16_t port = (uint16_t)json_get_int(event_data, "port");
            uint32_t ttl  = (uint32_t)json_get_int(event_data, "ttl");
            if (c->callbacks.on_turn_credentials)
                c->callbacks.on_turn_credentials(c, username, password,
                                                  server, port, ttl, c->user_data);
            break;
        }
        case P2P_SIG_MSG_FULL_OFFER:
        case P2P_SIG_MSG_FULL_ANSWER:
            dispatch_full_ice(c, type, event_data);
            break;
        case P2P_SIG_MSG_ERROR:
        {
            char err[512] = {0};
            json_get_string(event_data, "error", err, sizeof(err));
            if (c->callbacks.on_error)
                c->callbacks.on_error(c, err, c->user_data);
            break;
        }
        default:
            break;
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/*  Public API                                                        */
/* ------------------------------------------------------------------ */

int p2p_signaling_connect(p2p_signaling_client_t *c, const p2p_signaling_config_t *config)
{
    if (!c || !config || !config->server_url || !config->peer_id) return -1;
    memset(c, 0, sizeof(*c));

    /* Parse host:port */
    c->server_port = 443;
    if (sscanf(config->server_url, "%255[^:]:%hu", c->server_host, &c->server_port) < 1) {
        fprintf(stderr, "[sig] invalid server_url: %s\n", config->server_url);
        return -1;
    }

    snprintf(c->peer_id, sizeof(c->peer_id), "%s", config->peer_id);
    if (config->token)
        snprintf(c->token, sizeof(c->token), "%s", config->token);
    c->callbacks = config->callbacks;
    c->user_data = config->user_data;
    p2p_mutex_init(&c->post_mutex);

    /* Create TLS context */
    c->tls_ctx = p2p_tls_ctx_create();
    if (!c->tls_ctx) {
        fprintf(stderr, "[sig] TLS context creation failed\n");
        return -1;
    }

    /* Open POST connection */
    c->post_conn = p2p_tls_connect(c->tls_ctx, c->server_host, c->server_port);
    if (!c->post_conn) {
        fprintf(stderr, "[sig] POST TLS connect to %s:%u failed\n",
                c->server_host, c->server_port);
        p2p_tls_ctx_destroy(c->tls_ctx);
        c->tls_ctx = NULL;
        return -1;
    }

    /* Open SSE connection */
    c->sse_conn = p2p_tls_connect(c->tls_ctx, c->server_host, c->server_port);
    if (!c->sse_conn) {
        fprintf(stderr, "[sig] SSE TLS connect to %s:%u failed\n",
                c->server_host, c->server_port);
        p2p_tls_close(c->post_conn);
        c->post_conn = NULL;
        p2p_tls_ctx_destroy(c->tls_ctx);
        c->tls_ctx = NULL;
        return -1;
    }

    /* Send SSE GET request */
    char sse_path[1024];
    snprintf(sse_path, sizeof(sse_path),
             "/v1/events?peer_id=%s&token=%s", c->peer_id, c->token);
    char host_hdr[280];
    snprintf(host_hdr, sizeof(host_hdr), "%s:%u", c->server_host, c->server_port);

    if (p2p_https_get_sse(c->sse_conn, host_hdr, sse_path) != 0) {
        fprintf(stderr, "[sig] SSE handshake failed\n");
        p2p_tls_close(c->sse_conn);   c->sse_conn  = NULL;
        p2p_tls_close(c->post_conn);  c->post_conn = NULL;
        p2p_tls_ctx_destroy(c->tls_ctx); c->tls_ctx = NULL;
        return -1;
    }

    c->connected = 1;
    c->running   = 1;

    /* Start SSE reader thread */
    if (p2p_thread_create(&c->sse_thread, sse_thread_func, c) != 0) {
        fprintf(stderr, "[sig] SSE thread creation failed\n");
        c->connected = 0;
        c->running   = 0;
        p2p_tls_close(c->sse_conn);   c->sse_conn  = NULL;
        p2p_tls_close(c->post_conn);  c->post_conn = NULL;
        p2p_tls_ctx_destroy(c->tls_ctx); c->tls_ctx = NULL;
        return -1;
    }

    if (c->callbacks.on_connected)
        c->callbacks.on_connected(c, c->user_data);

    return 0;
}

void p2p_signaling_disconnect(p2p_signaling_client_t *c)
{
    if (!c) return;
    c->running = 0;
    if (c->sse_conn) {
        p2p_tls_close(c->sse_conn);
        c->sse_conn = NULL;
    }
    if (c->connected) {
        p2p_thread_join(c->sse_thread);
    }
    if (c->post_conn) {
        p2p_tls_close(c->post_conn);
        c->post_conn = NULL;
    }
    if (c->tls_ctx) {
        p2p_tls_ctx_destroy(c->tls_ctx);
        c->tls_ctx = NULL;
    }
    p2p_mutex_destroy(&c->post_mutex);
    c->connected = 0;
}

int p2p_signaling_create_room(p2p_signaling_client_t *c, const char *room_id)
{
    if (!c || !c->connected) return -1;
    snprintf(c->room_id, sizeof(c->room_id), "%s", room_id);

    char body[P2P_SIG_MAX_MSG_SIZE];
    if (build_signal_json(c, "create_room", NULL, room_id,
                          NULL, NULL, 0, body, sizeof(body)) != 0)
        return -1;

    char resp[P2P_SIG_MAX_MSG_SIZE];
    int status = post_signal(c, body, resp, sizeof(resp));
    if (status < 200 || status >= 300) {
        fprintf(stderr, "[sig] create_room HTTP %d: %s\n", status, resp);
        return -1;
    }
    return 0;
}

int p2p_signaling_join_room(p2p_signaling_client_t *c, const char *room_id)
{
    if (!c || !c->connected) return -1;
    snprintf(c->room_id, sizeof(c->room_id), "%s", room_id);

    char body[P2P_SIG_MAX_MSG_SIZE];
    if (build_signal_json(c, "join_room", NULL, room_id,
                          NULL, NULL, 0, body, sizeof(body)) != 0)
        return -1;

    char resp[P2P_SIG_MAX_MSG_SIZE];
    int status = post_signal(c, body, resp, sizeof(resp));
    if (status < 200 || status >= 300) {
        fprintf(stderr, "[sig] join_room HTTP %d: %s\n", status, resp);
        return -1;
    }

    /* The join response may contain TURN credentials */
    dispatch_turn_from_response(c, resp);

    return 0;
}

int p2p_signaling_leave_room(p2p_signaling_client_t *c)
{
    if (!c || !c->connected) return -1;

    char body[P2P_SIG_MAX_MSG_SIZE];
    if (build_signal_json(c, "leave_room", NULL, c->room_id,
                          NULL, NULL, 0, body, sizeof(body)) != 0)
        return -1;

    char resp[P2P_SIG_MAX_MSG_SIZE];
    int status = post_signal(c, body, resp, sizeof(resp));
    c->room_id[0] = '\0';
    return (status >= 200 && status < 300) ? 0 : -1;
}

int p2p_signaling_send_ice_offer(p2p_signaling_client_t *c,
                                  const char *to_peer, const char *sdp)
{
    if (!c || !c->connected || !to_peer || !sdp) return -1;

    char body[P2P_SIG_MAX_MSG_SIZE];
    if (build_signal_json(c, "ice_offer", to_peer, c->room_id,
                          sdp, NULL, 0, body, sizeof(body)) != 0)
        return -1;

    char resp[P2P_SIG_MAX_MSG_SIZE];
    int status = post_signal(c, body, resp, sizeof(resp));
    return (status >= 200 && status < 300) ? 0 : -1;
}

int p2p_signaling_send_ice_answer(p2p_signaling_client_t *c,
                                   const char *to_peer, const char *sdp)
{
    if (!c || !c->connected || !to_peer || !sdp) return -1;

    char body[P2P_SIG_MAX_MSG_SIZE];
    if (build_signal_json(c, "ice_answer", to_peer, c->room_id,
                          sdp, NULL, 0, body, sizeof(body)) != 0)
        return -1;

    char resp[P2P_SIG_MAX_MSG_SIZE];
    int status = post_signal(c, body, resp, sizeof(resp));
    return (status >= 200 && status < 300) ? 0 : -1;
}

int p2p_signaling_send_ice_candidate(p2p_signaling_client_t *c,
                                      const char *to_peer, const char *candidate)
{
    if (!c || !c->connected || !to_peer || !candidate) return -1;

    char body[P2P_SIG_MAX_MSG_SIZE];
    char esc[P2P_SIG_MAX_SDP_SIZE * 2];
    json_escape(candidate, esc, sizeof(esc));

    int n = snprintf(body, sizeof(body),
        "{\"type\":\"ice_candidate\",\"peer_id\":\"%s\",\"token\":\"%s\""
        ",\"to\":\"%s\",\"room_id\":\"%s\",\"candidate\":\"%s\"}",
        c->peer_id, c->token, to_peer, c->room_id, esc);
    if (n <= 0 || (size_t)n >= sizeof(body)) return -1;

    char resp[P2P_SIG_MAX_MSG_SIZE];
    int status = post_signal(c, body, resp, sizeof(resp));
    return (status >= 200 && status < 300) ? 0 : -1;
}

int p2p_signaling_send_gathering_done(p2p_signaling_client_t *c, const char *to_peer)
{
    if (!c || !c->connected || !to_peer) return -1;

    char body[P2P_SIG_MAX_MSG_SIZE];
    if (build_signal_json(c, "gathering_done", to_peer, c->room_id,
                          NULL, NULL, 0, body, sizeof(body)) != 0)
        return -1;

    char resp[P2P_SIG_MAX_MSG_SIZE];
    int status = post_signal(c, body, resp, sizeof(resp));
    return (status >= 200 && status < 300) ? 0 : -1;
}

int p2p_signaling_send_full_offer(p2p_signaling_client_t *c,
                                   const char *to_peer, const char *sdp,
                                   const char **candidates, int count)
{
    if (!c || !c->connected || !to_peer || !sdp) return -1;

    char body[P2P_SIG_MAX_MSG_SIZE];
    if (build_signal_json(c, "full_offer", to_peer, c->room_id,
                          sdp, candidates, count, body, sizeof(body)) != 0)
        return -1;

    char resp[P2P_SIG_MAX_MSG_SIZE];
    int status = post_signal(c, body, resp, sizeof(resp));
    return (status >= 200 && status < 300) ? 0 : -1;
}

int p2p_signaling_send_full_answer(p2p_signaling_client_t *c,
                                    const char *to_peer, const char *sdp,
                                    const char **candidates, int count)
{
    if (!c || !c->connected || !to_peer || !sdp) return -1;

    char body[P2P_SIG_MAX_MSG_SIZE];
    if (build_signal_json(c, "full_answer", to_peer, c->room_id,
                          sdp, candidates, count, body, sizeof(body)) != 0)
        return -1;

    char resp[P2P_SIG_MAX_MSG_SIZE];
    int status = post_signal(c, body, resp, sizeof(resp));
    return (status >= 200 && status < 300) ? 0 : -1;
}
