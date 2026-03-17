#ifndef P2P_ADAPTER_H
#define P2P_ADAPTER_H

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
#define P2P_SEND_QUEUE_CAP    128
#define P2P_VIDEO_JITTER_SIZE 8

/* Framing header for AV data sent via xquic DATAGRAM */
#define P2P_FRAME_TYPE_VIDEO  0x01
#define P2P_FRAME_TYPE_AUDIO  0x02
#define P2P_FRAME_TYPE_IDR_REQ 0x03
#define P2P_FRAME_TYPE_DATA   0x04
#define P2P_FRAME_TYPE_VIDEO_STOP   0x05
#define P2P_FRAME_TYPE_VIDEO_START  0x06
#define P2P_FRAME_TYPE_AUDIO_STOP   0x07
#define P2P_FRAME_TYPE_AUDIO_START  0x08
#define P2P_FRAME_TYPE_PING         0x09
#define P2P_FRAME_FLAG_KEY    0x01

typedef struct __attribute__((packed)) {
    uint8_t  type;
    uint8_t  flags;
    uint32_t seq;
    uint64_t timestamp_us;
    uint32_t total_len;
    uint32_t frag_offset;
    uint16_t frag_len;
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

/* Abstracted ICE state (hides libjuice types from consumers) */
typedef enum {
    P2P_ICE_STATE_GATHERING,
    P2P_ICE_STATE_CONNECTING,
    P2P_ICE_STATE_CONNECTED,
    P2P_ICE_STATE_COMPLETED,
    P2P_ICE_STATE_FAILED,
    P2P_ICE_STATE_DISCONNECTED,
} p2p_ice_state_t;

struct p2p_engine_s;

/* Per-peer datagram send queue (for pacing via xquic) */
typedef struct {
    uint8_t  data[P2P_ICE_MTU];
    uint16_t len;
    int      qos;
} p2p_dgram_entry_t;

typedef struct {
    p2p_dgram_entry_t entries[P2P_SEND_QUEUE_CAP];
    int head, tail, count;
    p2p_mutex_t mutex;
} p2p_dgram_send_queue_t;

/*
 * Per-peer context.  On the Publisher side one of these exists for each
 * Subscriber; on the Subscriber side there is exactly one (for the Publisher).
 */
typedef struct p2p_peer_ctx_s {
    int                     index;
    struct p2p_engine_s    *engine;
    p2p_peer_state_t        state;

    /* ICE agent (opaque -- internally juice_agent_t*) */
    void                   *ice_agent;

    /* xquic connection */
    xqc_cid_t               cid;
    xqc_connection_t       *xqc_conn;

    /* xquic reliable control stream */
    xqc_stream_t           *ctrl_stream;

    /* Virtual addressing for xquic */
    struct sockaddr_in      virtual_local_addr;
    struct sockaddr_in      virtual_peer_addr;

    /* Peer identity from signaling */
    char                    peer_id[64];

    void                   *user_data;

    int                     offer_pending;
    int                     answer_pending;

    /* Per-peer AV pause flags (set by subscriber control messages) */
    volatile int            video_paused;
    volatile int            audio_paused;

    /* Set by ICE callback when ICE connects; cleared by engine thread */
    volatile int            needs_continue_send;

    /* Set when QUIC is closed by idle timeout; engine thread will reconnect */
    volatile int            needs_quic_restart;

    /* Set when both QUIC and ICE are dead; app must re-negotiate via signaling */
    volatile int            needs_ice_restart;

    /* Set by callback to defer peer removal to engine thread's safe point */
    volatile int            needs_removal;

    /* Consecutive QUIC reconnect failures (escalates to ICE restart) */
    int                     quic_restart_fail_count;

    /* Stale CID to close (deferred from callback to engine thread) */
    xqc_cid_t               stale_cid;
    volatile int            needs_stale_close;

    /* Timestamp of last received ICE packet (microseconds) for manual idle check */
    volatile uint64_t       last_ice_recv_us;

    /* Datagram send queue for pacing */
    p2p_dgram_send_queue_t  send_queue;
} p2p_peer_ctx_t;

/*
 * Callbacks that the application (Publisher / Subscriber framework) implements.
 */
typedef struct {
    void (*on_peer_ice_state)(p2p_peer_ctx_t *peer, p2p_ice_state_t state, void *user_data);
    void (*on_peer_ice_candidate)(p2p_peer_ctx_t *peer, const char *sdp, void *user_data);
    void (*on_peer_ice_gathering_done)(p2p_peer_ctx_t *peer, void *user_data);
    void (*on_peer_quic_connected)(p2p_peer_ctx_t *peer, void *user_data);
    void (*on_peer_quic_closed)(p2p_peer_ctx_t *peer, void *user_data);
    void (*on_peer_data_recv)(p2p_peer_ctx_t *peer, const p2p_frame_header_t *hdr,
                              const uint8_t *payload, void *user_data);
    void (*on_peer_ice_restart_needed)(p2p_peer_ctx_t *peer, void *user_data);
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
    int                     enable_tcp;
    p2p_role_t              role;
    p2p_adapter_callbacks_t callbacks;
    void                   *user_data;
} p2p_engine_config_t;

/*
 * Main engine -- holds the xquic engine and an array of peer contexts.
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

    /* ICE-TCP */
    int                     enable_tcp;
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

/* ---- ICE info query (hides libjuice from callers) ---- */
typedef struct {
    char local_cand[256];
    char remote_cand[256];
    char local_addr[64];
    char remote_addr[64];
} p2p_ice_info_t;

int  p2p_get_peer_ice_info(p2p_peer_ctx_t *peer, p2p_ice_info_t *info);

/* ---- QUIC connection (called after ICE connected) ---- */
int  p2p_peer_start_quic(p2p_peer_ctx_t *peer);

/* ---- AV data sending via xquic DATAGRAM (congestion controlled + paced) ---- */

int  p2p_peer_send_data_via_quic(p2p_peer_ctx_t *peer, uint8_t type, uint8_t flags,
                                 uint32_t seq, uint64_t timestamp_us,
                                 const uint8_t *payload, uint32_t payload_len);

int  p2p_peer_flush_send_queue(p2p_peer_ctx_t *peer);

/* ---- Legacy direct framing (bypasses xquic, no congestion control) ---- */

int  p2p_peer_send_data(p2p_peer_ctx_t *peer, uint8_t type, uint8_t flags,
                        uint32_t seq, uint64_t timestamp_us,
                        const uint8_t *payload, uint32_t payload_len);

/* ---- Control messages via xquic reliable STREAM ---- */

int  p2p_peer_send_idr_request(p2p_peer_ctx_t *peer);
int  p2p_peer_send_video_stop(p2p_peer_ctx_t *peer);
int  p2p_peer_send_video_start(p2p_peer_ctx_t *peer);
int  p2p_peer_send_audio_stop(p2p_peer_ctx_t *peer);
int  p2p_peer_send_audio_start(p2p_peer_ctx_t *peer);
int  p2p_peer_send_ping(p2p_peer_ctx_t *peer);

/* ---- Video Jitter Buffer (used by receiver) ---- */

typedef struct {
    uint8_t *y, *u, *v;
    int lsy, lsu, lsv;
    int w, h;
    uint64_t timestamp_us;
    int valid;
} p2p_jitter_frame_t;

typedef struct {
    p2p_jitter_frame_t frames[P2P_VIDEO_JITTER_SIZE];
    int write_idx;
    int read_idx;
    int count;
    uint64_t target_delay_us;
    p2p_mutex_t mutex;
} p2p_video_jitter_buf_t;

void p2p_video_jitter_init(p2p_video_jitter_buf_t *jb, uint64_t target_delay_us);
void p2p_video_jitter_destroy(p2p_video_jitter_buf_t *jb);
void p2p_video_jitter_push(p2p_video_jitter_buf_t *jb,
                           uint8_t *y, uint8_t *u, uint8_t *v,
                           int lsy, int lsu, int lsv, int w, int h,
                           uint64_t timestamp_us);
int  p2p_video_jitter_pop(p2p_video_jitter_buf_t *jb, p2p_jitter_frame_t *out);

/* ---- Logging (hides libjuice log level from callers) ---- */
void p2p_adapter_set_log_level(int level);

/* ---- Utility ---- */
uint64_t p2p_now_us(void);

#ifdef __cplusplus
}
#endif

#endif
