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
#include <time.h>
#include "p2p_platform.h"
#ifndef _WIN32
#include <signal.h>
#include <execinfo.h>
#endif
#include <libavutil/log.h>

typedef struct {
    /* Config */
    char signaling_addr[256];
    char room_id[64];
    char peer_id[64];
    char token[1024];
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
    p2p_mutex_t          idr_mutex;

    /* Sequence counters */
    uint32_t             video_seq;
    uint32_t             audio_seq;

    /* ICE-TCP */
    int                  enable_tcp;

    /* AV mode flags */
    int                  no_video;
    int                  no_audio;

    /* Pending peer for full_offer (sent on gathering_done) */
    char                 pending_peer_id[64];

    /* State */
    volatile int         running;
    int                  capture_started;
} client_ctx_t;

static client_ctx_t g_ctx;

/* ---- Send helpers ---- */

static int peer_is_sendable(p2p_peer_ctx_t *peer)
{
    return peer->ice_agent &&
           peer->state >= P2P_PEER_STATE_QUIC_CONNECTED &&
           peer->state < P2P_PEER_STATE_FAILED;
}

static int has_active_receiver(client_ctx_t *ctx, uint8_t type)
{
    for (int i = 0; i < P2P_MAX_SUBSCRIBERS; i++) {
        p2p_peer_ctx_t *peer = &ctx->engine.peers[i];
        if (!peer_is_sendable(peer)) continue;
        if (type == P2P_FRAME_TYPE_VIDEO && peer->video_paused) continue;
        if (type == P2P_FRAME_TYPE_AUDIO && peer->audio_paused) continue;
        return 1;
    }
    return 0;
}

static void send_to_all_peers(client_ctx_t *ctx, uint8_t type, uint8_t flags,
                              uint32_t seq, uint64_t ts,
                              const uint8_t *data, uint32_t len)
{
    for (int i = 0; i < P2P_MAX_SUBSCRIBERS; i++) {
        p2p_peer_ctx_t *peer = &ctx->engine.peers[i];
        if (!peer_is_sendable(peer)) continue;
        if (type == P2P_FRAME_TYPE_VIDEO && peer->video_paused) continue;
        if (type == P2P_FRAME_TYPE_AUDIO && peer->audio_paused) continue;
        p2p_peer_send_data_via_quic(peer, type, flags, seq, ts, data, len);
    }
}

static void send_idr_to_peer(client_ctx_t *ctx, p2p_peer_ctx_t *peer)
{
    p2p_mutex_lock(&ctx->idr_mutex);
    if (ctx->idr_cache && ctx->idr_cache_size > 0) {
        uint64_t ts = p2p_capture_now_us();
        uint32_t seq = p2p_atomic_fetch_add((volatile P2P_ATOMIC_INT *)&ctx->video_seq, 1);
        p2p_peer_send_data_via_quic(peer, P2P_FRAME_TYPE_VIDEO, P2P_FRAME_FLAG_KEY,
                                    seq, ts, ctx->idr_cache, ctx->idr_cache_size);
        fprintf(stderr, "[client] sent cached IDR (%d bytes) seq=%u to %s\n",
                ctx->idr_cache_size, seq, peer->peer_id);
    }
    p2p_mutex_unlock(&ctx->idr_mutex);
}

/* ---- Video frame callback (called from capture thread) ---- */

static uint32_t s_cap_total = 0;
static uint32_t s_enc_ok = 0;
static uint32_t s_enc_skip = 0;
static uint64_t s_last_stat_us = 0;

