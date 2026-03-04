/*
 * p2pav_subscriber_example.c -- Demo subscriber using libp2pav.
 *
 * Receives video/audio frames and prints stats.
 * In a real application, decode and render the received frames.
 *
 * Build:
 *   cc -o sub_example p2pav_subscriber_example.c -lp2pav -ljuice -lxquic ...
 */

#include "p2pav.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>

static volatile int g_running = 1;
static uint32_t g_video_frames = 0;
static uint32_t g_audio_frames = 0;

static void on_signal(int sig) { (void)sig; g_running = 0; }

static void on_connected(p2pav_session_t *s, void *ud)
{
    printf("[sub] connected to signaling server\n");
}

static void on_peer_ready(p2pav_session_t *s, const char *peer_id, void *ud)
{
    printf("[sub] peer '%s' ready (ICE connected)\n", peer_id);
}

static void on_video_frame(p2pav_session_t *s, const char *from_peer,
                            const p2pav_video_frame_t *frame, void *ud)
{
    g_video_frames++;
    if (g_video_frames % 30 == 1) {
        printf("[sub] video from '%s': size=%d %s  (total=%u)\n",
               from_peer, frame->size,
               frame->is_keyframe ? "IDR" : "P",
               g_video_frames);
    }
}

static void on_audio_frame(p2pav_session_t *s, const char *from_peer,
                            const p2pav_audio_frame_t *frame, void *ud)
{
    g_audio_frames++;
}

static void on_stats(p2pav_session_t *s, const char *peer_id,
                      const p2pav_net_stats_t *stats, void *ud)
{
    printf("[sub] stats for '%s': rtt=%u us  path=%s  loss=%.2f%%\n",
           peer_id, stats->rtt_us,
           stats->path_type == P2PAV_PATH_DIRECT ? "DIRECT" :
           stats->path_type == P2PAV_PATH_RELAY  ? "RELAY"  : "UNKNOWN",
           stats->packet_loss_rate * 100.0);
}

static void on_error(p2pav_session_t *s, p2pav_error_t err, const char *detail, void *ud)
{
    fprintf(stderr, "[sub] error: %s (%s)\n", p2pav_error_string(err), detail ? detail : "");
}

int main(int argc, char *argv[])
{
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    printf("libp2pav subscriber example (%s)\n", p2pav_version_string());

    p2pav_set_log_level(P2PAV_LOG_INFO);

    p2pav_session_config_t cfg = {
        .signaling_url = "127.0.0.1:8443",
        .room_id       = "test-room",
        .peer_id       = "sub1",
        .role          = P2PAV_ROLE_SUBSCRIBER,
        .stun_server   = "127.0.0.1:3478",
    };

    p2pav_session_t *session = p2pav_session_create(&cfg);
    if (!session) {
        fprintf(stderr, "session create failed\n");
        return 1;
    }

    p2pav_session_callbacks_t cbs = {
        .on_connected  = on_connected,
        .on_peer_ready = on_peer_ready,
        .on_error      = on_error,
    };
    p2pav_session_set_callbacks(session, &cbs, NULL);

    p2pav_video_set_recv_callback(session, on_video_frame, NULL);
    p2pav_audio_set_recv_callback(session, on_audio_frame, NULL);

    p2pav_set_net_stats_callback(session, on_stats, 5000, NULL);

    p2pav_error_t err = p2pav_session_start(session);
    if (err != P2PAV_OK) {
        fprintf(stderr, "session start failed: %s\n", p2pav_error_string(err));
        p2pav_session_destroy(session);
        return 1;
    }

    printf("[sub] session started, waiting for frames...\n");

    while (g_running) {
        usleep(100000);  /* 100ms */

        if (g_video_frames > 0 && g_video_frames % 300 == 0) {
            p2pav_timing_t t;
            p2pav_get_timing(session, &t);
            if (t.first_video_recv_us > 0 && t.session_start_us > 0) {
                uint64_t first_frame_ms = (t.first_video_recv_us - t.session_start_us) / 1000;
                printf("[sub] first-frame latency: %llu ms\n",
                       (unsigned long long)first_frame_ms);
            }
        }
    }

    printf("[sub] stopping... (video=%u audio=%u)\n", g_video_frames, g_audio_frames);
    p2pav_session_stop(session);
    p2pav_session_destroy(session);
    printf("[sub] done\n");
    return 0;
}
