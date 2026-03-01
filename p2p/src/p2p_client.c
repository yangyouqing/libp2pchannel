/*
 * p2p_client.c -- Publisher: captures A/V, encodes, sends over P2P ICE channel.
 *
 * Usage:
 *   ./p2p_client --signaling 127.0.0.1:8080 --room test-room --peer-id pub1
 *                --video-dev /dev/video0 --audio-dev default
 *                --stun 127.0.0.1:3478
 */

#include "p2p_adapter.h"
#include "p2p_signaling.h"
#include "p2p_av_capture.h"
#include "p2p_av_codec.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <getopt.h>
#include <pthread.h>

typedef struct {
    /* Config */
    char signaling_addr[256];
    char room_id[64];
    char peer_id[64];
    char video_dev[128];
    char audio_dev[128];
    char stun_host[256];
    uint16_t stun_port;
    char ssl_cert[512];
    char ssl_key[512];

    /* Core */
    p2p_engine_t         engine;
    p2p_signaling_client_t sig_client;

    /* A/V capture */
    p2p_video_capture_t  vcap;
    p2p_audio_capture_t  acap;

    /* Encoders */
    p2p_video_encoder_t  venc;
    p2p_audio_encoder_t  aenc;

    /* IDR cache for fast first-frame delivery to new subscribers */
    uint8_t             *idr_cache;
    int                  idr_cache_size;
    pthread_mutex_t      idr_mutex;

    /* Sequence counters */
    uint32_t             video_seq;
    uint32_t             audio_seq;

    /* State */
    volatile int         running;
    int                  capture_started;
} client_ctx_t;

static client_ctx_t g_ctx;

/* ---- Send helpers ---- */

static void send_to_all_peers(client_ctx_t *ctx, uint8_t type, uint8_t flags,
                              uint32_t seq, uint64_t ts,
                              const uint8_t *data, uint32_t len)
{
    for (int i = 0; i < P2P_MAX_SUBSCRIBERS; i++) {
        p2p_peer_ctx_t *peer = &ctx->engine.peers[i];
        if (peer->state >= P2P_PEER_STATE_ICE_CONNECTED && peer->ice_agent) {
            p2p_peer_send_data(peer, type, flags, seq, ts, data, len);
        }
    }
}

static void send_idr_to_peer(client_ctx_t *ctx, p2p_peer_ctx_t *peer)
{
    pthread_mutex_lock(&ctx->idr_mutex);
    if (ctx->idr_cache && ctx->idr_cache_size > 0) {
        uint64_t ts = p2p_capture_now_us();
        p2p_peer_send_data(peer, P2P_FRAME_TYPE_VIDEO, P2P_FRAME_FLAG_KEY,
                           0, ts, ctx->idr_cache, ctx->idr_cache_size);
        fprintf(stderr, "[client] sent cached IDR (%d bytes) to %s\n",
                ctx->idr_cache_size, peer->peer_id);
    }
    pthread_mutex_unlock(&ctx->idr_mutex);
}

/* ---- V4L2 video frame callback (called from capture thread) ---- */

static void on_video_frame(const uint8_t *data, size_t size,
                           int width, int height, int pixfmt,
                           uint64_t timestamp_us, void *user_data)
{
    client_ctx_t *ctx = (client_ctx_t *)user_data;
    uint8_t *out_data = NULL;
    int out_size = 0;
    int is_key = 0;

    int ret = p2p_video_encoder_encode(&ctx->venc, data, size, pixfmt,
                                       &out_data, &out_size, &is_key);
    if (ret <= 0 || out_size <= 0) return;

    uint32_t seq = __sync_fetch_and_add(&ctx->video_seq, 1);
    uint8_t flags = is_key ? P2P_FRAME_FLAG_KEY : 0;

    /* Cache IDR for new subscribers */
    if (is_key) {
        pthread_mutex_lock(&ctx->idr_mutex);
        ctx->idr_cache = realloc(ctx->idr_cache, out_size);
        if (ctx->idr_cache) {
            memcpy(ctx->idr_cache, out_data, out_size);
            ctx->idr_cache_size = out_size;
        }
        pthread_mutex_unlock(&ctx->idr_mutex);
    }

    send_to_all_peers(ctx, P2P_FRAME_TYPE_VIDEO, flags, seq,
                      timestamp_us, out_data, out_size);
}

/* ---- ALSA audio frame callback (called from capture thread) ---- */

