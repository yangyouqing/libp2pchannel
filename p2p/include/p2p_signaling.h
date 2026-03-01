#ifndef P2P_SIGNALING_H
#define P2P_SIGNALING_H

#include <stddef.h>
#include <stdint.h>
#include <pthread.h>

#ifdef __cplusplus
extern "C" {
#endif

#define P2P_SIG_MAX_MSG_SIZE    8192
#define P2P_SIG_MAX_PEER_ID     64
#define P2P_SIG_MAX_ROOM_ID     64
#define P2P_SIG_MAX_SDP_SIZE    4096

/* Message types matching the Go signaling server protocol */
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
    const char                 *server_url;    /* ws://host:port/ws */
    const char                 *peer_id;
    p2p_signaling_callbacks_t   callbacks;
    void                       *user_data;
} p2p_signaling_config_t;

/* Opaque signaling client (implementation uses a WebSocket library) */
struct p2p_signaling_client_s {
    char                        server_url[512];
    char                        peer_id[P2P_SIG_MAX_PEER_ID];
    char                        room_id[P2P_SIG_MAX_ROOM_ID];
    p2p_signaling_callbacks_t   callbacks;
    void                       *user_data;
    int                         connected;
    int                         ws_fd;
    pthread_t                   recv_thread;
    pthread_t                   heartbeat_thread;
    int                         running;
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

/* JSON serialization helpers */
int  p2p_sig_message_to_json(const p2p_sig_message_t *msg, char *buf, size_t size);
int  p2p_sig_message_from_json(const char *json, size_t len, p2p_sig_message_t *msg);

#ifdef __cplusplus
}
#endif

#endif
