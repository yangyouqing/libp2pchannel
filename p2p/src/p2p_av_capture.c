#include "p2p_av_capture.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

uint64_t p2p_capture_now_us(void)
{
    return p2p_monotonic_us();
}

/* ====================================================================
 *  Linux implementation -- V4L2 video, ALSA audio
 * ==================================================================== */
#ifdef __linux__

#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <linux/videodev2.h>
#include <alsa/asoundlib.h>

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

    /* Capture as MJPEG; video is always re-encoded to H.264 before sending. */
    struct v4l2_format fmt;
    memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width = cap->width;
    fmt.fmt.pix.height = cap->height;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_MJPEG;
    fmt.fmt.pix.field = V4L2_FIELD_NONE;

    if (xioctl(cap->fd, VIDIOC_S_FMT, &fmt) < 0 ||
        fmt.fmt.pix.pixelformat != V4L2_PIX_FMT_MJPEG) {
        fprintf(stderr, "[v4l2] VIDIOC_S_FMT MJPEG failed: %s\n", strerror(errno));
        close(cap->fd);
        return -1;
    }
    cap->pixfmt = P2P_V4L2_PIX_MJPEG;
    cap->v4l2_pixfmt = V4L2_PIX_FMT_MJPEG;
    cap->width = fmt.fmt.pix.width;
    cap->height = fmt.fmt.pix.height;
    cap->bytesperline = fmt.fmt.pix.bytesperline;
    cap->sizeimage = fmt.fmt.pix.sizeimage;

    /* Set frame rate */
    struct v4l2_streamparm parm;
    memset(&parm, 0, sizeof(parm));
    parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    parm.parm.capture.timeperframe.numerator = 1;
    parm.parm.capture.timeperframe.denominator = cap->fps;
    xioctl(cap->fd, VIDIOC_S_PARM, &parm);

    /* Request 4 mmap buffers to handle USB bandwidth fluctuations */
    struct v4l2_requestbuffers req;
    memset(&req, 0, sizeof(req));
    req.count = 4;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;
    if (xioctl(cap->fd, VIDIOC_REQBUFS, &req) < 0 || req.count < 1) {
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

    fprintf(stderr, "[v4l2] opened %s: %dx%d @%dfps fmt=MJPEG (H.264 encoded on send)\n",
            cfg->device, cap->width, cap->height, cap->fps);
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
    if (p2p_thread_create(&cap->thread, video_capture_thread, cap) != 0) {
        cap->running = 0;
        return -1;
    }
    return 0;
}

