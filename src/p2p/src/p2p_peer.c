/*
 * p2p_peer.c -- Subscriber: receives A/V over P2P ICE channel, decodes, renders.
 *
 * Usage:
 *   ./p2p_peer --signaling 127.0.0.1:8080 --room test-room --peer-id sub1
 *              --stun 127.0.0.1:3478
 */

#include "p2p_adapter.h"
#include "p2p_signaling.h"
#include "p2p_av_capture.h"
#include "p2p_av_codec.h"
#include "p2p_sdl_render.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "p2p_platform.h"
#ifndef _WIN32
#include <signal.h>
#endif
#include <libavutil/log.h>

/* Reassembly buffer for fragmented frames */
#define REASM_MAX_SIZE (512 * 1024)

typedef struct {
    uint8_t  type;
    uint8_t  flags;
    uint32_t seq;
    uint64_t timestamp_us;
    uint32_t total_len;
    uint32_t received;
    int      active;
    uint8_t  data[REASM_MAX_SIZE];
} reasm_buf_t;

typedef struct {
    /* Config */
    char signaling_addr[256];
    char room_id[64];
    char peer_id[64];
    char token[1024];
    char stun_host[256];
    uint16_t stun_port;

    /* Core */
    p2p_engine_t         engine;
    p2p_signaling_client_t sig_client;

    /* Decoders */
    p2p_video_decoder_t  vdec;
    p2p_audio_decoder_t  adec;
    int                  vdec_open;
    int                  adec_open;

    /* SDL2 render */
    p2p_sdl_video_t      sdl_video;
    p2p_sdl_audio_t      sdl_audio;
    int                  sdl_initialized;

    /* Reassembly */
    reasm_buf_t          video_reasm;
    reasm_buf_t          audio_reasm;
    p2p_mutex_t          reasm_mutex;

    /* Frame queue for main-thread rendering */
    struct {
        uint8_t *y, *u, *v;
        int lsy, lsu, lsv;
        int w, h;
        int ready;
    } vframe;
    struct {
        int16_t *samples;
        int count;
        int ready;
    } aframe;
    p2p_mutex_t          frame_mutex;

    /* Frame loss tracking */
    uint32_t             last_video_seq;
    int                  has_last_video_seq;
    uint32_t             video_frames_ok;
    uint32_t             video_frames_lost;
    uint32_t             video_frames_incomplete;
    int                  waiting_for_keyframe;
    uint64_t             last_idr_req_us;
    uint64_t             last_rx_stat_us;
    uint32_t             rx_frag_total;
    uint32_t             rx_frag_dup;

    /* Timing */
    uint64_t             first_frame_time;
    int                  got_first_frame;

    /* ICE-TCP */
    int                  enable_tcp;

    /* State */
    volatile int         running;
} peer_ctx_t;

static peer_ctx_t g_ctx;

/* ---- Frame reassembly & decode ---- */

static void process_complete_frame(peer_ctx_t *ctx, reasm_buf_t *rb)
{
    if (rb->type == P2P_FRAME_TYPE_VIDEO) {
        if (!ctx->vdec_open) {
            if (p2p_video_decoder_open(&ctx->vdec) != 0) return;
            ctx->vdec_open = 1;
        }

        uint8_t *y, *u, *v;
        int lsy, lsu, lsv, w, h;
        int ret = p2p_video_decoder_decode(&ctx->vdec,
            rb->data, rb->total_len,
            &y, &u, &v, &lsy, &lsu, &lsv, &w, &h);

        if (ret == 0 && w > 0 && h > 0) {
            if (!ctx->got_first_frame) {
                ctx->first_frame_time = p2p_capture_now_us();
                ctx->got_first_frame = 1;
                fprintf(stderr, "[peer] first video frame decoded: %dx%d\n", w, h);
            }

            p2p_mutex_lock(&ctx->frame_mutex);
            int y_size = lsy * h;
            int uv_h = h / 2;
            int u_size = lsu * uv_h;
            int v_size = lsv * uv_h;

            ctx->vframe.y = realloc(ctx->vframe.y, y_size);
            ctx->vframe.u = realloc(ctx->vframe.u, u_size);
            ctx->vframe.v = realloc(ctx->vframe.v, v_size);
            if (ctx->vframe.y) memcpy(ctx->vframe.y, y, y_size);
            if (ctx->vframe.u) memcpy(ctx->vframe.u, u, u_size);
            if (ctx->vframe.v) memcpy(ctx->vframe.v, v, v_size);
            ctx->vframe.lsy = lsy;
            ctx->vframe.lsu = lsu;
            ctx->vframe.lsv = lsv;
            ctx->vframe.w = w;
            ctx->vframe.h = h;
            ctx->vframe.ready = 1;
            p2p_mutex_unlock(&ctx->frame_mutex);
        }
    } else if (rb->type == P2P_FRAME_TYPE_AUDIO) {
        if (!ctx->adec_open) {
            if (p2p_audio_decoder_open(&ctx->adec, 48000, 1) != 0) return;
            ctx->adec_open = 1;
        }

        int16_t *samples = NULL;
        int num_samples = 0;
        int ret = p2p_audio_decoder_decode(&ctx->adec,
            rb->data, rb->total_len,
            &samples, &num_samples);

        if (ret > 0 && samples && num_samples > 0) {
            p2p_mutex_lock(&ctx->frame_mutex);
            ctx->aframe.samples = realloc(ctx->aframe.samples,
                                          num_samples * sizeof(int16_t));
            if (ctx->aframe.samples)
                memcpy(ctx->aframe.samples, samples, num_samples * sizeof(int16_t));
            ctx->aframe.count = num_samples;
            ctx->aframe.ready = 1;
            p2p_mutex_unlock(&ctx->frame_mutex);
        }
    }
}

