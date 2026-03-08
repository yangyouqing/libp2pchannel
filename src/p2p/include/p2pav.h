/*
 * p2pav.h -- libp2pav: Pure P2P Audio/Video Transport Library
 *
 * Wraps xquic-over-libjuice with integrated signaling into a single
 * opaque API.  Users send/receive encoded frames; the library handles
 * ICE, QUIC, signaling, fragmentation, reassembly, and diagnostics.
 *
 * API categories:
 *   Session  -- lifecycle, connection, peer events
 *   Video    -- send/recv encoded video frames, mute, keyframe, bitrate
 *   Audio    -- send/recv encoded audio frames, mute
 *   Data     -- generic reliable/unreliable binary channels
 *   Diag     -- network stats, timing, ICE candidates, logging
 */

#ifndef P2PAV_H
#define P2PAV_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ==================================================================
 *  Shared library export/import
 * ================================================================== */

#if defined(_WIN32) || defined(__CYGWIN__)
#  ifdef P2PAV_BUILDING_DLL
#    define P2PAV_API __declspec(dllexport)
#  elif defined(P2PAV_DLL)
#    define P2PAV_API __declspec(dllimport)
#  else
#    define P2PAV_API
#  endif
#elif defined(__GNUC__) && __GNUC__ >= 4
#  define P2PAV_API __attribute__((visibility("default")))
#else
#  define P2PAV_API
#endif

/* ==================================================================
 *  Version
 * ================================================================== */

#define P2PAV_VERSION_MAJOR  1
#define P2PAV_VERSION_MINOR  0
#define P2PAV_VERSION_PATCH  0

P2PAV_API const char *p2pav_version_string(void);
P2PAV_API int         p2pav_version_number(void);   /* 0xMMmmpp */

/* ==================================================================
 *  Error codes
 * ================================================================== */

typedef enum {
    P2PAV_OK = 0,
    P2PAV_ERR_INVALID_PARAM,
    P2PAV_ERR_NOT_CONNECTED,
    P2PAV_ERR_ROOM_FULL,
    P2PAV_ERR_AUTH_FAILED,
    P2PAV_ERR_ICE_FAILED,
    P2PAV_ERR_QUIC_FAILED,
    P2PAV_ERR_CODEC_FAILED,
    P2PAV_ERR_TIMEOUT,
    P2PAV_ERR_PEER_NOT_FOUND,
    P2PAV_ERR_ALLOC_FAILED,
    P2PAV_ERR_CHANNEL_FULL,
    P2PAV_ERR_INTERNAL,
} p2pav_error_t;

P2PAV_API const char *p2pav_error_string(p2pav_error_t err);

/* ==================================================================
 *  Logging
 * ================================================================== */

typedef enum {
    P2PAV_LOG_NONE = 0,
    P2PAV_LOG_ERROR,
    P2PAV_LOG_WARN,
    P2PAV_LOG_INFO,
    P2PAV_LOG_DEBUG,
    P2PAV_LOG_TRACE,
} p2pav_log_level_t;

typedef void (*p2pav_log_cb_t)(p2pav_log_level_t level,
                                const char *tag,
                                const char *msg,
                                void *user_data);

P2PAV_API void p2pav_set_log_level(p2pav_log_level_t level);
P2PAV_API void p2pav_set_log_callback(p2pav_log_cb_t cb, void *user_data);

/* ==================================================================
 *  Common enums
 * ================================================================== */

typedef enum {
    P2PAV_ROLE_PUBLISHER,
    P2PAV_ROLE_SUBSCRIBER,
} p2pav_role_t;

typedef enum {
    P2PAV_RELIABLE,
    P2PAV_UNRELIABLE,
    P2PAV_SEMI_RELIABLE,
} p2pav_reliability_t;

typedef enum {
    P2PAV_CODEC_H264,
    P2PAV_CODEC_H265,
    P2PAV_CODEC_VP8,
    P2PAV_CODEC_VP9,
    P2PAV_CODEC_AV1,
} p2pav_video_codec_t;

typedef enum {
    P2PAV_CODEC_OPUS,
    P2PAV_CODEC_AAC,
    P2PAV_CODEC_G711A,
    P2PAV_CODEC_G711U,
} p2pav_audio_codec_t;

typedef enum {
    P2PAV_BITRATE_AUTO,
    P2PAV_BITRATE_LOW,        /* ~500kbps  (360p) */
    P2PAV_BITRATE_MEDIUM,     /* ~1.5Mbps  (720p) */
    P2PAV_BITRATE_HIGH,       /* ~4Mbps    (1080p) */
    P2PAV_BITRATE_CUSTOM,
} p2pav_bitrate_mode_t;

typedef enum {
    P2PAV_PATH_UNKNOWN,
    P2PAV_PATH_DIRECT,
    P2PAV_PATH_RELAY,
} p2pav_path_type_t;

