#ifndef P2P_SUBSCRIBER_H
#define P2P_SUBSCRIBER_H

#include "p2p_adapter.h"
#include "p2p_signaling.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*p2p_on_video_frame_t)(const uint8_t *data, size_t size,
                                      uint64_t timestamp_us, int is_keyframe,
                                      void *user_data);
typedef void (*p2p_on_audio_frame_t)(const uint8_t *data, size_t size,
                                      uint64_t timestamp_us, void *user_data);

typedef struct {
    const char             *signaling_url;
    const char             *room_id;
    const char             *peer_id;
    const char             *stun_host;
    uint16_t                stun_port;
    const char             *turn_host;
    uint16_t                turn_port;
    const char             *turn_username;
    const char             *turn_password;
    p2p_on_video_frame_t    on_video;
    p2p_on_audio_frame_t    on_audio;
    void                   *user_data;
} p2p_subscriber_config_t;

typedef struct {
    p2p_engine_t            engine;
    p2p_signaling_client_t  signaling;
    char                    room_id[P2P_SIG_MAX_ROOM_ID];
    char                    publisher_peer_id[P2P_SIG_MAX_PEER_ID];

    p2p_on_video_frame_t    on_video;
    p2p_on_audio_frame_t    on_audio;
    void                   *app_user_data;

    int                     running;
} p2p_subscriber_t;

int  p2p_subscriber_init(p2p_subscriber_t *sub, const p2p_subscriber_config_t *config);
void p2p_subscriber_destroy(p2p_subscriber_t *sub);

int  p2p_subscriber_start(p2p_subscriber_t *sub);
void p2p_subscriber_stop(p2p_subscriber_t *sub);

#ifdef __cplusplus
}
#endif

#endif
