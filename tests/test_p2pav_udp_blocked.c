/*
 * test_p2pav_udp_blocked.c -- Long-running test for libp2pav under a
 *                             fully UDP-blocked environment.
 *
 * Runs as EITHER publisher or subscriber (--role flag), so two separate
 * processes handle ICE independently -- avoiding same-process ICE-TCP
 * timing issues.
 *
 * Usage (publisher):
 *   ./test_p2pav_udp_blocked --role publisher \
 *       --signaling HOST:PORT --stun HOST:PORT \
 *       --cert FILE --key FILE --token JWT --room ROOM \
 *       [--duration 600] [--enable-tcp]
 *
 * Usage (subscriber):
 *   ./test_p2pav_udp_blocked --role subscriber \
 *       --signaling HOST:PORT --stun HOST:PORT \
 *       --token JWT --room ROOM \
 *       [--duration 600] [--enable-tcp] [--result-file FILE]
 */

#include "p2pav.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>

#ifndef _WIN32
#include <sys/time.h>
#endif

#define DEFAULT_DURATION_SEC  600
#define STAT_INTERVAL_SEC     30
#define VIDEO_FRAME_SIZE      4096
#define AUDIO_FRAME_SIZE      160
#define VIDEO_INTERVAL_US     33333   /* ~30 fps */
#define AUDIO_INTERVAL_US     20000   /* ~50 fps */
#define WAIT_POLL_MS          100
#define ICE_TIMEOUT_MS        60000

typedef struct {
    const char *role;
    const char *signaling_url;
    const char *stun_server;
    const char *ssl_cert;
    const char *ssl_key;
    const char *token;
    const char *room;
    const char *result_file;
    int         duration_sec;
    int         enable_tcp;
} test_config_t;

static volatile int g_running         = 1;
static volatile int g_peer_ready      = 0;
static volatile int g_sig_connected   = 0;
static volatile int g_video_sent      = 0;
static volatile int g_audio_sent      = 0;
static volatile int g_video_recv      = 0;
static volatile int g_audio_recv      = 0;

static void on_signal(int sig) { (void)sig; g_running = 0; }

static uint64_t now_us(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000000ULL + (uint64_t)tv.tv_usec;
}

static void on_connected(p2pav_session_t *s, void *ud)
{
    (void)s; (void)ud;
    fprintf(stderr, "[%s] signaling connected\n", (const char *)ud);
    g_sig_connected = 1;
}

static void on_peer_ready(p2pav_session_t *s, const char *peer_id, void *ud)
{
    (void)s;
    fprintf(stderr, "[%s] peer '%s' ICE connected\n", (const char *)ud, peer_id);
    g_peer_ready = 1;
}

static void on_error(p2pav_session_t *s, p2pav_error_t err,
                     const char *detail, void *ud)
{
    (void)s;
    fprintf(stderr, "[%s] error: %s (%s)\n",
            (const char *)ud, p2pav_error_string(err), detail ? detail : "");
}

static void on_video_recv(p2pav_session_t *s, const char *from_peer,
                          const p2pav_video_frame_t *frame, void *ud)
{
    (void)s; (void)from_peer; (void)frame; (void)ud;
    g_video_recv++;
}

static void on_audio_recv(p2pav_session_t *s, const char *from_peer,
                          const p2pav_audio_frame_t *frame, void *ud)
{
    (void)s; (void)from_peer; (void)frame; (void)ud;
    g_audio_recv++;
}

static void usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s --role publisher|subscriber [OPTIONS]\n"
        "  --role ROLE            publisher or subscriber (required)\n"
        "  --signaling HOST:PORT  Signaling server (required)\n"
        "  --stun HOST:PORT       STUN/TURN server\n"
        "  --cert FILE            TLS certificate for QUIC\n"
        "  --key FILE             TLS private key for QUIC\n"
        "  --token JWT            JWT token (required)\n"
        "  --room ROOM            Room name (required)\n"
        "  --duration SEC         Test duration (default %d)\n"
        "  --enable-tcp           Enable ICE-TCP candidates\n"
        "  --result-file FILE     Write results to this file (subscriber)\n"
        "  --help                 Show this help\n",
        prog, DEFAULT_DURATION_SEC);
}