static void request_idr_if_needed(peer_ctx_t *ctx, p2p_peer_ctx_t *peer)
{
    uint64_t now = p2p_capture_now_us();
    if (now - ctx->last_idr_req_us < 200000)
        return;
    ctx->last_idr_req_us = now;
    ctx->waiting_for_keyframe = 1;
    p2p_peer_send_idr_request(peer);
    fprintf(stderr, "[RX] IDR request sent\n");
}

static void rx_print_stats(peer_ctx_t *ctx)
{
    uint64_t now = p2p_capture_now_us();
    if (now - ctx->last_rx_stat_us < 5000000ULL) return;
    ctx->last_rx_stat_us = now;
    fprintf(stderr, "[RX-STAT] ok=%u lost=%u incomplete=%u frags=%u dup=%u\n",
            ctx->video_frames_ok, ctx->video_frames_lost,
            ctx->video_frames_incomplete,
            ctx->rx_frag_total, ctx->rx_frag_dup);
}

static void on_data_recv(p2p_peer_ctx_t *peer, const p2p_frame_header_t *hdr,
                         const uint8_t *payload, void *user_data)
{
    peer_ctx_t *ctx = (peer_ctx_t *)user_data;

    if (hdr->type == P2P_FRAME_TYPE_IDR_REQ) return;
    if (hdr->type != P2P_FRAME_TYPE_VIDEO && hdr->type != P2P_FRAME_TYPE_AUDIO)
        return;

    reasm_buf_t *rb = (hdr->type == P2P_FRAME_TYPE_VIDEO)
                    ? &ctx->video_reasm
                    : &ctx->audio_reasm;

    p2p_mutex_lock(&ctx->reasm_mutex);

    if (hdr->type == P2P_FRAME_TYPE_VIDEO) {
        ctx->rx_frag_total++;

        /* Detect sequence gaps (lost frames) */
        if (ctx->has_last_video_seq && hdr->frag_offset == 0) {
            uint32_t expected = ctx->last_video_seq + 1;
            if (hdr->seq > expected) {
                uint32_t gap = hdr->seq - expected;
                ctx->video_frames_lost += gap;
                fprintf(stderr, "[RX] LOST %u frames (expected seq=%u got seq=%u)\n",
                        gap, expected, hdr->seq);
                request_idr_if_needed(ctx, peer);
            }
        }

        /* Track: if we're starting a new frame and the old one was incomplete */
        if (rb->active && hdr->seq != rb->seq && rb->received < rb->total_len) {
            ctx->video_frames_incomplete++;
            fprintf(stderr, "[RX] INCOMPLETE seq=%u got=%u/%u bytes\n",
                    rb->seq, rb->received, rb->total_len);
            request_idr_if_needed(ctx, peer);
        }

        if (hdr->frag_offset == 0) {
            ctx->last_video_seq = hdr->seq;
            ctx->has_last_video_seq = 1;
        }

        /* If waiting for keyframe, drop non-key P-frames */
        if (ctx->waiting_for_keyframe) {
            if (hdr->frag_offset == 0 && (hdr->flags & P2P_FRAME_FLAG_KEY)) {
                ctx->waiting_for_keyframe = 0;
                fprintf(stderr, "[RX] got IDR seq=%u, resuming\n", hdr->seq);
            } else if (hdr->frag_offset == 0) {
                p2p_mutex_unlock(&ctx->reasm_mutex);
                return;
            }
        }

        rx_print_stats(ctx);
    }

    /* New frame or different sequence -> reset reassembly */
    if (!rb->active || rb->seq != hdr->seq || rb->type != hdr->type) {
        /* Stale fragment from an older frame -> discard */
        if (rb->active && hdr->type == rb->type && hdr->seq < rb->seq) {
            ctx->rx_frag_dup++;
            p2p_mutex_unlock(&ctx->reasm_mutex);
            return;
        }
        rb->type = hdr->type;
        rb->flags = hdr->flags;
        rb->seq = hdr->seq;
        rb->timestamp_us = hdr->timestamp_us;
        rb->total_len = hdr->total_len;
        rb->received = 0;
        rb->active = 1;
        if (rb->total_len > REASM_MAX_SIZE) {
            fprintf(stderr, "[RX] frame seq=%u too large %u > %d\n",
                    hdr->seq, rb->total_len, REASM_MAX_SIZE);
            rb->active = 0;
            p2p_mutex_unlock(&ctx->reasm_mutex);
            return;
        }
    }

    /* Bounds check */
    uint32_t frag_end = hdr->frag_offset + hdr->frag_len;
    if (hdr->frag_offset >= REASM_MAX_SIZE || frag_end > REASM_MAX_SIZE ||
        frag_end > rb->total_len) {
        fprintf(stderr, "[RX] OOB seq=%u off=%u len=%u total=%u\n",
                hdr->seq, hdr->frag_offset, hdr->frag_len, rb->total_len);
        p2p_mutex_unlock(&ctx->reasm_mutex);
        return;
    }

    memcpy(rb->data + hdr->frag_offset, payload, hdr->frag_len);
    rb->received += hdr->frag_len;

    if (hdr->frag_offset == 0)
        rb->flags = hdr->flags;

    /* Frame complete? */
    if (rb->received >= rb->total_len) {
        if (hdr->type == P2P_FRAME_TYPE_VIDEO) {
            fprintf(stderr, "[RX] seq=%u %s size=%u OK\n",
                    rb->seq, (rb->flags & P2P_FRAME_FLAG_KEY) ? "IDR" : "P", rb->total_len);
            ctx->video_frames_ok++;
        }
        process_complete_frame(ctx, rb);
        rb->active = 0;
        rb->received = 0;
    }

    p2p_mutex_unlock(&ctx->reasm_mutex);
}

