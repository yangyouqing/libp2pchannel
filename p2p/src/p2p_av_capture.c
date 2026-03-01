#include "p2p_av_capture.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <linux/videodev2.h>
#include <alsa/asoundlib.h>

uint64_t p2p_capture_now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}

/* ======== V4L2 Video Capture ======== */

static int xioctl(int fd, unsigned long request, void *arg)
{
    int r;
    do { r = ioctl(fd, request, arg); } while (r == -1 && errno == EINTR);
    return r;
}

int p2p_video_capture_open(p2p_video_capture_t *cap, const p2p_video_capture_config_t *cfg)
{
    if (!cap || !cfg || !cfg->device) return -1;
    memset(cap, 0, sizeof(*cap));
    cap->cb = cfg->cb;
    cap->user_data = cfg->user_data;
    cap->width = cfg->width > 0 ? cfg->width : 640;
    cap->height = cfg->height > 0 ? cfg->height : 480;
    cap->fps = cfg->fps > 0 ? cfg->fps : 30;

    cap->fd = open(cfg->device, O_RDWR | O_NONBLOCK);
    if (cap->fd < 0) {
        fprintf(stderr, "[v4l2] cannot open %s: %s\n", cfg->device, strerror(errno));
        return -1;
    }

    /* Try MJPEG first, fall back to YUYV */
    struct v4l2_format fmt;
    memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width = cap->width;
    fmt.fmt.pix.height = cap->height;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_MJPEG;
    fmt.fmt.pix.field = V4L2_FIELD_NONE;

    if (xioctl(cap->fd, VIDIOC_S_FMT, &fmt) == 0 &&
        fmt.fmt.pix.pixelformat == V4L2_PIX_FMT_MJPEG) {
        cap->pixfmt = P2P_V4L2_PIX_MJPEG;
        cap->v4l2_pixfmt = V4L2_PIX_FMT_MJPEG;
    } else {
        fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
        if (xioctl(cap->fd, VIDIOC_S_FMT, &fmt) < 0) {
            fprintf(stderr, "[v4l2] VIDIOC_S_FMT failed: %s\n", strerror(errno));
            close(cap->fd);
            return -1;
        }
        cap->pixfmt = P2P_V4L2_PIX_YUYV;
        cap->v4l2_pixfmt = V4L2_PIX_FMT_YUYV;
    }
    cap->width = fmt.fmt.pix.width;
    cap->height = fmt.fmt.pix.height;

    /* Set frame rate */
    struct v4l2_streamparm parm;
    memset(&parm, 0, sizeof(parm));
    parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    parm.parm.capture.timeperframe.numerator = 1;
    parm.parm.capture.timeperframe.denominator = cap->fps;
    xioctl(cap->fd, VIDIOC_S_PARM, &parm);

    /* Request 2 mmap buffers for low latency */
    struct v4l2_requestbuffers req;
    memset(&req, 0, sizeof(req));
    req.count = 2;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;
    if (xioctl(cap->fd, VIDIOC_REQBUFS, &req) < 0 || req.count < 2) {
        fprintf(stderr, "[v4l2] VIDIOC_REQBUFS failed\n");
        close(cap->fd);
        return -1;
    }
    cap->n_buffers = req.count;

    for (int i = 0; i < cap->n_buffers; i++) {
        struct v4l2_buffer buf;
        memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;
        if (xioctl(cap->fd, VIDIOC_QUERYBUF, &buf) < 0) {
            fprintf(stderr, "[v4l2] VIDIOC_QUERYBUF failed\n");
            close(cap->fd);
            return -1;
        }
        cap->buffers[i].length = buf.length;
        cap->buffers[i].start = mmap(NULL, buf.length,
            PROT_READ | PROT_WRITE, MAP_SHARED, cap->fd, buf.m.offset);
        if (cap->buffers[i].start == MAP_FAILED) {
            fprintf(stderr, "[v4l2] mmap failed\n");
            close(cap->fd);
            return -1;
        }
    }

    fprintf(stderr, "[v4l2] opened %s: %dx%d @%dfps fmt=%s\n",
            cfg->device, cap->width, cap->height, cap->fps,
            cap->pixfmt == P2P_V4L2_PIX_MJPEG ? "MJPEG" : "YUYV");
    return 0;
}

static void *video_capture_thread(void *arg)
{
    p2p_video_capture_t *cap = (p2p_video_capture_t *)arg;

    while (cap->running) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(cap->fd, &fds);
        struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };

        int r = select(cap->fd + 1, &fds, NULL, NULL, &tv);
        if (r <= 0) continue;

        struct v4l2_buffer buf;
        memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;

        if (xioctl(cap->fd, VIDIOC_DQBUF, &buf) < 0) {
            if (errno == EAGAIN) continue;
            fprintf(stderr, "[v4l2] DQBUF error: %s\n", strerror(errno));
            break;
        }

        uint64_t ts = p2p_capture_now_us();
        if (cap->cb) {
            cap->cb(cap->buffers[buf.index].start, buf.bytesused,
                    cap->width, cap->height, cap->pixfmt, ts, cap->user_data);
        }

        if (xioctl(cap->fd, VIDIOC_QBUF, &buf) < 0) {
            fprintf(stderr, "[v4l2] QBUF error: %s\n", strerror(errno));
            break;
        }
    }
    return NULL;
}