/* ==================================================================
 *  Opaque session handle
 * ================================================================== */

typedef struct p2pav_session_s p2pav_session_t;

/* ==================================================================
 *  Session API
 * ================================================================== */

#define P2PAV_MAX_PEERS  10

typedef struct {
    const char       *signaling_url;   /* "host:port" (HTTPS) */
    const char       *auth_token;      /* JWT token (NULL = no auth) */
    const char       *room_id;
    const char       *peer_id;
    p2pav_role_t      role;

    const char       *stun_server;     /* "host:port", NULL = signaling-provided */
    const char       *turn_server;
    const char       *turn_username;
    const char       *turn_password;

    /* TLS cert/key for xquic (publisher must provide) */
    const char       *ssl_cert_file;
    const char       *ssl_key_file;

    int               max_peers;       /* 0 = default (10) */
    int               enable_tcp;      /* enable ICE-TCP candidates */
} p2pav_session_config_t;

typedef struct {
    void (*on_connected)(p2pav_session_t *s, void *user_data);
    void (*on_disconnected)(p2pav_session_t *s,
                            p2pav_error_t reason, void *user_data);

    void (*on_peer_joined)(p2pav_session_t *s,
                           const char *peer_id, void *user_data);
    void (*on_peer_left)(p2pav_session_t *s,
                         const char *peer_id, void *user_data);
    void (*on_peer_ready)(p2pav_session_t *s,
                          const char *peer_id, void *user_data);

    void (*on_error)(p2pav_session_t *s,
                     p2pav_error_t err, const char *detail,
                     void *user_data);
} p2pav_session_callbacks_t;

P2PAV_API p2pav_session_t *p2pav_session_create(const p2pav_session_config_t *config);
P2PAV_API void             p2pav_session_destroy(p2pav_session_t *session);

P2PAV_API p2pav_error_t    p2pav_session_start(p2pav_session_t *session);
P2PAV_API void             p2pav_session_stop(p2pav_session_t *session);

P2PAV_API void p2pav_session_set_callbacks(p2pav_session_t *session,
                                            const p2pav_session_callbacks_t *cb,
                                            void *user_data);

P2PAV_API p2pav_error_t p2pav_session_kick_peer(p2pav_session_t *session,
                                                  const char *peer_id);

P2PAV_API int p2pav_session_get_peer_count(p2pav_session_t *session);

/* ==================================================================
 *  Video API
 * ================================================================== */

typedef struct {
    p2pav_video_codec_t   codec;
    int                   width;
    int                   height;
    int                   fps;
    p2pav_bitrate_mode_t  bitrate_mode;
    int                   bitrate_bps;     /* used when BITRATE_CUSTOM */
    int                   gop_size;        /* 0 = default (= fps) */
    p2pav_reliability_t   reliability;     /* default SEMI_RELIABLE */
} p2pav_video_config_t;

typedef struct {
    const uint8_t  *data;
    int             size;
    uint64_t        timestamp_us;
    int             is_keyframe;
} p2pav_video_frame_t;

P2PAV_API p2pav_error_t p2pav_video_set_config(p2pav_session_t *session,
                                               const p2pav_video_config_t *config);

P2PAV_API p2pav_error_t p2pav_video_send(p2pav_session_t *session,
                                          const p2pav_video_frame_t *frame);

P2PAV_API void p2pav_video_mute(p2pav_session_t *session, int mute);

typedef void (*p2pav_on_video_frame_t)(p2pav_session_t *s,
                                       const char *from_peer,
                                       const p2pav_video_frame_t *frame,
                                       void *user_data);

P2PAV_API void p2pav_video_set_recv_callback(p2pav_session_t *session,
                                              p2pav_on_video_frame_t cb,
                                              void *user_data);

P2PAV_API p2pav_error_t p2pav_video_request_keyframe(p2pav_session_t *session,
                                                      const char *from_peer);

typedef void (*p2pav_on_keyframe_request_t)(p2pav_session_t *s,
                                             const char *from_peer,
                                             void *user_data);

P2PAV_API void p2pav_video_set_keyframe_request_callback(p2pav_session_t *session,
                                                         p2pav_on_keyframe_request_t cb,
                                                         void *user_data);

P2PAV_API p2pav_error_t p2pav_video_set_bitrate(p2pav_session_t *session,
                                                  p2pav_bitrate_mode_t mode,
                                                  int bitrate_bps);

/* ==================================================================
 *  Audio API
 * ================================================================== */

typedef struct {
    p2pav_audio_codec_t   codec;
    int                   sample_rate;
    int                   channels;
    int                   bitrate_bps;
    int                   frame_duration_ms;
    p2pav_reliability_t   reliability;     /* default UNRELIABLE */
} p2pav_audio_config_t;