static void on_video_frame(const uint8_t *data, size_t size,
                           int width, int height, int pixfmt,
                           uint64_t timestamp_us, void *user_data)
{
    client_ctx_t *ctx = (client_ctx_t *)user_data;
    uint8_t *out_data = NULL;
    int out_size = 0;
    int is_key = 0;

    s_cap_total++;

    if (!has_active_receiver(ctx, P2P_FRAME_TYPE_VIDEO))
        goto stats;

    int ret = p2p_video_encoder_encode(&ctx->venc, data, size, pixfmt,
                                       &out_data, &out_size, &is_key);
    if (ret <= 0 || out_size <= 0) {
        s_enc_skip++;
        goto stats;
    }

    s_enc_ok++;
    uint32_t seq = p2p_atomic_fetch_add((volatile P2P_ATOMIC_INT *)&ctx->video_seq, 1);
    uint8_t flags = is_key ? P2P_FRAME_FLAG_KEY : 0;

    int nfrags = (out_size + P2P_FRAME_MAX_FRAG - 1) / P2P_FRAME_MAX_FRAG;
    fprintf(stderr, "[TX] seq=%u %s size=%d frags=%d\n",
            seq, is_key ? "IDR" : "P", out_size, nfrags);

    /* Cache IDR for new subscribers */
    if (is_key) {
        p2p_mutex_lock(&ctx->idr_mutex);
        ctx->idr_cache = realloc(ctx->idr_cache, out_size);
        if (ctx->idr_cache) {
            memcpy(ctx->idr_cache, out_data, out_size);
            ctx->idr_cache_size = out_size;
        }
        p2p_mutex_unlock(&ctx->idr_mutex);
    }

    send_to_all_peers(ctx, P2P_FRAME_TYPE_VIDEO, flags, seq,
                      timestamp_us, out_data, out_size);
stats:
    if (timestamp_us - s_last_stat_us >= 5000000ULL) {
        fprintf(stderr, "[TX-STAT] cap=%u enc_ok=%u skip=%u (%.0f%% skip)\n",
                s_cap_total, s_enc_ok, s_enc_skip,
                s_cap_total ? 100.0 * s_enc_skip / s_cap_total : 0);
        s_last_stat_us = timestamp_us;
    }
}

/* ---- ALSA audio frame callback (called from capture thread) ---- */

static uint32_t s_audio_cap_total = 0;
static uint32_t s_audio_enc_ok = 0;
static uint32_t s_audio_enc_skip = 0;
static uint32_t s_audio_no_recv = 0;
static uint32_t s_audio_sent = 0;
static uint64_t s_audio_last_stat_us = 0;

static void on_audio_frame(const int16_t *samples, int num_samples,
                           int channels, int sample_rate,
                           uint64_t timestamp_us, void *user_data)
{
    client_ctx_t *ctx = (client_ctx_t *)user_data;

    s_audio_cap_total++;

    if (!has_active_receiver(ctx, P2P_FRAME_TYPE_AUDIO)) {
        s_audio_no_recv++;
        return;
    }

    int frame_size = ctx->aenc.frame_size;
    if (frame_size <= 0) frame_size = 960;

    int offset = 0;
    while (offset + frame_size <= num_samples) {
        uint8_t *out_data = NULL;
        int out_size = 0;

        int ret = p2p_audio_encoder_encode(&ctx->aenc,
                                           samples + offset * channels,
                                           frame_size,
                                           &out_data, &out_size);
        if (ret <= 0 || out_size <= 0) {
            s_audio_enc_skip++;
            offset += frame_size;
            continue;
        }

        s_audio_enc_ok++;
        uint32_t seq = p2p_atomic_fetch_add((volatile P2P_ATOMIC_INT *)&ctx->audio_seq, 1);

        fprintf(stderr, "[TX-AUDIO] seq=%u size=%d samples=%d\n", seq, out_size, frame_size);

        send_to_all_peers(ctx, P2P_FRAME_TYPE_AUDIO, 0, seq,
                          timestamp_us, out_data, out_size);
        s_audio_sent++;
        offset += frame_size;
    }
}

/* ---- Start capture & encoding ---- */

