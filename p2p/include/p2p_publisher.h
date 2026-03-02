#ifndef P2P_PUBLISHER_H
#define P2P_PUBLISHER_H

#include "p2p_adapter.h"
#include "p2p_signaling.h"

#ifdef __cplusplus
extern "C" {
#endif

#define P2P_PUB_MAX_IDR_CACHE_SIZE  (512 * 1024)   /* 512KB IDR frame cache */

typedef struct {
    const char     *signaling_url;
    const char     *room_id;
    const char     *peer_id;
    const char     *stun_host;
    uint16_t        stun_port;
    const char     *turn_host;
    uint16_t        turn_port;
    const char     *turn_username;
    const char     *turn_password;
    const char     *ssl_cert_file;
    const char     *ssl_key_file;
} p2p_publisher_config_t;

typedef struct {
    p2p_engine_t            engine;
    p2p_signaling_client_t  signaling;
    char                    room_id[P2P_SIG_MAX_ROOM_ID];

    /* IDR frame cache for fast first-frame delivery */
    uint8_t                *idr_cache;
    size_t                  idr_cache_size;
    uint64_t                idr_timestamp_us;
    p2p_mutex_t             idr_mutex;

    int                     running;
} p2p_publisher_t;

int  p2p_publisher_init(p2p_publisher_t *pub, const p2p_publisher_config_t *config);
void p2p_publisher_destroy(p2p_publisher_t *pub);

int  p2p_publisher_start(p2p_publisher_t *pub);
void p2p_publisher_stop(p2p_publisher_t *pub);

/*
 * Send a video frame to all connected subscribers.
 * If is_keyframe is true, the frame is also cached for fast first-frame delivery.
 */
int  p2p_publisher_send_video(p2p_publisher_t *pub, const uint8_t *data,
                               size_t size, uint64_t timestamp_us, int is_keyframe);

int  p2p_publisher_send_audio(p2p_publisher_t *pub, const uint8_t *data,
                               size_t size, uint64_t timestamp_us);

int  p2p_publisher_get_subscriber_count(p2p_publisher_t *pub);

#ifdef __cplusplus
}
#endif

#endif