static void on_audio_frame(const int16_t *samples, int num_samples,
                           int channels, int sample_rate,
                           uint64_t timestamp_us, void *user_data)
{
    client_ctx_t *ctx = (client_ctx_t *)user_data;
    uint8_t *out_data = NULL;
    int out_size = 0;

    int ret = p2p_audio_encoder_encode(&ctx->aenc, samples, num_samples,
                                       &out_data, &out_size);
    if (ret <= 0 || out_size <= 0) return;

    uint32_t seq = __sync_fetch_and_add(&ctx->audio_seq, 1);

    send_to_all_peers(ctx, P2P_FRAME_TYPE_AUDIO, 0, seq,
                      timestamp_us, out_data, out_size);
}

/* ---- Start capture & encoding ---- */

static int start_capture(client_ctx_t *ctx)
{
    if (ctx->capture_started) return 0;

    /* Video encoder */
    p2p_video_encoder_config_t vcfg = {
        .width = 640, .height = 480, .fps = 30,
        .bitrate = 1000000, .gop_size = 30
    };
    if (p2p_video_encoder_open(&ctx->venc, &vcfg) != 0) {
        fprintf(stderr, "[client] video encoder open failed\n");
        return -1;
    }

    /* Audio encoder */
    p2p_audio_encoder_config_t acfg = {
        .sample_rate = 48000, .channels = 1,
        .bitrate = 64000, .frame_size = 960
    };
    if (p2p_audio_encoder_open(&ctx->aenc, &acfg) != 0) {
        fprintf(stderr, "[client] audio encoder open failed\n");
        return -1;
    }

    /* Video capture */
    p2p_video_capture_config_t vccfg = {
        .device = ctx->video_dev,
        .width = 640, .height = 480, .fps = 30,
        .cb = on_video_frame, .user_data = ctx
    };
    if (p2p_video_capture_open(&ctx->vcap, &vccfg) != 0) {
        fprintf(stderr, "[client] video capture open failed\n");
        return -1;
    }
    if (p2p_video_capture_start(&ctx->vcap) != 0) {
        fprintf(stderr, "[client] video capture start failed\n");
        return -1;
    }

    /* Audio capture */
    p2p_audio_capture_config_t accfg = {
        .device = ctx->audio_dev,
        .sample_rate = 48000, .channels = 1,
        .period_frames = 960,
        .cb = on_audio_frame, .user_data = ctx
    };
    if (p2p_audio_capture_open(&ctx->acap, &accfg) != 0) {
        fprintf(stderr, "[client] audio capture open failed (continuing without audio)\n");
    } else {
        p2p_audio_capture_start(&ctx->acap);
    }

    ctx->capture_started = 1;
    fprintf(stderr, "[client] capture & encoding started\n");
    return 0;
}

/* ---- Adapter callbacks ---- */

static void on_ice_state(p2p_peer_ctx_t *peer, juice_state_t state, void *user_data)
{
    client_ctx_t *ctx = (client_ctx_t *)user_data;
    fprintf(stderr, "[client] peer %s ICE state -> %d\n", peer->peer_id, state);

    if (state == JUICE_STATE_CONNECTED || state == JUICE_STATE_COMPLETED) {
        fprintf(stderr, "[client] ICE connected with %s\n", peer->peer_id);

        /* Start capture on first connection */
        if (!ctx->capture_started)
            start_capture(ctx);

        /* Send cached IDR to new subscriber for fast first-frame */
        send_idr_to_peer(ctx, peer);
    }
}

static void on_ice_candidate(p2p_peer_ctx_t *peer, const char *sdp, void *user_data)
{
    client_ctx_t *ctx = (client_ctx_t *)user_data;
    p2p_signaling_send_ice_candidate(&ctx->sig_client, peer->peer_id, sdp);
}

static void on_ice_gathering_done(p2p_peer_ctx_t *peer, void *user_data)
{
    client_ctx_t *ctx = (client_ctx_t *)user_data;
    p2p_signaling_send_gathering_done(&ctx->sig_client, peer->peer_id);
}

/* ---- Signaling callbacks ---- */

static void on_sig_connected(p2p_signaling_client_t *client, void *user_data)
{
    client_ctx_t *ctx = (client_ctx_t *)user_data;
    fprintf(stderr, "[client] signaling connected, creating room %s\n", ctx->room_id);
    p2p_signaling_create_room(client, ctx->room_id);
}

static void on_sig_room_created(p2p_signaling_client_t *client,
                                const char *room_id, void *user_data)
{
    fprintf(stderr, "[client] room '%s' created, waiting for subscribers...\n", room_id);
}

