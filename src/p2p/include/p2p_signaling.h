#ifndef P2P_SIGNALING_H
#define P2P_SIGNALING_H

#include <stddef.h>
#include <stdint.h>
#include "p2p_platform.h"
#include "p2p_tls.h"

#ifdef __cplusplus
extern "C" {
#endif

#define P2P_SIG_MAX_MSG_SIZE    8192
#define P2P_SIG_POST_QUEUE_SIZE 32
#define P2P_SIG_MAX_PEER_ID     64
#define P2P_SIG_MAX_ROOM_ID     64
#define P2P_SIG_MAX_SDP_SIZE    4096
#define P2P_SIG_MAX_CANDIDATES  16
#define P2P_SIG_MAX_CAND_SIZE   256

/* Message types (kept for callback dispatch compatibility) */
typedef enum {
    P2P_SIG_MSG_CREATE_ROOM = 1,
    P2P_SIG_MSG_JOIN_ROOM,
    P2P_SIG_MSG_LEAVE_ROOM,
    P2P_SIG_MSG_ICE_OFFER,
    P2P_SIG_MSG_ICE_ANSWER,
    P2P_SIG_MSG_ICE_CANDIDATE,
    P2P_SIG_MSG_GATHERING_DONE,
    P2P_SIG_MSG_TURN_CREDENTIALS,
    P2P_SIG_MSG_ROOM_INFO,
    P2P_SIG_MSG_HEARTBEAT,
    P2P_SIG_MSG_ERROR,
    P2P_SIG_MSG_PEER_JOINED,
    P2P_SIG_MSG_PEER_LEFT,
    P2P_SIG_MSG_FULL_OFFER,
    P2P_SIG_MSG_FULL_ANSWER,
} p2p_sig_msg_type_t;

typedef struct {
    p2p_sig_msg_type_t  type;
    char                from_peer[P2P_SIG_MAX_PEER_ID];
    char                to_peer[P2P_SIG_MAX_PEER_ID];
    char                room_id[P2P_SIG_MAX_ROOM_ID];
    char                sdp[P2P_SIG_MAX_SDP_SIZE];
    char                candidate[P2P_SIG_MAX_SDP_SIZE];
    /* TURN credentials */
    char                turn_username[128];
    char                turn_password[128];
    char                turn_server[256];
    uint16_t            turn_port;
    uint32_t            turn_ttl;
    /* FullOffer / FullAnswer: batched candidates */
    char                candidates[P2P_SIG_MAX_CANDIDATES][P2P_SIG_MAX_CAND_SIZE];
    int                 candidate_count;
} p2p_sig_message_t;

typedef struct p2p_signaling_client_s p2p_signaling_client_t;

/* Callbacks from the signaling client to the application */
typedef struct {
    void (*on_connected)(p2p_signaling_client_t *client, void *user_data);
    void (*on_disconnected)(p2p_signaling_client_t *client, void *user_data);
    void (*on_room_created)(p2p_signaling_client_t *client, const char *room_id, void *user_data);
    void (*on_peer_joined)(p2p_signaling_client_t *client, const char *peer_id, void *user_data);
    void (*on_peer_left)(p2p_signaling_client_t *client, const char *peer_id, void *user_data);
    void (*on_ice_offer)(p2p_signaling_client_t *client, const char *from_peer,
                         const char *sdp, void *user_data);
    void (*on_ice_answer)(p2p_signaling_client_t *client, const char *from_peer,
                          const char *sdp, void *user_data);
    void (*on_ice_candidate)(p2p_signaling_client_t *client, const char *from_peer,
                             const char *candidate, void *user_data);
    void (*on_gathering_done)(p2p_signaling_client_t *client, const char *from_peer,
                              void *user_data);
    void (*on_turn_credentials)(p2p_signaling_client_t *client,
                                const char *username, const char *password,
                                const char *server, uint16_t port,
                                uint32_t ttl, void *user_data);
    void (*on_error)(p2p_signaling_client_t *client, const char *error, void *user_data);
} p2p_signaling_callbacks_t;

typedef struct {
    const char                 *server_url;    /* host:port (HTTPS) */
    const char                 *peer_id;
    const char                 *token;         /* JWT auth token */
    p2p_signaling_callbacks_t   callbacks;
    void                       *user_data;
} p2p_signaling_config_t;

struct p2p_signaling_client_s {
    char                        server_host[256];
    uint16_t                    server_port;
    char                        peer_id[P2P_SIG_MAX_PEER_ID];
    char                        token[1024];
    char                        room_id[P2P_SIG_MAX_ROOM_ID];
    p2p_signaling_callbacks_t   callbacks;
    void                       *user_data;
    int                         connected;
    int                         running;

    /* TLS connections: one for SSE, one for POST */
    p2p_tls_ctx_t              *tls_ctx;
    p2p_tls_conn_t             *sse_conn;
    p2p_tls_conn_t             *post_conn;
    p2p_mutex_t                 post_mutex;   /* serialize POST requests */
    p2p_thread_t                sse_thread;

    /* Async POST queue (fire-and-forget for ICE signaling) */
    char                        post_queue[P2P_SIG_POST_QUEUE_SIZE][P2P_SIG_MAX_MSG_SIZE];
    int                         post_queue_head;
    int                         post_queue_tail;
    p2p_mutex_t                 post_queue_mutex;
    p2p_cond_t                  post_queue_cond;
    p2p_thread_t                post_worker_thread;
    int                         post_worker_running;
};

int  p2p_signaling_connect(p2p_signaling_client_t *client, const p2p_signaling_config_t *config);
void p2p_signaling_disconnect(p2p_signaling_client_t *client);

int  p2p_signaling_create_room(p2p_signaling_client_t *client, const char *room_id);
int  p2p_signaling_join_room(p2p_signaling_client_t *client, const char *room_id);
int  p2p_signaling_leave_room(p2p_signaling_client_t *client);

int  p2p_signaling_send_ice_offer(p2p_signaling_client_t *client,
                                   const char *to_peer, const char *sdp);
int  p2p_signaling_send_ice_answer(p2p_signaling_client_t *client,
                                    const char *to_peer, const char *sdp);
int  p2p_signaling_send_ice_candidate(p2p_signaling_client_t *client,
                                       const char *to_peer, const char *candidate);
int  p2p_signaling_send_gathering_done(p2p_signaling_client_t *client, const char *to_peer);

/* Batched offer/answer: SDP + all candidates in one POST */
int  p2p_signaling_send_full_offer(p2p_signaling_client_t *client,
                                    const char *to_peer, const char *sdp,
                                    const char **candidates, int count);
int  p2p_signaling_send_full_answer(p2p_signaling_client_t *client,
                                     const char *to_peer, const char *sdp,
                                     const char **candidates, int count);

/* Async (fire-and-forget) variants for ICE signaling.
 * These enqueue the POST and return immediately without waiting for the
 * HTTP response, so callers (SSE thread, libjuice thread) are never blocked. */
int  p2p_signaling_send_ice_offer_async(p2p_signaling_client_t *client,
                                         const char *to_peer, const char *sdp);
int  p2p_signaling_send_ice_answer_async(p2p_signaling_client_t *client,
                                          const char *to_peer, const char *sdp);
int  p2p_signaling_send_ice_candidate_async(p2p_signaling_client_t *client,
                                              const char *to_peer, const char *candidate);
int  p2p_signaling_send_gathering_done_async(p2p_signaling_client_t *client,
                                               const char *to_peer);

/* JSON serialization helpers (kept for compatibility) */
int  p2p_sig_message_to_json(const p2p_sig_message_t *msg, char *buf, size_t size);
int  p2p_sig_message_from_json(const char *json, size_t len, p2p_sig_message_t *msg);

#ifdef __cplusplus
}
#endif

#endif