/* ---- Adapter callbacks ---- */

static void on_ice_state(p2p_peer_ctx_t *peer, juice_state_t state, void *user_data)
{
    fprintf(stderr, "[peer] ICE state -> %d with %s\n", state, peer->peer_id);

    if (state == JUICE_STATE_CONNECTED || state == JUICE_STATE_COMPLETED) {
        fprintf(stderr, "[peer] ICE connected with publisher %s\n", peer->peer_id);
    }
}

static void on_ice_candidate(p2p_peer_ctx_t *peer, const char *sdp, void *user_data)
{
    peer_ctx_t *ctx = (peer_ctx_t *)user_data;
    p2p_signaling_send_ice_candidate(&ctx->sig_client, peer->peer_id, sdp);
}

static void on_ice_gathering_done(p2p_peer_ctx_t *peer, void *user_data)
{
    peer_ctx_t *ctx = (peer_ctx_t *)user_data;
    p2p_signaling_send_gathering_done(&ctx->sig_client, peer->peer_id);
}

/* ---- Signaling callbacks ---- */

static void on_sig_connected(p2p_signaling_client_t *client, void *user_data)
{
    peer_ctx_t *ctx = (peer_ctx_t *)user_data;
    fprintf(stderr, "[peer] signaling connected, joining room %s\n", ctx->room_id);
    p2p_signaling_join_room(client, ctx->room_id);
}

static void on_sig_room_created(p2p_signaling_client_t *client,
                                const char *room_id, void *user_data)
{
    fprintf(stderr, "[peer] joined room '%s'\n", room_id);
}