static int start_capture(client_ctx_t *ctx)
{
    if (ctx->capture_started) return 0;

    if (!ctx->no_video) {
        /* Video capture - open first to get actual resolution */
        p2p_video_capture_config_t vccfg = {
            .device = ctx->video_dev,
            .width = 640, .height = 480, .fps = 30,
            .cb = on_video_frame, .user_data = ctx
        };
        if (p2p_video_capture_open(&ctx->vcap, &vccfg) != 0) {
            fprintf(stderr, "[client] video capture open FAILED (continuing without video)\n");
            ctx->no_video = 1;
        } else {
            /* Video encoder - use actual capture resolution */
            p2p_video_encoder_config_t vcfg = {
                .width = ctx->vcap.width, .height = ctx->vcap.height, .fps = 30,
                .bitrate = 2000000, .gop_size = 15
            };
            if (p2p_video_encoder_open(&ctx->venc, &vcfg) != 0) {
                fprintf(stderr, "[client] video encoder open FAILED (continuing without video)\n");
                ctx->no_video = 1;
            } else if (p2p_video_capture_start(&ctx->vcap) != 0) {
                fprintf(stderr, "[client] video capture start FAILED (continuing without video)\n");
                ctx->no_video = 1;
            }
        }
    } else {
        fprintf(stderr, "[client] video disabled (--no-video)\n");
    }

    if (!ctx->no_audio) {
        /* Audio encoder */
        fprintf(stderr, "[client] opening audio encoder...\n");
        p2p_audio_encoder_config_t acfg = {
            .sample_rate = 48000, .channels = 1,
            .bitrate = 64000, .frame_size = 960
        };
        if (p2p_audio_encoder_open(&ctx->aenc, &acfg) != 0) {
            fprintf(stderr, "[client] audio encoder open FAILED\n");
            ctx->no_audio = 1;
        } else {
            fprintf(stderr, "[client] audio encoder opened OK\n");
        }

        if (!ctx->no_audio) {
            /* Audio capture */
            fprintf(stderr, "[client] opening audio capture device '%s'...\n", ctx->audio_dev);
            p2p_audio_capture_config_t accfg = {
                .device = ctx->audio_dev,
                .sample_rate = 48000, .channels = 1,
                .period_frames = 960,
                .cb = on_audio_frame, .user_data = ctx
            };
            if (p2p_audio_capture_open(&ctx->acap, &accfg) != 0) {
                fprintf(stderr, "[client] audio capture open FAILED (continuing without audio)\n");
                ctx->no_audio = 1;
            } else {
                fprintf(stderr, "[client] audio capture opened OK, starting...\n");
                if (p2p_audio_capture_start(&ctx->acap) != 0) {
                    fprintf(stderr, "[client] audio capture start FAILED\n");
                    ctx->no_audio = 1;
                } else {
                    fprintf(stderr, "[client] audio capture started OK\n");
                }
            }
        }
    } else {
        fprintf(stderr, "[client] audio disabled (--no-audio)\n");
    }

    ctx->capture_started = 1;
    fprintf(stderr, "[client] capture & encoding started\n");
    return 0;
}

/* ---- IDR request from subscriber ---- */

static void on_data_recv(p2p_peer_ctx_t *peer, const p2p_frame_header_t *hdr,
                         const uint8_t *payload, void *user_data)
{
    client_ctx_t *ctx = (client_ctx_t *)user_data;
    if (hdr->type == P2P_FRAME_TYPE_IDR_REQ) {
        ctx->venc.force_idr = 1;
    } else if (hdr->type == P2P_FRAME_TYPE_VIDEO_STOP) {
        peer->video_paused = 1;
        fprintf(stderr, "[client] peer %s paused video\n", peer->peer_id);
    } else if (hdr->type == P2P_FRAME_TYPE_VIDEO_START) {
        peer->video_paused = 0;
        ctx->venc.force_idr = 1;
        fprintf(stderr, "[client] peer %s resumed video\n", peer->peer_id);
    } else if (hdr->type == P2P_FRAME_TYPE_AUDIO_STOP) {
        peer->audio_paused = 1;
        fprintf(stderr, "[client] peer %s paused audio\n", peer->peer_id);
    } else if (hdr->type == P2P_FRAME_TYPE_AUDIO_START) {
        peer->audio_paused = 0;
        fprintf(stderr, "[client] peer %s resumed audio\n", peer->peer_id);
    } else if (hdr->type == P2P_FRAME_TYPE_PING) {
        /* Keepalive from subscriber; no-op, ice_on_recv already updated last_ice_recv_us */
    }
}

/* ---- Adapter callbacks ---- */

