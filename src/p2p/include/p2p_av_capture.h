#ifndef P2P_AV_CAPTURE_H
#define P2P_AV_CAPTURE_H

#include <stdint.h>
#include <stddef.h>
#include "p2p_platform.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Pixel format tags passed to the video encoder */
#define P2P_V4L2_PIX_MJPEG   0
#define P2P_V4L2_PIX_YUYV    1
#define P2P_V4L2_PIX_YUV420P 2

typedef void (*p2p_video_frame_cb_t)(const uint8_t *data, size_t size,
                                     int width, int height, int pixfmt,
                                     uint64_t timestamp_us, void *user_data);

typedef void (*p2p_audio_frame_cb_t)(const int16_t *samples, int num_samples,
                                     int channels, int sample_rate,
                                     uint64_t timestamp_us, void *user_data);

/* ---- Video capture config ---- */

typedef struct {
    const char *device;       /* Linux: "/dev/video0"  Windows: "Integrated Camera" */
    int         width;
    int         height;
    int         fps;
    p2p_video_frame_cb_t cb;
    void       *user_data;
} p2p_video_capture_config_t;

/* ---- Video capture context ---- */

typedef struct {
#if defined(__linux__) && !defined(__ANDROID__)
    int         fd;
    uint32_t    v4l2_pixfmt;
    uint32_t    bytesperline;
    uint32_t    sizeimage;
    struct {
        void   *start;
        size_t  length;
    } buffers[4];
    int         n_buffers;
    void       *pipe_handle;    /* FILE* for rpicam pipe capture */
#elif defined(_WIN32)
    void       *fmt_ctx;        /* AVFormatContext* */
    int         video_stream;   /* stream index */
#endif
    int         width;
    int         height;
    int         fps;
    int         pixfmt;         /* P2P_V4L2_PIX_* */

    p2p_thread_t   thread;
    volatile int   running;

    p2p_video_frame_cb_t cb;
    void       *user_data;
} p2p_video_capture_t;

int  p2p_video_capture_open(p2p_video_capture_t *cap, const p2p_video_capture_config_t *cfg);
int  p2p_video_capture_start(p2p_video_capture_t *cap);
void p2p_video_capture_stop(p2p_video_capture_t *cap);
void p2p_video_capture_close(p2p_video_capture_t *cap);

/* ---- Audio capture config ---- */

typedef struct {
    const char *device;       /* Linux: "default"  Windows: "Microphone (Realtek)" */
    int         sample_rate;
    int         channels;
    int         period_frames;
    p2p_audio_frame_cb_t cb;
    void       *user_data;
} p2p_audio_capture_config_t;

/* ---- Audio capture context ---- */

typedef struct {
#if defined(__linux__) && !defined(__ANDROID__)
    void       *pcm_handle;    /* snd_pcm_t* */
#elif defined(_WIN32)
    void       *fmt_ctx;       /* AVFormatContext* */
    int         audio_stream;  /* stream index */
#endif
    int         sample_rate;
    int         channels;
    int         period_frames;

    p2p_thread_t   thread;
    volatile int   running;

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