static void on_sig_ice_offer(p2p_signaling_client_t *client,
                             const char *from_peer, const char *sdp, void *user_data)
{
    peer_ctx_t *ctx = (peer_ctx_t *)user_data;
    fprintf(stderr, "[peer] got ICE offer from publisher '%s'\n", from_peer);

    p2p_peer_ctx_t *peer = p2p_engine_find_peer(&ctx->engine, from_peer);
    if (!peer) {
        peer = p2p_engine_add_peer(&ctx->engine, from_peer);
        if (!peer) {
            fprintf(stderr, "[peer] cannot add peer\n");
            return;
        }
    }

    /* Set remote description and gather our candidates */
    p2p_peer_set_remote_description(peer, sdp);
    p2p_peer_gather_candidates(peer);

    /* Send answer */
    char local_sdp[P2P_SIG_MAX_SDP_SIZE];
    if (p2p_peer_get_local_description(peer, local_sdp, sizeof(local_sdp)) == 0) {
        p2p_signaling_send_ice_answer(&ctx->sig_client, from_peer, local_sdp);
    }
}

static void on_sig_ice_candidate(p2p_signaling_client_t *client,
                                 const char *from_peer, const char *candidate,
                                 void *user_data)
{
    peer_ctx_t *ctx = (peer_ctx_t *)user_data;
    fprintf(stderr, "[peer] sig_ice_candidate from=%s cand=%s\n", from_peer, candidate);
    p2p_peer_ctx_t *peer = p2p_engine_find_peer(&ctx->engine, from_peer);
    if (peer) {
        p2p_peer_add_remote_candidate(peer, candidate);
    } else {
        fprintf(stderr, "[peer] WARNING: peer '%s' not found, dropping candidate\n", from_peer);
    }
}

static void on_sig_gathering_done(p2p_signaling_client_t *client,
                                  const char *from_peer, void *user_data)
{
    peer_ctx_t *ctx = (peer_ctx_t *)user_data;
    p2p_peer_ctx_t *peer = p2p_engine_find_peer(&ctx->engine, from_peer);
    if (peer) {
        p2p_peer_set_remote_gathering_done(peer);
    }
}

static void on_sig_disconnected(p2p_signaling_client_t *client, void *user_data)
{
    fprintf(stderr, "[peer] signaling disconnected\n");
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
        "  --peer-id ID            Subscriber peer ID (default sub1)\n"
        "  --stun HOST:PORT        STUN server (default 127.0.0.1:3478)\n"
        "  --enable-tcp            Enable ICE-TCP transport candidates\n"
        "  --help                  Show this help\n", prog);
}