static int count_active_peers(client_ctx_t *ctx)
{
    int n = 0;
    for (int i = 0; i < P2P_MAX_SUBSCRIBERS; i++) {
        p2p_peer_ctx_t *p = &ctx->engine.peers[i];
        if (p->ice_agent && p->state >= P2P_PEER_STATE_QUIC_CONNECTED &&
            p->state < P2P_PEER_STATE_FAILED)
            n++;
    }
    return n;
}

static void on_ice_state(p2p_peer_ctx_t *peer, p2p_ice_state_t state, void *user_data)
{
    client_ctx_t *ctx = (client_ctx_t *)user_data;
    fprintf(stderr, "[client] peer %s ICE state -> %d\n", peer->peer_id, state);

    if (state == P2P_ICE_STATE_CONNECTED) {
        fprintf(stderr, "[client] ICE connected with %s (awaiting QUIC handshake)\n",
                peer->peer_id);
        if (!ctx->capture_started)
            start_capture(ctx);
    } else if (state == P2P_ICE_STATE_COMPLETED) {
        fprintf(stderr, "[client] ICE completed with %s\n", peer->peer_id);
    } else if (state == P2P_ICE_STATE_FAILED || state == P2P_ICE_STATE_DISCONNECTED) {
        fprintf(stderr, "[client] peer %s ICE %s\n",
                peer->peer_id,
                state == P2P_ICE_STATE_FAILED ? "failed" : "disconnected");
        fprintf(stderr, "[client] active peers: %d\n", count_active_peers(ctx));
    }
}

static void on_ice_restart_needed_pub(p2p_peer_ctx_t *peer, void *user_data)
{
    (void)user_data;
    fprintf(stderr, "[client] ICE restart needed for %s, marking for removal\n", peer->peer_id);
    peer->needs_removal = 1;
}

static void on_quic_connected(p2p_peer_ctx_t *peer, void *user_data)
{
    client_ctx_t *ctx = (client_ctx_t *)user_data;
    fprintf(stderr, "[client] QUIC connected with %s, sending IDR\n", peer->peer_id);
    send_idr_to_peer(ctx, peer);
}

static void on_ice_candidate(p2p_peer_ctx_t *peer, const char *sdp, void *user_data)
{
    client_ctx_t *ctx = (client_ctx_t *)user_data;
    fprintf(stderr, "[client] trickle candidate to %s\n", peer->peer_id);
    p2p_signaling_send_ice_candidate_async(&ctx->sig_client, peer->peer_id, sdp);
}

static void on_ice_gathering_done(p2p_peer_ctx_t *peer, void *user_data)
{
    client_ctx_t *ctx = (client_ctx_t *)user_data;
    fprintf(stderr, "[client] gathering done, notifying %s\n", peer->peer_id);
    p2p_signaling_send_gathering_done_async(&ctx->sig_client, peer->peer_id);
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
    client_ctx_t *ctx = (client_ctx_t *)user_data;
    fprintf(stderr, "[client] room '%s' created, waiting for subscribers...\n", room_id);

    /* Pre-start capture so camera/encoder are warm and IDR cache is ready
     * before any peer joins -- saves 200-500ms on first frame latency. */
    if (!ctx->capture_started)
        start_capture(ctx);
}

