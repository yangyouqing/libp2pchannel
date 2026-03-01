#include "p2p_signaling.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>

/*
 * Minimal JSON serialization / deserialization for signaling messages.
 * Production code should use a proper JSON library (cJSON, etc.); this
 * implementation keeps external dependencies to zero.
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

/* Minimal JSON field extractor – finds "key":value and copies value to dst. */
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

int p2p_sig_message_from_json(const char *json, size_t len, p2p_sig_message_t *msg)
{
    if (!json || !msg) return -1;
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

/*
 * Simple TCP-based signaling transport.  Messages are length-prefixed:
 *   [4 bytes big-endian length][JSON payload]
 * A production version would use a real WebSocket library.
 */

static int tcp_connect(const char *host, uint16_t port)
{
    struct addrinfo hints, *res, *rp;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%u", port);

    if (getaddrinfo(host, port_str, &hints, &res) != 0)
        return -1;

    int fd = -1;
    for (rp = res; rp; rp = rp->ai_next) {
        fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd < 0) continue;
        if (connect(fd, rp->ai_addr, rp->ai_addrlen) == 0) break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    return fd;
}

static int send_frame(int fd, const char *data, size_t len)
{
    uint32_t nlen = htonl((uint32_t)len);
    if (write(fd, &nlen, 4) != 4) return -1;
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = write(fd, data + sent, len - sent);
        if (n <= 0) return -1;
        sent += n;
    }
    return 0;
}

static int recv_frame(int fd, char *buf, size_t buf_sz, size_t *out_len)
{
    uint32_t nlen;
    ssize_t n = read(fd, &nlen, 4);
    if (n != 4) return -1;
    uint32_t len = ntohl(nlen);
    if (len == 0 || len >= buf_sz) return -1;
    size_t got = 0;
    while (got < len) {
        n = read(fd, buf + got, len - got);
        if (n <= 0) return -1;
        got += n;
    }
    buf[len] = '\0';
    if (out_len) *out_len = len;
    return 0;
}

static int send_message(p2p_signaling_client_t *c, const p2p_sig_message_t *msg)
{
    char json[P2P_SIG_MAX_MSG_SIZE];
    if (p2p_sig_message_to_json(msg, json, sizeof(json)) != 0) return -1;
    return send_frame(c->ws_fd, json, strlen(json));
}

static void *recv_thread_func(void *arg)
{
    p2p_signaling_client_t *c = (p2p_signaling_client_t *)arg;
    char buf[P2P_SIG_MAX_MSG_SIZE];

    while (c->running) {
        size_t len;
        if (recv_frame(c->ws_fd, buf, sizeof(buf), &len) != 0) {
            if (c->running && c->callbacks.on_disconnected)
                c->callbacks.on_disconnected(c, c->user_data);
            break;
        }

        p2p_sig_message_t msg;
        if (p2p_sig_message_from_json(buf, len, &msg) != 0) continue;

        switch (msg.type) {
        case P2P_SIG_MSG_ROOM_INFO:
            if (c->callbacks.on_room_created)
                c->callbacks.on_room_created(c, msg.room_id, c->user_data);
            break;
        case P2P_SIG_MSG_PEER_JOINED:
            if (c->callbacks.on_peer_joined)
                c->callbacks.on_peer_joined(c, msg.from_peer, c->user_data);
            break;
        case P2P_SIG_MSG_PEER_LEFT:
            if (c->callbacks.on_peer_left)
                c->callbacks.on_peer_left(c, msg.from_peer, c->user_data);
            break;
        case P2P_SIG_MSG_ICE_OFFER:
            if (c->callbacks.on_ice_offer)
                c->callbacks.on_ice_offer(c, msg.from_peer, msg.sdp, c->user_data);
            break;
        case P2P_SIG_MSG_ICE_ANSWER:
            if (c->callbacks.on_ice_answer)
                c->callbacks.on_ice_answer(c, msg.from_peer, msg.sdp, c->user_data);
            break;
        case P2P_SIG_MSG_ICE_CANDIDATE:
            if (c->callbacks.on_ice_candidate)
                c->callbacks.on_ice_candidate(c, msg.from_peer, msg.candidate, c->user_data);
            break;
        case P2P_SIG_MSG_GATHERING_DONE:
            if (c->callbacks.on_gathering_done)
                c->callbacks.on_gathering_done(c, msg.from_peer, c->user_data);
            break;
        case P2P_SIG_MSG_TURN_CREDENTIALS:
            if (c->callbacks.on_turn_credentials)
                c->callbacks.on_turn_credentials(c, msg.turn_username, msg.turn_password,
                    msg.turn_server, msg.turn_port, msg.turn_ttl, c->user_data);
            break;
        case P2P_SIG_MSG_ERROR:
            if (c->callbacks.on_error)
                c->callbacks.on_error(c, msg.sdp, c->user_data);
            break;
        default:
            break;
        }
    }
    return NULL;
}

static void *heartbeat_thread_func(void *arg)
{
    p2p_signaling_client_t *c = (p2p_signaling_client_t *)arg;
    while (c->running) {
        sleep(15);
        if (!c->running || !c->connected) break;
        p2p_sig_message_t msg;
        memset(&msg, 0, sizeof(msg));
        msg.type = P2P_SIG_MSG_HEARTBEAT;
        snprintf(msg.from_peer, sizeof(msg.from_peer), "%s", c->peer_id);
        send_message(c, &msg);
    }
    return NULL;
}