int main(int argc, char *argv[])
{
    peer_ctx_t *ctx = &g_ctx;
    memset(ctx, 0, sizeof(*ctx));

    av_log_set_level(AV_LOG_FATAL);

    /* Defaults */
    strcpy(ctx->signaling_addr, "127.0.0.1:8443");
    strcpy(ctx->room_id, "test-room");
    strcpy(ctx->peer_id, "sub1");
    ctx->token[0] = '\0';
    strcpy(ctx->stun_host, "127.0.0.1");
    ctx->stun_port = 3478;

    static struct p2p_option long_opts[] = {
        {"signaling", required_argument, NULL, 's'},
        {"room",      required_argument, NULL, 'r'},
        {"peer-id",   required_argument, NULL, 'p'},
        {"token",     required_argument, NULL, 'T'},
        {"stun",       required_argument, NULL, 't'},
        {"enable-tcp", no_argument,      NULL, 'E'},
        {"help",       no_argument,      NULL, 'h'},
        {NULL, 0, NULL, 0}
    };

    int opt;
    while ((opt = p2p_getopt_long(argc, argv, "s:r:p:T:t:Eh", long_opts, NULL)) != -1) {
        switch (opt) {
        case 's': snprintf(ctx->signaling_addr, sizeof(ctx->signaling_addr), "%s", p2p_optarg); break;
        case 'r': snprintf(ctx->room_id, sizeof(ctx->room_id), "%s", p2p_optarg); break;
        case 'p': snprintf(ctx->peer_id, sizeof(ctx->peer_id), "%s", p2p_optarg); break;
        case 'T': snprintf(ctx->token, sizeof(ctx->token), "%s", p2p_optarg); break;
        case 'E': ctx->enable_tcp = 1; break;
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
    p2p_net_init();
    p2p_set_ctrl_handler(&ctx->running);
#else
    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);
#endif
    p2p_mutex_init(&ctx->reasm_mutex);
    p2p_mutex_init(&ctx->frame_mutex);

    fprintf(stderr, "[peer] P2P AV Subscriber starting\n");
    fprintf(stderr, "[peer] signaling=%s room=%s peer=%s stun=%s:%d\n",
            ctx->signaling_addr, ctx->room_id, ctx->peer_id,
            ctx->stun_host, ctx->stun_port);

    /* Init SDL2 (must be on main thread) */
    if (p2p_sdl_init() != 0) {
        fprintf(stderr, "[peer] SDL init failed\n");
        return 1;
    }

    if (p2p_sdl_video_init(&ctx->sdl_video, "P2P Video", 640, 480) != 0) {
        fprintf(stderr, "[peer] SDL video init failed\n");
        p2p_sdl_quit();
        return 1;
    }

    if (p2p_sdl_audio_init(&ctx->sdl_audio, 48000, 1) != 0) {
        fprintf(stderr, "[peer] SDL audio init failed (continuing without audio)\n");
    }
    ctx->sdl_initialized = 1;

    /* Init P2P adapter engine */
    p2p_engine_config_t ecfg = {
        .stun_server_host = ctx->stun_host,
        .stun_server_port = ctx->stun_port,
        .enable_tcp = ctx->enable_tcp,
        .role = P2P_ROLE_SUBSCRIBER,
        .callbacks = {
            .on_peer_ice_state = on_ice_state,
            .on_peer_ice_candidate = on_ice_candidate,
            .on_peer_ice_gathering_done = on_ice_gathering_done,
            .on_peer_data_recv = on_data_recv,
        },
        .user_data = ctx
    };

    if (p2p_engine_init(&ctx->engine, &ecfg) != 0) {
        fprintf(stderr, "[peer] engine init failed\n");
        p2p_sdl_video_destroy(&ctx->sdl_video);
        p2p_sdl_audio_destroy(&ctx->sdl_audio);
        p2p_sdl_quit();
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
            .on_ice_offer = on_sig_ice_offer,
            .on_ice_candidate = on_sig_ice_candidate,
            .on_gathering_done = on_sig_gathering_done,
        },
        .user_data = ctx
    };

    if (p2p_signaling_connect(&ctx->sig_client, &scfg) != 0) {
        fprintf(stderr, "[peer] signaling connect failed\n");
        p2p_engine_stop(&ctx->engine);
        p2p_engine_destroy(&ctx->engine);
        p2p_sdl_video_destroy(&ctx->sdl_video);
        p2p_sdl_audio_destroy(&ctx->sdl_audio);
        p2p_sdl_quit();
        return 1;
    }

    /* Main loop: SDL event processing + rendering (must run on main thread) */
    ctx->running = 1;
    while (ctx->running) {
        if (p2p_sdl_poll_events() < 0) {
            ctx->running = 0;
            break;
        }

        /* Render decoded video frame if available */
        p2p_mutex_lock(&ctx->frame_mutex);
        if (ctx->vframe.ready) {
            p2p_sdl_video_display(&ctx->sdl_video,
                ctx->vframe.y, ctx->vframe.u, ctx->vframe.v,
                ctx->vframe.lsy, ctx->vframe.lsu, ctx->vframe.lsv,
                ctx->vframe.w, ctx->vframe.h);
            ctx->vframe.ready = 0;
        }
        if (ctx->aframe.ready) {
            p2p_sdl_audio_play(&ctx->sdl_audio,
                ctx->aframe.samples, ctx->aframe.count);
            ctx->aframe.ready = 0;
        }
        p2p_mutex_unlock(&ctx->frame_mutex);

        p2p_sleep_ms(5);
    }

    fprintf(stderr, "[peer] shutting down...\n");

    /* Cleanup */
    p2p_signaling_disconnect(&ctx->sig_client);
    p2p_engine_stop(&ctx->engine);
    p2p_engine_destroy(&ctx->engine);

    if (ctx->vdec_open) p2p_video_decoder_close(&ctx->vdec);
    if (ctx->adec_open) p2p_audio_decoder_close(&ctx->adec);

    if (ctx->sdl_initialized) {
        p2p_sdl_video_destroy(&ctx->sdl_video);
        p2p_sdl_audio_destroy(&ctx->sdl_audio);
        p2p_sdl_quit();
    }

    p2p_mutex_lock(&ctx->frame_mutex);
    free(ctx->vframe.y);
    free(ctx->vframe.u);
    free(ctx->vframe.v);
    free(ctx->aframe.samples);
    p2p_mutex_unlock(&ctx->frame_mutex);

    p2p_mutex_destroy(&ctx->reasm_mutex);
    p2p_mutex_destroy(&ctx->frame_mutex);

#ifdef _WIN32
    p2p_net_cleanup();
#endif

    fprintf(stderr, "[peer] done\n");
    return 0;
}