void p2p_video_capture_stop(p2p_video_capture_t *cap)
{
    if (!cap) return;
    cap->running = 0;
    p2p_thread_join(cap->thread);

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
    if (p2p_thread_create(&cap->thread, audio_capture_thread, cap) != 0) {
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
    p2p_thread_join(cap->thread);
}

void p2p_audio_capture_close(p2p_audio_capture_t *cap)
{
    if (!cap) return;
    if (cap->pcm_handle) {
        snd_pcm_close((snd_pcm_t *)cap->pcm_handle);
        cap->pcm_handle = NULL;
    }
}

/* ====================================================================
 *  Windows implementation -- FFmpeg dshow video & audio capture
 * ==================================================================== */
#elif defined(_WIN32)

#include <libavdevice/avdevice.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswresample/swresample.h>

/* ======== dshow Video Capture ======== */

int p2p_video_capture_open(p2p_video_capture_t *cap, const p2p_video_capture_config_t *cfg)
{
    if (!cap || !cfg || !cfg->device) return -1;
    memset(cap, 0, sizeof(*cap));
    cap->cb = cfg->cb;
    cap->user_data = cfg->user_data;
    cap->width = cfg->width > 0 ? cfg->width : 640;
    cap->height = cfg->height > 0 ? cfg->height : 480;
    cap->fps = cfg->fps > 0 ? cfg->fps : 30;

    avdevice_register_all();

    const AVInputFormat *ifmt = av_find_input_format("dshow");
    if (!ifmt) {
        fprintf(stderr, "[dshow] dshow input format not found\n");
        return -1;
    }

    char device_url[512];
    snprintf(device_url, sizeof(device_url), "video=%s", cfg->device);

    AVDictionary *opts = NULL;
    char size_str[32], fps_str[16];
    snprintf(size_str, sizeof(size_str), "%dx%d", cap->width, cap->height);
    snprintf(fps_str, sizeof(fps_str), "%d", cap->fps);
    av_dict_set(&opts, "video_size", size_str, 0);
    av_dict_set(&opts, "framerate", fps_str, 0);
    av_dict_set(&opts, "vcodec", "mjpeg", 0);

    AVFormatContext *fmt_ctx = NULL;
    int ret = avformat_open_input(&fmt_ctx, device_url, ifmt, &opts);
    av_dict_free(&opts);
    if (ret < 0) {
        char errbuf[128];
        av_strerror(ret, errbuf, sizeof(errbuf));
        fprintf(stderr, "[dshow] cannot open %s: %s\n", device_url, errbuf);
        return -1;
    }

    if (avformat_find_stream_info(fmt_ctx, NULL) < 0) {
        fprintf(stderr, "[dshow] cannot find stream info\n");
        avformat_close_input(&fmt_ctx);
        return -1;
    }

    int vidx = -1;
    for (unsigned i = 0; i < fmt_ctx->nb_streams; i++) {
        if (fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            vidx = (int)i;
            break;
        }
    }
    if (vidx < 0) {
        fprintf(stderr, "[dshow] no video stream found\n");
        avformat_close_input(&fmt_ctx);
        return -1;
    }

    cap->fmt_ctx = fmt_ctx;
    cap->video_stream = vidx;
    cap->width = fmt_ctx->streams[vidx]->codecpar->width;
    cap->height = fmt_ctx->streams[vidx]->codecpar->height;
    cap->pixfmt = P2P_V4L2_PIX_MJPEG;

    fprintf(stderr, "[dshow] opened %s: %dx%d @%dfps fmt=MJPEG (H.264 encoded on send)\n",
            cfg->device, cap->width, cap->height, cap->fps);
    return 0;
}

static void *video_capture_thread(void *arg)
{
    p2p_video_capture_t *cap = (p2p_video_capture_t *)arg;
    AVFormatContext *fmt_ctx = (AVFormatContext *)cap->fmt_ctx;
    AVPacket *pkt = av_packet_alloc();
    if (!pkt) return NULL;

    while (cap->running) {
        int ret = av_read_frame(fmt_ctx, pkt);
        if (ret < 0) {
            if (ret == AVERROR(EAGAIN)) { p2p_sleep_ms(1); continue; }
            fprintf(stderr, "[dshow] av_read_frame error: %d\n", ret);
            break;
        }
        if (pkt->stream_index == cap->video_stream && cap->cb) {
            uint64_t ts = p2p_capture_now_us();
            cap->cb(pkt->data, pkt->size,
                    cap->width, cap->height, cap->pixfmt, ts, cap->user_data);
        }
        av_packet_unref(pkt);
    }

    av_packet_free(&pkt);
    return NULL;
}

int p2p_video_capture_start(p2p_video_capture_t *cap)
{
    if (!cap || !cap->fmt_ctx) return -1;
    cap->running = 1;
    if (p2p_thread_create(&cap->thread, video_capture_thread, cap) != 0) {
        cap->running = 0;
        return -1;
    }
    return 0;
}

void p2p_video_capture_stop(p2p_video_capture_t *cap)
{
    if (!cap) return;
    cap->running = 0;
    p2p_thread_join(cap->thread);
}

void p2p_video_capture_close(p2p_video_capture_t *cap)
{
    if (!cap) return;
    if (cap->fmt_ctx) {
        AVFormatContext *fmt_ctx = (AVFormatContext *)cap->fmt_ctx;
        avformat_close_input(&fmt_ctx);
        cap->fmt_ctx = NULL;
    }
}

/* ======== dshow Audio Capture ======== */

int p2p_audio_capture_open(p2p_audio_capture_t *cap, const p2p_audio_capture_config_t *cfg)
{
    if (!cap || !cfg) return -1;
    memset(cap, 0, sizeof(*cap));
    cap->cb = cfg->cb;
    cap->user_data = cfg->user_data;
    cap->sample_rate = cfg->sample_rate > 0 ? cfg->sample_rate : 48000;
    cap->channels = cfg->channels > 0 ? cfg->channels : 1;
    cap->period_frames = cfg->period_frames > 0 ? cfg->period_frames : 960;

    avdevice_register_all();

    const AVInputFormat *ifmt = av_find_input_format("dshow");
    if (!ifmt) {
        fprintf(stderr, "[dshow] dshow input format not found\n");
        return -1;
    }

    const char *dev = cfg->device ? cfg->device : "default";
    char device_url[512];
    snprintf(device_url, sizeof(device_url), "audio=%s", dev);

    AVDictionary *opts = NULL;
    char sr_str[16], ch_str[8];
    snprintf(sr_str, sizeof(sr_str), "%d", cap->sample_rate);
    snprintf(ch_str, sizeof(ch_str), "%d", cap->channels);
    av_dict_set(&opts, "sample_rate", sr_str, 0);
    av_dict_set(&opts, "channels", ch_str, 0);
    av_dict_set(&opts, "sample_size", "16", 0);

    AVFormatContext *fmt_ctx = NULL;
    int ret = avformat_open_input(&fmt_ctx, device_url, ifmt, &opts);
    av_dict_free(&opts);
    if (ret < 0) {
        char errbuf[128];
        av_strerror(ret, errbuf, sizeof(errbuf));
        fprintf(stderr, "[dshow] cannot open audio %s: %s\n", device_url, errbuf);
        return -1;
    }

    if (avformat_find_stream_info(fmt_ctx, NULL) < 0) {
        fprintf(stderr, "[dshow] cannot find audio stream info\n");
        avformat_close_input(&fmt_ctx);
        return -1;
    }

    int aidx = -1;
    for (unsigned i = 0; i < fmt_ctx->nb_streams; i++) {
        if (fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            aidx = (int)i;
            break;
        }
    }
    if (aidx < 0) {
        fprintf(stderr, "[dshow] no audio stream found\n");
        avformat_close_input(&fmt_ctx);
        return -1;
    }

    cap->fmt_ctx = fmt_ctx;
    cap->audio_stream = aidx;
    cap->sample_rate = fmt_ctx->streams[aidx]->codecpar->sample_rate;
    cap->channels = fmt_ctx->streams[aidx]->codecpar->ch_layout.nb_channels;

    fprintf(stderr, "[dshow] opened audio %s: %dHz %dch\n",
            dev, cap->sample_rate, cap->channels);
    return 0;
}

static void *audio_capture_thread(void *arg)
{
    p2p_audio_capture_t *cap = (p2p_audio_capture_t *)arg;
    AVFormatContext *fmt_ctx = (AVFormatContext *)cap->fmt_ctx;
    AVPacket *pkt = av_packet_alloc();
    if (!pkt) return NULL;

    while (cap->running) {
        int ret = av_read_frame(fmt_ctx, pkt);
        if (ret < 0) {
            if (ret == AVERROR(EAGAIN)) { p2p_sleep_ms(1); continue; }
            fprintf(stderr, "[dshow] audio av_read_frame error: %d\n", ret);
            break;
        }
        if (pkt->stream_index == cap->audio_stream && cap->cb) {
            int num_samples = pkt->size / (cap->channels * sizeof(int16_t));
            uint64_t ts = p2p_capture_now_us();
            cap->cb((const int16_t *)pkt->data, num_samples,
                    cap->channels, cap->sample_rate, ts, cap->user_data);
        }
        av_packet_unref(pkt);
    }

    av_packet_free(&pkt);
    return NULL;
}

int p2p_audio_capture_start(p2p_audio_capture_t *cap)
{
    if (!cap || !cap->fmt_ctx) return -1;
    cap->running = 1;
    if (p2p_thread_create(&cap->thread, audio_capture_thread, cap) != 0) {
        cap->running = 0;
        return -1;
    }
    return 0;
}

void p2p_audio_capture_stop(p2p_audio_capture_t *cap)
{
    if (!cap) return;
    cap->running = 0;
    p2p_thread_join(cap->thread);
}

void p2p_audio_capture_close(p2p_audio_capture_t *cap)
{
    if (!cap) return;
    if (cap->fmt_ctx) {
        AVFormatContext *fmt_ctx = (AVFormatContext *)cap->fmt_ctx;
        avformat_close_input(&fmt_ctx);
        cap->fmt_ctx = NULL;
    }
}

#else
#error "Unsupported platform: define __linux__ or _WIN32"
#endif