static void on_sig_peer_joined(p2p_signaling_client_t *client,
                               const char *peer_id, void *user_data)
{
    client_ctx_t *ctx = (client_ctx_t *)user_data;
    fprintf(stderr, "[client] subscriber '%s' joined, starting ICE exchange\n", peer_id);

    p2p_peer_ctx_t *peer = p2p_engine_add_peer(&ctx->engine, peer_id);
    if (!peer) {
        fprintf(stderr, "[client] cannot add peer (max %d)\n", P2P_MAX_SUBSCRIBERS);
        return;
    }

    /* Gather ICE candidates */
    p2p_peer_gather_candidates(peer);

    /* Send local SDP to remote peer */
    char sdp[P2P_SIG_MAX_SDP_SIZE];
    if (p2p_peer_get_local_description(peer, sdp, sizeof(sdp)) == 0) {
        p2p_signaling_send_ice_offer(&ctx->sig_client, peer_id, sdp);
    }
}

static void on_sig_ice_answer(p2p_signaling_client_t *client,
                              const char *from_peer, const char *sdp, void *user_data)
{
    client_ctx_t *ctx = (client_ctx_t *)user_data;
    p2p_peer_ctx_t *peer = p2p_engine_find_peer(&ctx->engine, from_peer);
    if (peer) {
        p2p_peer_set_remote_description(peer, sdp);
    }
}

static void on_sig_ice_candidate(p2p_signaling_client_t *client,
                                 const char *from_peer, const char *candidate,
                                 void *user_data)
{
    client_ctx_t *ctx = (client_ctx_t *)user_data;
    p2p_peer_ctx_t *peer = p2p_engine_find_peer(&ctx->engine, from_peer);
    if (peer) {
        p2p_peer_add_remote_candidate(peer, candidate);
    }
}

static void on_sig_gathering_done(p2p_signaling_client_t *client,
                                  const char *from_peer, void *user_data)
{
    client_ctx_t *ctx = (client_ctx_t *)user_data;
    p2p_peer_ctx_t *peer = p2p_engine_find_peer(&ctx->engine, from_peer);
    if (peer) {
        p2p_peer_set_remote_gathering_done(peer);
    }
}

static void on_sig_peer_left(p2p_signaling_client_t *client,
                             const char *peer_id, void *user_data)
{
    client_ctx_t *ctx = (client_ctx_t *)user_data;
    p2p_peer_ctx_t *peer = p2p_engine_find_peer(&ctx->engine, peer_id);
    if (peer) {
        fprintf(stderr, "[client] subscriber '%s' left\n", peer_id);
        p2p_engine_remove_peer(&ctx->engine, peer->index);
    }
}

static void on_sig_disconnected(p2p_signaling_client_t *client, void *user_data)
{
    fprintf(stderr, "[client] signaling disconnected\n");
}

/* ---- Signal handler ---- */

static void sig_handler(int sig)
{
    (void)sig;
    g_ctx.running = 0;
}

/* ---- Argument parsing ---- */

static void print_usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s [options]\n"
        "  --signaling HOST:PORT   Signaling server address (default 127.0.0.1:8080)\n"
        "  --room ROOM_ID          Room name (default test-room)\n"
        "  --peer-id ID            Publisher peer ID (default pub1)\n"
        "  --video-dev DEV         V4L2 video device (default /dev/video0)\n"
        "  --audio-dev DEV         ALSA audio device (default default)\n"
        "  --stun HOST:PORT        STUN server (default 127.0.0.1:3478)\n"
        "  --ssl-cert FILE         SSL certificate for QUIC (default ./server.crt)\n"
        "  --ssl-key FILE          SSL private key for QUIC (default ./server.key)\n"
        "  --help                  Show this help\n", prog);
}