static void on_sig_peer_joined(p2p_signaling_client_t *client,
                               const char *peer_id, void *user_data)
{
    client_ctx_t *ctx = (client_ctx_t *)user_data;
    fprintf(stderr, "[client] subscriber '%s' joined, starting ICE exchange\n", peer_id);

    /* If a stale peer with the same ID exists, remove it first */
    p2p_peer_ctx_t *old = p2p_engine_find_peer(&ctx->engine, peer_id);
    if (old) {
        fprintf(stderr, "[client] removing stale peer '%s' before re-adding\n", peer_id);
        p2p_engine_remove_peer(&ctx->engine, old->index);
    }

    /* Force next encoded frame to be IDR so cached IDR is fresh for the new peer */
    if (!ctx->no_video && ctx->capture_started)
        ctx->venc.force_idr = 1;

    p2p_peer_ctx_t *peer = p2p_engine_add_peer(&ctx->engine, peer_id);
    if (!peer) {
        fprintf(stderr, "[client] cannot add peer (max %d)\n", P2P_MAX_SUBSCRIBERS);
        return;
    }

    /* Send offer immediately (async, non-blocking) so the peer can start
     * processing while we gather candidates in parallel. */
    char sdp[P2P_SIG_MAX_SDP_SIZE];
    p2p_peer_get_local_description(peer, sdp, sizeof(sdp));
    p2p_signaling_send_ice_offer_async(&ctx->sig_client, peer_id, sdp);

    snprintf(ctx->pending_peer_id, sizeof(ctx->pending_peer_id), "%s", peer_id);

    p2p_peer_gather_candidates(peer);
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

#ifndef _WIN32
static void sig_handler(int sig)
{
    (void)sig;
    g_ctx.running = 0;
}

static void crash_handler(int sig)
{
    void *bt[32];
    int n = backtrace(bt, 32);
    fprintf(stderr, "\n[CRASH] signal %d, backtrace:\n", sig);
    backtrace_symbols_fd(bt, n, 2);
    _exit(128 + sig);
}
#endif

/* ---- Argument parsing ---- */

static void print_usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s [options]\n"
        "  --signaling HOST:PORT   Signaling server address (default 127.0.0.1:8080)\n"
        "  --room ROOM_ID          Room name (if omitted, 6-char random room is generated)\n"
        "  --peer-id ID            Publisher peer ID (default pub1)\n"
#ifdef _WIN32
        "  --video-dev DEV         Video device name (default \"Integrated Camera\")\n"
        "  --audio-dev DEV         Audio device name (default \"Microphone\")\n"
#else
        "  --video-dev DEV         V4L2 video device (default /dev/video0)\n"
        "  --audio-dev DEV         ALSA audio device (default default)\n"
#endif
        "  --stun HOST:PORT        STUN server (default 127.0.0.1:3478)\n"
        "  --ssl-cert FILE         SSL certificate for QUIC (default ./server.crt)\n"
        "  --ssl-key FILE          SSL private key for QUIC (default ./server.key)\n"
        "  --enable-tcp            Enable ICE-TCP transport candidates\n"
        "  --no-video              Audio-only mode (skip video capture/encoding)\n"
        "  --no-audio              Video-only mode (skip audio capture/encoding)\n"
        "  --help                  Show this help\n", prog);
}