static void parse_args(int argc, char **argv, test_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->duration_sec = DEFAULT_DURATION_SEC;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--role") == 0 && i + 1 < argc)
            cfg->role = argv[++i];
        else if (strcmp(argv[i], "--signaling") == 0 && i + 1 < argc)
            cfg->signaling_url = argv[++i];
        else if (strcmp(argv[i], "--stun") == 0 && i + 1 < argc)
            cfg->stun_server = argv[++i];
        else if (strcmp(argv[i], "--cert") == 0 && i + 1 < argc)
            cfg->ssl_cert = argv[++i];
        else if (strcmp(argv[i], "--key") == 0 && i + 1 < argc)
            cfg->ssl_key = argv[++i];
        else if (strcmp(argv[i], "--token") == 0 && i + 1 < argc)
            cfg->token = argv[++i];
        else if (strcmp(argv[i], "--room") == 0 && i + 1 < argc)
            cfg->room = argv[++i];
        else if (strcmp(argv[i], "--duration") == 0 && i + 1 < argc)
            cfg->duration_sec = atoi(argv[++i]);
        else if (strcmp(argv[i], "--enable-tcp") == 0)
            cfg->enable_tcp = 1;
        else if (strcmp(argv[i], "--result-file") == 0 && i + 1 < argc)
            cfg->result_file = argv[++i];
        else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            usage(argv[0]); exit(0);
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            usage(argv[0]); exit(1);
        }
    }
}

static int run_publisher(const test_config_t *cfg)
{
    const char *tag = "pub";

    p2pav_session_config_t scfg = {0};
    scfg.signaling_url = cfg->signaling_url;
    scfg.auth_token    = cfg->token;
    scfg.room_id       = cfg->room;
    scfg.peer_id       = "pub1";
    scfg.role          = P2PAV_ROLE_PUBLISHER;
    scfg.stun_server   = cfg->stun_server;
    scfg.ssl_cert_file = cfg->ssl_cert;
    scfg.ssl_key_file  = cfg->ssl_key;
    scfg.enable_tcp    = cfg->enable_tcp;

    p2pav_session_t *sess = p2pav_session_create(&scfg);
    if (!sess) { fprintf(stderr, "[%s] FATAL: session create failed\n", tag); return 1; }

    p2pav_video_config_t vcfg = {
        .codec = P2PAV_CODEC_H264, .width = 640, .height = 480,
        .fps = 30, .bitrate_mode = P2PAV_BITRATE_MEDIUM,
        .reliability = P2PAV_SEMI_RELIABLE,
    };
    p2pav_video_set_config(sess, &vcfg);

    p2pav_audio_config_t acfg = {
        .codec = P2PAV_CODEC_OPUS, .sample_rate = 48000, .channels = 1,
        .bitrate_bps = 64000, .frame_duration_ms = 20,
        .reliability = P2PAV_UNRELIABLE,
    };
    p2pav_audio_set_config(sess, &acfg);

    p2pav_session_callbacks_t cbs = {
        .on_connected  = on_connected,
        .on_peer_ready = on_peer_ready,
        .on_error      = on_error,
    };
    p2pav_session_set_callbacks(sess, &cbs, (void *)tag);

    fprintf(stderr, "[%s] starting session (room=%s)...\n", tag, cfg->room);
    if (p2pav_session_start(sess) != P2PAV_OK) {
        fprintf(stderr, "[%s] FATAL: session start failed\n", tag);
        p2pav_session_destroy(sess);
        return 1;
    }

    fprintf(stderr, "[%s] waiting for subscriber ICE connection (timeout %ds)...\n",
            tag, ICE_TIMEOUT_MS / 1000);
    int elapsed_ms = 0;
    while (!g_peer_ready && elapsed_ms < ICE_TIMEOUT_MS && g_running) {
        usleep(WAIT_POLL_MS * 1000);
        elapsed_ms += WAIT_POLL_MS;
    }
    if (!g_peer_ready) {
        fprintf(stderr, "[%s] FATAL: ICE connection timeout after %d ms\n", tag, elapsed_ms);
        p2pav_session_stop(sess);
        p2pav_session_destroy(sess);
        return 1;
    }
    fprintf(stderr, "[%s] ICE connected in %d ms\n", tag, elapsed_ms);

    uint8_t dummy_video[VIDEO_FRAME_SIZE];
    uint8_t dummy_audio[AUDIO_FRAME_SIZE];
    memset(dummy_video, 0x42, sizeof(dummy_video));
    memset(dummy_audio, 0x11, sizeof(dummy_audio));

    uint64_t t_start    = now_us();
    uint64_t t_end      = t_start + (uint64_t)cfg->duration_sec * 1000000ULL;
    uint64_t t_last_vid = t_start;
    uint64_t t_last_aud = t_start;
    uint64_t t_last_stat = t_start;

    fprintf(stderr, "[%s] sending fake A/V for %ds...\n", tag, cfg->duration_sec);

    while (now_us() < t_end && g_running) {
        uint64_t t_now = now_us();

        if ((t_now - t_last_vid) >= VIDEO_INTERVAL_US) {
            p2pav_video_frame_t vf = {
                .data = dummy_video, .size = (int)sizeof(dummy_video),
                .timestamp_us = t_now,
                .is_keyframe = (g_video_sent % 30 == 0) ? 1 : 0,
            };
            p2pav_video_send(sess, &vf);
            g_video_sent++;
            t_last_vid = t_now;
        }

        if ((t_now - t_last_aud) >= AUDIO_INTERVAL_US) {
            p2pav_audio_frame_t af = {
                .data = dummy_audio, .size = (int)sizeof(dummy_audio),
                .timestamp_us = t_now,
            };
            p2pav_audio_send(sess, &af);
            g_audio_sent++;
            t_last_aud = t_now;
        }

        if ((t_now - t_last_stat) >= (uint64_t)STAT_INTERVAL_SEC * 1000000ULL) {
            int secs = (int)((t_now - t_start) / 1000000ULL);
            fprintf(stderr, "[%s] %4ds | video_sent=%d  audio_sent=%d\n",
                    tag, secs, g_video_sent, g_audio_sent);
            t_last_stat = t_now;
        }

        usleep(1000);
    }

    fprintf(stderr, "[%s] done sending. total: video=%d audio=%d\n",
            tag, g_video_sent, g_audio_sent);

    p2pav_session_stop(sess);
    p2pav_session_destroy(sess);
    return 0;
}

