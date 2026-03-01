#ifndef P2P_AV_CAPTURE_H
#define P2P_AV_CAPTURE_H

#include <stdint.h>
#include <stddef.h>
#include <pthread.h>

#ifdef __cplusplus
extern "C" {
#endif

/* V4L2 pixel formats we request, in preference order */
#define P2P_V4L2_PIX_MJPEG  0
#define P2P_V4L2_PIX_YUYV   1

typedef void (*p2p_video_frame_cb_t)(const uint8_t *data, size_t size,
                                     int width, int height, int pixfmt,
                                     uint64_t timestamp_us, void *user_data);

typedef void (*p2p_audio_frame_cb_t)(const int16_t *samples, int num_samples,
                                     int channels, int sample_rate,
                                     uint64_t timestamp_us, void *user_data);

/* ---- V4L2 video capture ---- */

typedef struct {
    const char *device;       /* e.g. "/dev/video0" */
    int         width;
    int         height;
    int         fps;
    p2p_video_frame_cb_t cb;
    void       *user_data;
} p2p_video_capture_config_t;

typedef struct {
    int         fd;
    int         width;
    int         height;
    int         fps;
    int         pixfmt;       /* P2P_V4L2_PIX_* */
    uint32_t    v4l2_pixfmt;  /* V4L2_PIX_FMT_* value */

    struct {
        void   *start;
        size_t  length;
    } buffers[4];
    int         n_buffers;

    pthread_t   thread;
    volatile int running;

    p2p_video_frame_cb_t cb;
    void       *user_data;
} p2p_video_capture_t;

int  p2p_video_capture_open(p2p_video_capture_t *cap, const p2p_video_capture_config_t *cfg);
int  p2p_video_capture_start(p2p_video_capture_t *cap);
void p2p_video_capture_stop(p2p_video_capture_t *cap);
void p2p_video_capture_close(p2p_video_capture_t *cap);

/* ---- ALSA audio capture ---- */

typedef struct {
    const char *device;       /* e.g. "default" */
    int         sample_rate;  /* e.g. 48000 */
    int         channels;     /* 1 = mono */
    int         period_frames;/* e.g. 960 (20ms at 48kHz) */
    p2p_audio_frame_cb_t cb;
    void       *user_data;
} p2p_audio_capture_config_t;

typedef struct {
    void       *pcm_handle;  /* snd_pcm_t* */
    int         sample_rate;
    int         channels;
    int         period_frames;

    pthread_t   thread;
    volatile int running;

    p2p_audio_frame_cb_t cb;
    void       *user_data;
} p2p_audio_capture_t;

int  p2p_audio_capture_open(p2p_audio_capture_t *cap, const p2p_audio_capture_config_t *cfg);
int  p2p_audio_capture_start(p2p_audio_capture_t *cap);
void p2p_audio_capture_stop(p2p_audio_capture_t *cap);
void p2p_audio_capture_close(p2p_audio_capture_t *cap);

/* Utility */
uint64_t p2p_capture_now_us(void);

#ifdef __cplusplus
}
#endif

#endif
