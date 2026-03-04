/*
 * p2pav_publisher_example.c -- Demo publisher using libp2pav.
 *
 * Sends synthetic video/audio frames to demonstrate the API.
 * In a real application, replace with actual capture + encode output.
 *
 * Build:
 *   cc -o pub_example p2pav_publisher_example.c -lp2pav -ljuice -lxquic ...
 */

#include "p2pav.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>

static volatile int g_running = 1;

static void on_signal(int sig) { (void)sig; g_running = 0; }

static void on_connected(p2pav_session_t *s, void *ud)
{
    printf("[pub] connected to signaling server\n");
}

static void on_peer_ready(p2pav_session_t *s, const char *peer_id, void *ud)
{
    printf("[pub] peer '%s' ready (ICE connected)\n", peer_id);
}

static void on_keyframe_request(p2pav_session_t *s, const char *from_peer, void *ud)
{
    printf("[pub] keyframe requested by '%s'\n", from_peer);
}

static void on_error(p2pav_session_t *s, p2pav_error_t err, const char *detail, void *ud)
{
    fprintf(stderr, "[pub] error: %s (%s)\n", p2pav_error_string(err), detail ? detail : "");
}

int main(int argc, char *argv[])
{
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    printf("libp2pav publisher example (%s)\n", p2pav_version_string());

    p2pav_set_log_level(P2PAV_LOG_INFO);

    p2pav_session_config_t cfg = {
        .signaling_url = "127.0.0.1:8443",
        .room_id       = "test-room",
        .peer_id       = "pub1",
        .role          = P2PAV_ROLE_PUBLISHER,
        .stun_server   = "127.0.0.1:3478",
        .ssl_cert_file = "./server.crt",
        .ssl_key_file  = "./server.key",
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

    p2pav_video_set_keyframe_request_callback(session, on_keyframe_request, NULL);

    p2pav_video_config_t vcfg = {
        .codec        = P2PAV_CODEC_H264,
        .width        = 640,
        .height       = 480,
        .fps          = 30,
        .bitrate_mode = P2PAV_BITRATE_MEDIUM,
        .reliability  = P2PAV_SEMI_RELIABLE,
    };
    p2pav_video_set_config(session, &vcfg);

    p2pav_audio_config_t acfg = {
        .codec             = P2PAV_CODEC_OPUS,
        .sample_rate       = 48000,
        .channels          = 1,
        .bitrate_bps       = 64000,
        .frame_duration_ms = 20,
        .reliability       = P2PAV_UNRELIABLE,
    };
    p2pav_audio_set_config(session, &acfg);

    p2pav_error_t err = p2pav_session_start(session);
    if (err != P2PAV_OK) {
        fprintf(stderr, "session start failed: %s\n", p2pav_error_string(err));
        p2pav_session_destroy(session);
        return 1;
    }

    printf("[pub] session started, sending synthetic frames...\n");

    uint32_t frame_count = 0;
    uint8_t dummy_video[4096];
    uint8_t dummy_audio[960];
    memset(dummy_video, 0x42, sizeof(dummy_video));
    memset(dummy_audio, 0, sizeof(dummy_audio));

    while (g_running) {
        usleep(33333);   /* ~30 fps */
        frame_count++;

        p2pav_video_frame_t vf = {
            .data         = dummy_video,
            .size         = sizeof(dummy_video),
            .timestamp_us = (uint64_t)frame_count * 33333,
            .is_keyframe  = (frame_count % 30 == 1) ? 1 : 0,
        };
        p2pav_video_send(session, &vf);

        p2pav_audio_frame_t af = {
            .data         = dummy_audio,
            .size         = sizeof(dummy_audio),
            .timestamp_us = (uint64_t)frame_count * 33333,
        };
        p2pav_audio_send(session, &af);

        if (frame_count % 150 == 0) {
            p2pav_timing_t t;
            p2pav_get_timing(session, &t);
            printf("[pub] frames=%u  peers=%d\n",
                   frame_count, p2pav_session_get_peer_count(session));
        }
    }

    printf("[pub] stopping...\n");
    p2pav_session_stop(session);
    p2pav_session_destroy(session);
    printf("[pub] done\n");
    return 0;
}