static int run_subscriber(const test_config_t *cfg)
{
    const char *tag = "sub";

    p2pav_session_config_t scfg = {0};
    scfg.signaling_url = cfg->signaling_url;
    scfg.auth_token    = cfg->token;
    scfg.room_id       = cfg->room;
    scfg.peer_id       = "sub1";
    scfg.role          = P2PAV_ROLE_SUBSCRIBER;
    scfg.stun_server   = cfg->stun_server;
    scfg.enable_tcp    = cfg->enable_tcp;

    p2pav_session_t *sess = p2pav_session_create(&scfg);
    if (!sess) { fprintf(stderr, "[%s] FATAL: session create failed\n", tag); return 1; }

    p2pav_video_config_t vcfg = {
        .codec = P2PAV_CODEC_H264, .width = 640, .height = 480,
        .fps = 30, .bitrate_mode = P2PAV_BITRATE_MEDIUM,
        .reliability = P2PAV_SEMI_RELIABLE,
    };
    p2pav_video_set_config(sess, &vcfg);

    p2pav_audio_config_t acfg = {
        .codec = P2PAV_CODEC_OPUS, .sample_rate = 48000, .channels = 1,
        .bitrate_bps = 64000, .frame_duration_ms = 20,
        .reliability = P2PAV_UNRELIABLE,
    };
    p2pav_audio_set_config(sess, &acfg);

    p2pav_video_set_recv_callback(sess, on_video_recv, NULL);
    p2pav_audio_set_recv_callback(sess, on_audio_recv, NULL);

    p2pav_session_callbacks_t cbs = {
        .on_connected  = on_connected,
        .on_peer_ready = on_peer_ready,
        .on_error      = on_error,
    };
    p2pav_session_set_callbacks(sess, &cbs, (void *)tag);

    fprintf(stderr, "[%s] starting session (room=%s)...\n", tag, cfg->room);
    if (p2pav_session_start(sess) != P2PAV_OK) {
        fprintf(stderr, "[%s] FATAL: session start failed\n", tag);
        p2pav_session_destroy(sess);
        return 1;
    }

    fprintf(stderr, "[%s] waiting for publisher ICE connection (timeout %ds)...\n",
            tag, ICE_TIMEOUT_MS / 1000);
    int elapsed_ms = 0;
    while (!g_peer_ready && elapsed_ms < ICE_TIMEOUT_MS && g_running) {
        usleep(WAIT_POLL_MS * 1000);
        elapsed_ms += WAIT_POLL_MS;
    }
    if (!g_peer_ready) {
        fprintf(stderr, "[%s] FATAL: ICE connection timeout after %d ms\n", tag, elapsed_ms);
        p2pav_session_stop(sess);
        p2pav_session_destroy(sess);
        return 1;
    }
    fprintf(stderr, "[%s] ICE connected in %d ms\n", tag, elapsed_ms);

    uint64_t t_start    = now_us();
    uint64_t t_end      = t_start + (uint64_t)cfg->duration_sec * 1000000ULL;
    uint64_t t_last_stat = t_start;

    fprintf(stderr, "[%s] receiving data for %ds...\n", tag, cfg->duration_sec);

    while (now_us() < t_end && g_running) {
        uint64_t t_now = now_us();

        if ((t_now - t_last_stat) >= (uint64_t)STAT_INTERVAL_SEC * 1000000ULL) {
            int secs = (int)((t_now - t_start) / 1000000ULL);
            fprintf(stderr, "[%s] %4ds | video_recv=%d  audio_recv=%d\n",
                    tag, secs, g_video_recv, g_audio_recv);
            t_last_stat = t_now;
        }

        usleep(WAIT_POLL_MS * 1000);
    }

    int total_sec = (int)((now_us() - t_start) / 1000000ULL);
    fprintf(stderr, "[%s] done receiving. video=%d audio=%d elapsed=%ds\n",
            tag, g_video_recv, g_audio_recv, total_sec);

    if (cfg->result_file) {
        FILE *f = fopen(cfg->result_file, "w");
        if (f) {
            fprintf(f, "video_recv=%d\naudio_recv=%d\nelapsed=%d\n",
                    g_video_recv, g_audio_recv, total_sec);
            fclose(f);
        }
    }

    int pass = (g_video_recv > 0 && g_audio_recv > 0 &&
                total_sec >= cfg->duration_sec - 5);

    p2pav_session_stop(sess);
    p2pav_session_destroy(sess);
    return pass ? 0 : 1;
}