int p2p_video_capture_start(p2p_video_capture_t *cap)
{
    if (!cap || cap->fd < 0) return -1;

    for (int i = 0; i < cap->n_buffers; i++) {
        struct v4l2_buffer buf;
        memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;
        if (xioctl(cap->fd, VIDIOC_QBUF, &buf) < 0) return -1;
    }

    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (xioctl(cap->fd, VIDIOC_STREAMON, &type) < 0) return -1;

    cap->running = 1;
    if (pthread_create(&cap->thread, NULL, video_capture_thread, cap) != 0) {
        cap->running = 0;
        return -1;
    }
    return 0;
}

void p2p_video_capture_stop(p2p_video_capture_t *cap)
{
    if (!cap) return;
    cap->running = 0;
    pthread_join(cap->thread, NULL);

    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    xioctl(cap->fd, VIDIOC_STREAMOFF, &type);
}

void p2p_video_capture_close(p2p_video_capture_t *cap)
{
    if (!cap) return;
    for (int i = 0; i < cap->n_buffers; i++) {
        if (cap->buffers[i].start && cap->buffers[i].start != MAP_FAILED)
            munmap(cap->buffers[i].start, cap->buffers[i].length);
    }
    if (cap->fd >= 0) {
        close(cap->fd);
        cap->fd = -1;
    }
}

/* ======== ALSA Audio Capture ======== */

int p2p_audio_capture_open(p2p_audio_capture_t *cap, const p2p_audio_capture_config_t *cfg)
{
    if (!cap || !cfg) return -1;
    memset(cap, 0, sizeof(*cap));
    cap->cb = cfg->cb;
    cap->user_data = cfg->user_data;
    cap->sample_rate = cfg->sample_rate > 0 ? cfg->sample_rate : 48000;
    cap->channels = cfg->channels > 0 ? cfg->channels : 1;
    cap->period_frames = cfg->period_frames > 0 ? cfg->period_frames : 960;

    const char *dev = cfg->device ? cfg->device : "default";
    snd_pcm_t *pcm = NULL;

    int err = snd_pcm_open(&pcm, dev, SND_PCM_STREAM_CAPTURE, 0);
    if (err < 0) {
        fprintf(stderr, "[alsa] cannot open %s: %s\n", dev, snd_strerror(err));
        return -1;
    }

    snd_pcm_hw_params_t *hw;
    snd_pcm_hw_params_alloca(&hw);
    snd_pcm_hw_params_any(pcm, hw);
    snd_pcm_hw_params_set_access(pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED);
    snd_pcm_hw_params_set_format(pcm, hw, SND_PCM_FORMAT_S16_LE);

    unsigned int rate = cap->sample_rate;
    snd_pcm_hw_params_set_rate_near(pcm, hw, &rate, NULL);
    cap->sample_rate = rate;

    snd_pcm_hw_params_set_channels(pcm, hw, cap->channels);

    snd_pcm_uframes_t period = cap->period_frames;
    snd_pcm_hw_params_set_period_size_near(pcm, hw, &period, NULL);
    cap->period_frames = period;

    snd_pcm_uframes_t bufsize = period * 4;
    snd_pcm_hw_params_set_buffer_size_near(pcm, hw, &bufsize);

    if ((err = snd_pcm_hw_params(pcm, hw)) < 0) {
        fprintf(stderr, "[alsa] hw_params failed: %s\n", snd_strerror(err));
        snd_pcm_close(pcm);
        return -1;
    }

    cap->pcm_handle = pcm;
    fprintf(stderr, "[alsa] opened %s: %dHz %dch period=%d\n",
            dev, cap->sample_rate, cap->channels, cap->period_frames);
    return 0;
}

static void *audio_capture_thread(void *arg)
{
    p2p_audio_capture_t *cap = (p2p_audio_capture_t *)arg;
    snd_pcm_t *pcm = (snd_pcm_t *)cap->pcm_handle;

    int buf_samples = cap->period_frames * cap->channels;
    int16_t *buf = malloc(buf_samples * sizeof(int16_t));
    if (!buf) return NULL;

    while (cap->running) {
        snd_pcm_sframes_t frames = snd_pcm_readi(pcm, buf, cap->period_frames);
        if (frames < 0) {
            frames = snd_pcm_recover(pcm, frames, 0);
            if (frames < 0) {
                fprintf(stderr, "[alsa] read error: %s\n", snd_strerror(frames));
                break;
            }
            continue;
        }
        if (frames > 0 && cap->cb) {
            uint64_t ts = p2p_capture_now_us();
            cap->cb(buf, frames, cap->channels, cap->sample_rate, ts, cap->user_data);
        }
    }

    free(buf);
    return NULL;
}

int p2p_audio_capture_start(p2p_audio_capture_t *cap)
{
    if (!cap || !cap->pcm_handle) return -1;
    cap->running = 1;
    if (pthread_create(&cap->thread, NULL, audio_capture_thread, cap) != 0) {
        cap->running = 0;
        return -1;
    }
    return 0;
}

void p2p_audio_capture_stop(p2p_audio_capture_t *cap)
{
    if (!cap) return;
    cap->running = 0;
    if (cap->pcm_handle)
        snd_pcm_abort((snd_pcm_t *)cap->pcm_handle);
    pthread_join(cap->thread, NULL);
}

void p2p_audio_capture_close(p2p_audio_capture_t *cap)
{
    if (!cap) return;
    if (cap->pcm_handle) {
        snd_pcm_close((snd_pcm_t *)cap->pcm_handle);
        cap->pcm_handle = NULL;
    }
}