int main(int argc, char *argv[])
{
    client_ctx_t *ctx = &g_ctx;
    memset(ctx, 0, sizeof(*ctx));

    /* Defaults */
    strcpy(ctx->signaling_addr, "127.0.0.1:8080");
    strcpy(ctx->room_id, "test-room");
    strcpy(ctx->peer_id, "pub1");
    strcpy(ctx->video_dev, "/dev/video0");
    strcpy(ctx->audio_dev, "default");
    strcpy(ctx->stun_host, "127.0.0.1");
    ctx->stun_port = 3478;
    strcpy(ctx->ssl_cert, "./server.crt");
    strcpy(ctx->ssl_key, "./server.key");

    static struct option long_opts[] = {
        {"signaling", required_argument, NULL, 's'},
        {"room",      required_argument, NULL, 'r'},
        {"peer-id",   required_argument, NULL, 'p'},
        {"video-dev", required_argument, NULL, 'v'},
        {"audio-dev", required_argument, NULL, 'a'},
        {"stun",      required_argument, NULL, 't'},
        {"ssl-cert",  required_argument, NULL, 'c'},
        {"ssl-key",   required_argument, NULL, 'k'},
        {"help",      no_argument,       NULL, 'h'},
        {NULL, 0, NULL, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "s:r:p:v:a:t:c:k:h", long_opts, NULL)) != -1) {
        switch (opt) {
        case 's': snprintf(ctx->signaling_addr, sizeof(ctx->signaling_addr), "%s", optarg); break;
        case 'r': snprintf(ctx->room_id, sizeof(ctx->room_id), "%s", optarg); break;
        case 'p': snprintf(ctx->peer_id, sizeof(ctx->peer_id), "%s", optarg); break;
        case 'v': snprintf(ctx->video_dev, sizeof(ctx->video_dev), "%s", optarg); break;
        case 'a': snprintf(ctx->audio_dev, sizeof(ctx->audio_dev), "%s", optarg); break;
        case 'c': snprintf(ctx->ssl_cert, sizeof(ctx->ssl_cert), "%s", optarg); break;
        case 'k': snprintf(ctx->ssl_key, sizeof(ctx->ssl_key), "%s", optarg); break;
        case 't': {
            char tmp[256];
            snprintf(tmp, sizeof(tmp), "%s", optarg);
            char *colon = strrchr(tmp, ':');
            if (colon) {
                *colon = '\0';
                ctx->stun_port = atoi(colon + 1);
            }
            snprintf(ctx->stun_host, sizeof(ctx->stun_host), "%s", tmp);
            break;
        }
        case 'h':
        default:
            print_usage(argv[0]);
            return opt == 'h' ? 0 : 1;
        }
    }

    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);
    pthread_mutex_init(&ctx->idr_mutex, NULL);

    fprintf(stderr, "[client] P2P AV Publisher starting\n");
    fprintf(stderr, "[client] signaling=%s room=%s peer=%s\n",
            ctx->signaling_addr, ctx->room_id, ctx->peer_id);
    fprintf(stderr, "[client] video=%s audio=%s stun=%s:%d\n",
            ctx->video_dev, ctx->audio_dev, ctx->stun_host, ctx->stun_port);

    /* Init P2P adapter engine */
    p2p_engine_config_t ecfg = {
        .stun_server_host = ctx->stun_host,
        .stun_server_port = ctx->stun_port,
        .ssl_cert_file = ctx->ssl_cert,
        .ssl_key_file = ctx->ssl_key,
        .role = P2P_ROLE_PUBLISHER,
        .callbacks = {
            .on_peer_ice_state = on_ice_state,
            .on_peer_ice_candidate = on_ice_candidate,
            .on_peer_ice_gathering_done = on_ice_gathering_done,
        },
        .user_data = ctx
    };

    if (p2p_engine_init(&ctx->engine, &ecfg) != 0) {
        fprintf(stderr, "[client] engine init failed\n");
        return 1;
    }
    p2p_engine_start(&ctx->engine);

    /* Connect to signaling server */
    p2p_signaling_config_t scfg = {
        .server_url = ctx->signaling_addr,
        .peer_id = ctx->peer_id,
        .callbacks = {
            .on_connected = on_sig_connected,
            .on_disconnected = on_sig_disconnected,
            .on_room_created = on_sig_room_created,
            .on_peer_joined = on_sig_peer_joined,
            .on_peer_left = on_sig_peer_left,
            .on_ice_answer = on_sig_ice_answer,
            .on_ice_candidate = on_sig_ice_candidate,
            .on_gathering_done = on_sig_gathering_done,
        },
        .user_data = ctx
    };

    if (p2p_signaling_connect(&ctx->sig_client, &scfg) != 0) {
        fprintf(stderr, "[client] signaling connect failed\n");
        p2p_engine_stop(&ctx->engine);
        p2p_engine_destroy(&ctx->engine);
        return 1;
    }

    /* Main loop */
    ctx->running = 1;
    while (ctx->running) {
        usleep(100000); /* 100ms idle */
    }

    fprintf(stderr, "[client] shutting down...\n");

    /* Cleanup */
    if (ctx->capture_started) {
        p2p_video_capture_stop(&ctx->vcap);
        p2p_video_capture_close(&ctx->vcap);
        p2p_audio_capture_stop(&ctx->acap);
        p2p_audio_capture_close(&ctx->acap);
        p2p_video_encoder_close(&ctx->venc);
        p2p_audio_encoder_close(&ctx->aenc);
    }

    p2p_signaling_disconnect(&ctx->sig_client);
    p2p_engine_stop(&ctx->engine);
    p2p_engine_destroy(&ctx->engine);

    pthread_mutex_lock(&ctx->idr_mutex);
    free(ctx->idr_cache);
    ctx->idr_cache = NULL;
    pthread_mutex_unlock(&ctx->idr_mutex);
    pthread_mutex_destroy(&ctx->idr_mutex);

    fprintf(stderr, "[client] done\n");
    return 0;
}