int main(int argc, char *argv[])
{
    client_ctx_t *ctx = &g_ctx;
    memset(ctx, 0, sizeof(*ctx));

    setvbuf(stderr, NULL, _IONBF, 0);
    setvbuf(stdout, NULL, _IONBF, 0);

    av_log_set_level(AV_LOG_FATAL);

#ifdef _WIN32
    p2p_net_init();
#endif

    /* Defaults */
    strcpy(ctx->signaling_addr, "127.0.0.1:8443");
    ctx->room_id[0] = '\0';  /* empty until --room or auto-generated */
    strcpy(ctx->peer_id, "pub1");
    ctx->token[0] = '\0';
#ifdef _WIN32
    strcpy(ctx->video_dev, "Integrated Camera");
    strcpy(ctx->audio_dev, "Microphone");
#else
    strcpy(ctx->video_dev, "/dev/video0");
    strcpy(ctx->audio_dev, "default");
#endif
    strcpy(ctx->stun_host, "127.0.0.1");
    ctx->stun_port = 3478;
    strcpy(ctx->ssl_cert, "./server.crt");
    strcpy(ctx->ssl_key, "./server.key");

    static struct p2p_option long_opts[] = {
        {"signaling", 1, NULL, 's'},
        {"room",      1, NULL, 'r'},
        {"peer-id",   1, NULL, 'p'},
        {"token",     1, NULL, 'T'},
        {"video-dev", 1, NULL, 'v'},
        {"audio-dev", 1, NULL, 'a'},
        {"stun",      1, NULL, 't'},
        {"ssl-cert",  1, NULL, 'c'},
        {"ssl-key",   1, NULL, 'k'},
        {"enable-tcp",0, NULL, 'E'},
        {"no-video",  0, NULL, 'V'},
        {"no-audio",  0, NULL, 'A'},
        {"help",      0, NULL, 'h'},
        {NULL, 0, NULL, 0}
    };

    int opt;
    while ((opt = p2p_getopt_long(argc, argv, "s:r:p:T:v:a:t:c:k:EVAh", long_opts, NULL)) != -1) {
        switch (opt) {
        case 's': snprintf(ctx->signaling_addr, sizeof(ctx->signaling_addr), "%s", p2p_optarg); break;
        case 'r': snprintf(ctx->room_id, sizeof(ctx->room_id), "%s", p2p_optarg); break;
        case 'p': snprintf(ctx->peer_id, sizeof(ctx->peer_id), "%s", p2p_optarg); break;
        case 'T': snprintf(ctx->token, sizeof(ctx->token), "%s", p2p_optarg); break;
        case 'v': snprintf(ctx->video_dev, sizeof(ctx->video_dev), "%s", p2p_optarg); break;
        case 'a': snprintf(ctx->audio_dev, sizeof(ctx->audio_dev), "%s", p2p_optarg); break;
        case 'c': snprintf(ctx->ssl_cert, sizeof(ctx->ssl_cert), "%s", p2p_optarg); break;
        case 'k': snprintf(ctx->ssl_key, sizeof(ctx->ssl_key), "%s", p2p_optarg); break;
        case 'E': ctx->enable_tcp = 1; break;
        case 'V': ctx->no_video = 1; break;
        case 'A': ctx->no_audio = 1; break;
        case 't': {
            char tmp[256];
            snprintf(tmp, sizeof(tmp), "%s", p2p_optarg);
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

#ifdef _WIN32
    p2p_set_ctrl_handler(&ctx->running);
#else
    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);
    signal(SIGSEGV, crash_handler);
    signal(SIGABRT, crash_handler);
#endif
    p2p_mutex_init(&ctx->idr_mutex);

    /* Auto-generate 6-char room_id if --room not specified */
    if (ctx->room_id[0] == '\0') {
        srand((unsigned)time(NULL));
        snprintf(ctx->room_id, sizeof(ctx->room_id), "%06d", rand() % 1000000);
        fprintf(stderr, "[client] room_id=%s (auto-generated)\n", ctx->room_id);
    }

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
        .enable_tcp = ctx->enable_tcp,
        .role = P2P_ROLE_PUBLISHER,
        .callbacks = {
            .on_peer_ice_state = on_ice_state,
            .on_peer_ice_candidate = on_ice_candidate,
            .on_peer_ice_gathering_done = on_ice_gathering_done,
            .on_peer_quic_connected = on_quic_connected,
            .on_peer_data_recv = on_data_recv,
            .on_peer_ice_restart_needed = on_ice_restart_needed_pub,
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
        .token = ctx->token[0] ? ctx->token : NULL,
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
        p2p_sleep_ms(100);
    }

    fprintf(stderr, "[client] shutting down...\n");

    /* Cleanup */
    if (ctx->capture_started) {
        if (!ctx->no_video) {
            p2p_video_capture_stop(&ctx->vcap);
            p2p_video_capture_close(&ctx->vcap);
            p2p_video_encoder_close(&ctx->venc);
        }
        if (!ctx->no_audio) {
            p2p_audio_capture_stop(&ctx->acap);
            p2p_audio_capture_close(&ctx->acap);
            p2p_audio_encoder_close(&ctx->aenc);
        }
    }

    p2p_signaling_disconnect(&ctx->sig_client);
    p2p_engine_stop(&ctx->engine);
    p2p_engine_destroy(&ctx->engine);

    p2p_mutex_lock(&ctx->idr_mutex);
    free(ctx->idr_cache);
    ctx->idr_cache = NULL;
    p2p_mutex_unlock(&ctx->idr_mutex);
    p2p_mutex_destroy(&ctx->idr_mutex);

#ifdef _WIN32
    p2p_net_cleanup();
#endif

    fprintf(stderr, "[client] done\n");
    return 0;
}