typedef struct {
    const uint8_t  *data;
    int             size;
    uint64_t        timestamp_us;
} p2pav_audio_frame_t;

P2PAV_API p2pav_error_t p2pav_audio_set_config(p2pav_session_t *session,
                                               const p2pav_audio_config_t *config);

P2PAV_API p2pav_error_t p2pav_audio_send(p2pav_session_t *session,
                                          const p2pav_audio_frame_t *frame);

P2PAV_API void p2pav_audio_mute(p2pav_session_t *session, int mute);

typedef void (*p2pav_on_audio_frame_t)(p2pav_session_t *s,
                                       const char *from_peer,
                                       const p2pav_audio_frame_t *frame,
                                       void *user_data);

P2PAV_API void p2pav_audio_set_recv_callback(p2pav_session_t *session,
                                              p2pav_on_audio_frame_t cb,
                                              void *user_data);

/* ==================================================================
 *  Data Channel API
 * ================================================================== */

typedef struct {
    const char         *label;
    p2pav_reliability_t reliability;
    int                 priority;       /* 0=lowest, 255=highest */
} p2pav_data_channel_config_t;

typedef int p2pav_data_channel_id_t;

P2PAV_API p2pav_data_channel_id_t p2pav_data_open(p2pav_session_t *session,
                                                   const char *peer_id,
                                                   const p2pav_data_channel_config_t *config);

P2PAV_API void p2pav_data_close(p2pav_session_t *session,
                                 p2pav_data_channel_id_t ch);

P2PAV_API p2pav_error_t p2pav_data_send(p2pav_session_t *session,
                                         p2pav_data_channel_id_t ch,
                                         const void *data, size_t len);

typedef void (*p2pav_on_data_t)(p2pav_session_t *s,
                                 const char *from_peer,
                                 p2pav_data_channel_id_t ch,
                                 const void *data, size_t len,
                                 void *user_data);

P2PAV_API void p2pav_data_set_recv_callback(p2pav_session_t *session,
                                             p2pav_on_data_t cb,
                                             void *user_data);

typedef void (*p2pav_on_data_channel_t)(p2pav_session_t *s,
                                         const char *peer_id,
                                         p2pav_data_channel_id_t ch,
                                         int is_open,
                                         void *user_data);

P2PAV_API void p2pav_data_set_channel_callback(p2pav_session_t *session,
                                                p2pav_on_data_channel_t cb,
                                                void *user_data);

/* ==================================================================
 *  Diagnostics API
 * ================================================================== */

typedef struct {
    p2pav_path_type_t  path_type;
    char               local_addr[64];
    char               remote_addr[64];
    char               ice_type[16];        /* "host"/"srflx"/"prflx"/"relay" */

    uint32_t  rtt_us;
    uint32_t  min_rtt_us;
    float     packet_loss_rate;
    uint32_t  cwnd;
    uint32_t  bytes_in_flight;
    uint64_t  total_bytes_sent;
    uint64_t  total_bytes_recv;
    uint32_t  total_packets_sent;
    uint32_t  total_packets_recv;
    uint32_t  total_packets_lost;
} p2pav_net_stats_t;

typedef struct {
    uint64_t  session_start_us;
    uint64_t  signaling_connected_us;
    uint64_t  room_joined_us;
    uint64_t  ice_gathering_start_us;
    uint64_t  ice_gathering_done_us;
    uint64_t  ice_connected_us;
    uint64_t  quic_connected_us;
    uint64_t  first_video_sent_us;
    uint64_t  first_video_recv_us;
    uint64_t  first_audio_recv_us;
} p2pav_timing_t;

typedef struct {
    int   count;
    struct {
        char type[16];
        char address[64];
        int  port;
    } candidates[32];
} p2pav_ice_candidates_t;

P2PAV_API p2pav_error_t p2pav_get_net_stats(p2pav_session_t *session,
                                             const char *peer_id,
                                             p2pav_net_stats_t *stats);

P2PAV_API p2pav_error_t p2pav_get_timing(p2pav_session_t *session,
                                          p2pav_timing_t *timing);

P2PAV_API p2pav_error_t p2pav_get_ice_candidates(p2pav_session_t *session,
                                                   const char *peer_id,
                                                   p2pav_ice_candidates_t *local,
                                                   p2pav_ice_candidates_t *remote);

typedef void (*p2pav_on_net_stats_t)(p2pav_session_t *s,
                                      const char *peer_id,
                                      const p2pav_net_stats_t *stats,
                                      void *user_data);

P2PAV_API void p2pav_set_net_stats_callback(p2pav_session_t *session,
                                             p2pav_on_net_stats_t cb,
                                             int interval_ms,
                                             void *user_data);

#ifdef __cplusplus
}
#endif

#endif /* P2PAV_H */