int main(int argc, char *argv[])
{
    signal(SIGINT,  on_signal);
    signal(SIGTERM, on_signal);

    test_config_t cfg;
    parse_args(argc, argv, &cfg);

    if (!cfg.role || !cfg.signaling_url || !cfg.token || !cfg.room) {
        fprintf(stderr, "Error: --role, --signaling, --token, and --room are required.\n");
        usage(argv[0]);
        return 1;
    }

    p2pav_set_log_level(P2PAV_LOG_DEBUG);

    printf("==============================================\n");
    printf("  libp2pav UDP-Blocked %s (%s)\n", cfg.role, p2pav_version_string());
    printf("==============================================\n");
    printf("Signaling:  %s\n", cfg.signaling_url);
    printf("STUN/TURN:  %s\n", cfg.stun_server ? cfg.stun_server : "(signaling-provided)");
    printf("ICE-TCP:    %s\n", cfg.enable_tcp ? "ENABLED" : "disabled");
    printf("Room:       %s\n", cfg.room);
    printf("Duration:   %d seconds\n\n", cfg.duration_sec);

    if (strcmp(cfg.role, "publisher") == 0)
        return run_publisher(&cfg);
    else if (strcmp(cfg.role, "subscriber") == 0)
        return run_subscriber(&cfg);

    fprintf(stderr, "Unknown role: %s (use publisher or subscriber)\n", cfg.role);
    return 1;
}