int p2p_signaling_connect(p2p_signaling_client_t *c, const p2p_signaling_config_t *config)
{
    if (!c || !config || !config->server_url || !config->peer_id) return -1;
    memset(c, 0, sizeof(*c));

    snprintf(c->server_url, sizeof(c->server_url), "%s", config->server_url);
    snprintf(c->peer_id, sizeof(c->peer_id), "%s", config->peer_id);
    c->callbacks = config->callbacks;
    c->user_data = config->user_data;

    /* Parse host:port from URL (format: host:port) */
    char host[256];
    uint16_t port = 8080;
    if (sscanf(config->server_url, "%255[^:]:%hu", host, &port) < 1) {
        fprintf(stderr, "signaling: invalid server_url: %s\n", config->server_url);
        return -1;
    }

    c->ws_fd = tcp_connect(host, port);
    if (c->ws_fd < 0) {
        fprintf(stderr, "signaling: connect to %s:%u failed\n", host, port);
        return -1;
    }

    c->connected = 1;
    c->running = 1;

    if (pthread_create(&c->recv_thread, NULL, recv_thread_func, c) != 0) {
        close(c->ws_fd);
        c->ws_fd = -1;
        c->connected = 0;
        return -1;
    }

    pthread_create(&c->heartbeat_thread, NULL, heartbeat_thread_func, c);

    if (c->callbacks.on_connected)
        c->callbacks.on_connected(c, c->user_data);

    return 0;
}

void p2p_signaling_disconnect(p2p_signaling_client_t *c)
{
    if (!c) return;
    c->running = 0;
    if (c->ws_fd >= 0) {
        shutdown(c->ws_fd, SHUT_RDWR);
        close(c->ws_fd);
        c->ws_fd = -1;
    }
    if (c->connected) {
        pthread_join(c->recv_thread, NULL);
        pthread_join(c->heartbeat_thread, NULL);
    }
    c->connected = 0;
}

int p2p_signaling_create_room(p2p_signaling_client_t *c, const char *room_id)
{
    if (!c || !c->connected) return -1;
    snprintf(c->room_id, sizeof(c->room_id), "%s", room_id);

    p2p_sig_message_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.type = P2P_SIG_MSG_CREATE_ROOM;
    snprintf(msg.from_peer, sizeof(msg.from_peer), "%s", c->peer_id);
    snprintf(msg.room_id, sizeof(msg.room_id), "%s", room_id);
    return send_message(c, &msg);
}

int p2p_signaling_join_room(p2p_signaling_client_t *c, const char *room_id)
{
    if (!c || !c->connected) return -1;
    snprintf(c->room_id, sizeof(c->room_id), "%s", room_id);

    p2p_sig_message_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.type = P2P_SIG_MSG_JOIN_ROOM;
    snprintf(msg.from_peer, sizeof(msg.from_peer), "%s", c->peer_id);
    snprintf(msg.room_id, sizeof(msg.room_id), "%s", room_id);
    return send_message(c, &msg);
}

int p2p_signaling_leave_room(p2p_signaling_client_t *c)
{
    if (!c || !c->connected) return -1;

    p2p_sig_message_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.type = P2P_SIG_MSG_LEAVE_ROOM;
    snprintf(msg.from_peer, sizeof(msg.from_peer), "%s", c->peer_id);
    snprintf(msg.room_id, sizeof(msg.room_id), "%s", c->room_id);
    return send_message(c, &msg);
}

int p2p_signaling_send_ice_offer(p2p_signaling_client_t *c,
                                  const char *to_peer, const char *sdp)
{
    if (!c || !c->connected || !to_peer || !sdp) return -1;

    p2p_sig_message_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.type = P2P_SIG_MSG_ICE_OFFER;
    snprintf(msg.from_peer, sizeof(msg.from_peer), "%s", c->peer_id);
    snprintf(msg.to_peer, sizeof(msg.to_peer), "%s", to_peer);
    snprintf(msg.room_id, sizeof(msg.room_id), "%s", c->room_id);
    snprintf(msg.sdp, sizeof(msg.sdp), "%s", sdp);
    return send_message(c, &msg);
}

int p2p_signaling_send_ice_answer(p2p_signaling_client_t *c,
                                   const char *to_peer, const char *sdp)
{
    if (!c || !c->connected || !to_peer || !sdp) return -1;

    p2p_sig_message_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.type = P2P_SIG_MSG_ICE_ANSWER;
    snprintf(msg.from_peer, sizeof(msg.from_peer), "%s", c->peer_id);
    snprintf(msg.to_peer, sizeof(msg.to_peer), "%s", to_peer);
    snprintf(msg.room_id, sizeof(msg.room_id), "%s", c->room_id);
    snprintf(msg.sdp, sizeof(msg.sdp), "%s", sdp);
    return send_message(c, &msg);
}

int p2p_signaling_send_ice_candidate(p2p_signaling_client_t *c,
                                      const char *to_peer, const char *candidate)
{
    if (!c || !c->connected || !to_peer || !candidate) return -1;

    p2p_sig_message_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.type = P2P_SIG_MSG_ICE_CANDIDATE;
    snprintf(msg.from_peer, sizeof(msg.from_peer), "%s", c->peer_id);
    snprintf(msg.to_peer, sizeof(msg.to_peer), "%s", to_peer);
    snprintf(msg.room_id, sizeof(msg.room_id), "%s", c->room_id);
    snprintf(msg.candidate, sizeof(msg.candidate), "%s", candidate);
    return send_message(c, &msg);
}

int p2p_signaling_send_gathering_done(p2p_signaling_client_t *c, const char *to_peer)
{
    if (!c || !c->connected || !to_peer) return -1;

    p2p_sig_message_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.type = P2P_SIG_MSG_GATHERING_DONE;
    snprintf(msg.from_peer, sizeof(msg.from_peer), "%s", c->peer_id);
    snprintf(msg.to_peer, sizeof(msg.to_peer), "%s", to_peer);
    snprintf(msg.room_id, sizeof(msg.room_id), "%s", c->room_id);
    return send_message(c, &msg);
}
