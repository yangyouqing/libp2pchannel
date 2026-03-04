#ifndef P2P_ADAPTER_H
#define P2P_ADAPTER_H

#include <juice/juice.h>
#include <xquic/xquic.h>
#include <xquic/xqc_http3.h>

#include "p2p_packet_queue.h"

#include <stdint.h>
#include <stddef.h>
#include "p2p_platform.h"

#ifdef __cplusplus
extern "C" {
#endif

#define P2P_MAX_SUBSCRIBERS   10
#define P2P_VIRTUAL_PORT_BASE 20000
#define P2P_ICE_MTU           1200

/* Direct framing header for sending AV data via ICE channel */
#define P2P_FRAME_TYPE_VIDEO  0x01
#define P2P_FRAME_TYPE_AUDIO  0x02
#define P2P_FRAME_TYPE_IDR_REQ 0x03
#define P2P_FRAME_TYPE_DATA   0x04
#define P2P_FRAME_FLAG_KEY    0x01

typedef struct __attribute__((packed)) {
    uint8_t  type;           /* P2P_FRAME_TYPE_VIDEO or _AUDIO */
    uint8_t  flags;          /* P2P_FRAME_FLAG_KEY for keyframes */
    uint32_t seq;            /* sequence number */
    uint64_t timestamp_us;   /* capture timestamp */
    uint32_t total_len;      /* total payload size (may span multiple chunks) */
    uint32_t frag_offset;    /* fragment offset within total payload */
    uint16_t frag_len;       /* this fragment's payload size */
} p2p_frame_header_t;       /* 22 bytes, followed by frag_len bytes of payload */

#define P2P_FRAME_HDR_SIZE sizeof(p2p_frame_header_t)
#define P2P_FRAME_MAX_FRAG (P2P_ICE_MTU - P2P_FRAME_HDR_SIZE)

typedef enum {
    P2P_ROLE_PUBLISHER,
    P2P_ROLE_SUBSCRIBER,
} p2p_role_t;

typedef enum {
    P2P_PEER_STATE_IDLE = 0,
    P2P_PEER_STATE_ICE_GATHERING,
    P2P_PEER_STATE_ICE_CONNECTING,
    P2P_PEER_STATE_ICE_CONNECTED,
    P2P_PEER_STATE_QUIC_HANDSHAKING,
    P2P_PEER_STATE_QUIC_CONNECTED,
    P2P_PEER_STATE_FAILED,
    P2P_PEER_STATE_CLOSED,
} p2p_peer_state_t;

struct p2p_engine_s;

/*
 * Per-peer context.  On the Publisher side one of these exists for each
 * Subscriber; on the Subscriber side there is exactly one (for the Publisher).
 */
typedef struct p2p_peer_ctx_s {
    int                     index;
    struct p2p_engine_s    *engine;
    p2p_peer_state_t        state;

    /* libjuice ICE agent */
    juice_agent_t          *ice_agent;

    /* xquic connection */
    xqc_cid_t               cid;
    xqc_connection_t       *xqc_conn;

    /* Virtual addressing for xquic – each peer gets a unique virtual port */
    struct sockaddr_in      virtual_local_addr;
    struct sockaddr_in      virtual_peer_addr;

    /* Peer identity from signaling */
    char                    peer_id[64];

    void                   *user_data;
} p2p_peer_ctx_t;

/*
 * Callbacks that the application (Publisher / Subscriber framework) implements.
 */
typedef struct {
    void (*on_peer_ice_state)(p2p_peer_ctx_t *peer, juice_state_t state, void *user_data);
    void (*on_peer_ice_candidate)(p2p_peer_ctx_t *peer, const char *sdp, void *user_data);
    void (*on_peer_ice_gathering_done)(p2p_peer_ctx_t *peer, void *user_data);
    void (*on_peer_quic_connected)(p2p_peer_ctx_t *peer, void *user_data);
    void (*on_peer_quic_closed)(p2p_peer_ctx_t *peer, void *user_data);
    /* Called when framed AV data arrives via direct framing */
    void (*on_peer_data_recv)(p2p_peer_ctx_t *peer, const p2p_frame_header_t *hdr,
                              const uint8_t *payload, void *user_data);
} p2p_adapter_callbacks_t;

typedef struct {
    const char             *stun_server_host;
    uint16_t                stun_server_port;
    const char             *turn_server_host;
    uint16_t                turn_server_port;
    const char             *turn_username;
    const char             *turn_password;
    const char             *ssl_cert_file;
    const char             *ssl_key_file;
    p2p_role_t              role;
    p2p_adapter_callbacks_t callbacks;
    void                   *user_data;
} p2p_engine_config_t;

/*
 * Main engine – holds the xquic engine and an array of peer contexts.
 */
typedef struct p2p_engine_s {
    p2p_role_t              role;
    xqc_engine_t           *xqc_engine;

    p2p_peer_ctx_t          peers[P2P_MAX_SUBSCRIBERS];
    int                     peer_count;

    p2p_packet_queue_t      recv_queue;

    p2p_adapter_callbacks_t callbacks;
    void                   *user_data;

    /* Timer management */
    p2p_thread_t            timer_thread;
    int                     timer_running;
    uint64_t                next_wakeup_us;
    p2p_mutex_t             timer_mutex;
    p2p_cond_t              timer_cond;

    /* Engine thread processing */
    p2p_thread_t            engine_thread;
    int                     engine_running;

    /* STUN/TURN config passed to each new ICE agent */
    char                    stun_host[256];
    uint16_t                stun_port;
    char                    turn_host[256];
    uint16_t                turn_port;
    char                    turn_username[128];
    char                    turn_password[128];

    /* SSL config for xquic */
    char                    ssl_cert_file[512];
    char                    ssl_key_file[512];
} p2p_engine_t;

/* ---- Lifecycle ---- */
int  p2p_engine_init(p2p_engine_t *engine, const p2p_engine_config_t *config);
void p2p_engine_destroy(p2p_engine_t *engine);
int  p2p_engine_start(p2p_engine_t *engine);
void p2p_engine_stop(p2p_engine_t *engine);

/* ---- Peer management ---- */
p2p_peer_ctx_t *p2p_engine_add_peer(p2p_engine_t *engine, const char *peer_id);
void p2p_engine_remove_peer(p2p_engine_t *engine, int peer_index);
p2p_peer_ctx_t *p2p_engine_find_peer(p2p_engine_t *engine, const char *peer_id);

/* ---- ICE operations ---- */
int  p2p_peer_gather_candidates(p2p_peer_ctx_t *peer);
int  p2p_peer_set_remote_description(p2p_peer_ctx_t *peer, const char *sdp);
int  p2p_peer_add_remote_candidate(p2p_peer_ctx_t *peer, const char *sdp);
int  p2p_peer_set_remote_gathering_done(p2p_peer_ctx_t *peer);
int  p2p_peer_get_local_description(p2p_peer_ctx_t *peer, char *buf, size_t size);

/* ---- QUIC connection (called after ICE connected) ---- */
int  p2p_peer_start_quic(p2p_peer_ctx_t *peer);

/* ---- Direct framing mode ---- */

/* Send a framed AV packet through ICE. Fragments if payload > MTU. */
int  p2p_peer_send_data(p2p_peer_ctx_t *peer, uint8_t type, uint8_t flags,
                        uint32_t seq, uint64_t timestamp_us,
                        const uint8_t *payload, uint32_t payload_len);

/* Send an IDR (keyframe) request to remote peer. */
int  p2p_peer_send_idr_request(p2p_peer_ctx_t *peer);

/* ---- Utility ---- */
uint64_t p2p_now_us(void);

#ifdef __cplusplus
}
#endif

#endif
