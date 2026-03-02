#include "p2p_av_codec.h"
#include "p2p_av_capture.h"  /* for P2P_V4L2_PIX_* constants */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libavutil/imgutils.h>
#include <libavutil/channel_layout.h>
#include <libavutil/log.h>
#include <libswscale/swscale.h>

static int s_av_log_initialized = 0;

/* ======== Video Encoder (H.264 libx264) ======== */

int p2p_video_encoder_open(p2p_video_encoder_t *enc, const p2p_video_encoder_config_t *cfg)
{
    if (!enc || !cfg) return -1;
    memset(enc, 0, sizeof(*enc));

    if (!s_av_log_initialized) {
        av_log_set_level(AV_LOG_FATAL);
        s_av_log_initialized = 1;
    }

    enc->width = cfg->width > 0 ? cfg->width : 640;
    enc->height = cfg->height > 0 ? cfg->height : 480;
    int fps = cfg->fps > 0 ? cfg->fps : 30;
    int bitrate = cfg->bitrate > 0 ? cfg->bitrate : 1000000;
    int gop = cfg->gop_size > 0 ? cfg->gop_size : fps;

    const AVCodec *codec = avcodec_find_encoder_by_name("libx264");
    if (!codec) {
        codec = avcodec_find_encoder(AV_CODEC_ID_H264);
        if (!codec) {
            fprintf(stderr, "[venc] H.264 encoder not found\n");
            return -1;
        }
    }

    AVCodecContext *ctx = avcodec_alloc_context3(codec);
    if (!ctx) return -1;

    ctx->width = enc->width;
    ctx->height = enc->height;
    ctx->time_base = (AVRational){1, fps};
    ctx->framerate = (AVRational){fps, 1};
    ctx->pix_fmt = AV_PIX_FMT_YUV420P;
    ctx->bit_rate = bitrate;
    ctx->gop_size = gop;
    ctx->max_b_frames = 0;
    ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;

    av_opt_set(ctx->priv_data, "preset", "ultrafast", 0);
    av_opt_set(ctx->priv_data, "tune", "zerolatency", 0);

    if (avcodec_open2(ctx, codec, NULL) < 0) {
        fprintf(stderr, "[venc] cannot open H.264 encoder\n");
        avcodec_free_context(&ctx);
        return -1;
    }

    AVFrame *frame = av_frame_alloc();
    frame->format = AV_PIX_FMT_YUV420P;
    frame->width = enc->width;
    frame->height = enc->height;
    av_frame_get_buffer(frame, 0);

    enc->codec_ctx = ctx;
    enc->frame = frame;
    enc->pkt = av_packet_alloc();
    enc->frame_count = 0;

    fprintf(stderr, "[venc] H.264 encoder opened: %dx%d @%dfps bitrate=%d gop=%d\n",
            enc->width, enc->height, fps, bitrate, gop);
    return 0;
}

int p2p_video_encoder_encode(p2p_video_encoder_t *enc,
                             const uint8_t *raw_data, int raw_size,
                             int in_pixfmt,
                             uint8_t **out_data, int *out_size,
                             int *is_keyframe)
{
    if (!enc || !enc->codec_ctx) return -1;

    AVCodecContext *ctx = (AVCodecContext *)enc->codec_ctx;
    AVFrame *frame = (AVFrame *)enc->frame;
    AVPacket *pkt = (AVPacket *)enc->pkt;

    av_frame_make_writable(frame);

    /* Only MJPEG capture is used; always decode to YUV420P then H.264 encode. */
    if (in_pixfmt == P2P_V4L2_PIX_MJPEG) {
        /* Validate JPEG: must start with SOI marker (0xFFD8) and have reasonable size */
        if (raw_size < 100 || raw_data[0] != 0xFF || raw_data[1] != 0xD8) {
            return -1;
        }

        /* Decode MJPEG (JPEG) to raw pixels, then convert to YUV420P */
        if (!enc->mjpeg_ctx) {
            const AVCodec *mjdec = avcodec_find_decoder(AV_CODEC_ID_MJPEG);
            if (!mjdec) {
                fprintf(stderr, "[venc] MJPEG decoder not found\n");
                return -1;
            }
            AVCodecContext *mctx = avcodec_alloc_context3(mjdec);
            mctx->thread_count = 1;
            mctx->flags2 |= AV_CODEC_FLAG2_FAST;
            if (avcodec_open2(mctx, mjdec, NULL) < 0) {
                avcodec_free_context(&mctx);
                return -1;
            }
            enc->mjpeg_ctx = mctx;
            enc->mjpeg_pkt = av_packet_alloc();
            enc->mjpeg_frame = av_frame_alloc();
            enc->mjpeg_last_fmt = -1;
        }
        AVCodecContext *mctx = (AVCodecContext *)enc->mjpeg_ctx;
        AVPacket *mpkt = (AVPacket *)enc->mjpeg_pkt;
        AVFrame *mframe = (AVFrame *)enc->mjpeg_frame;

        mpkt->data = (uint8_t *)raw_data;
        mpkt->size = raw_size;

        int ret2 = avcodec_send_packet(mctx, mpkt);
        if (ret2 < 0) {
            avcodec_flush_buffers(mctx);
            return -1;
        }
        ret2 = avcodec_receive_frame(mctx, mframe);
        if (ret2 < 0) {
            avcodec_flush_buffers(mctx);
            return -1;
        }
        if (mframe->decode_error_flags) {
            avcodec_flush_buffers(mctx);
            return -1;
        }

        /* MJPEG decoder outputs YUVJ422P or YUVJ420P; convert to YUV420P */
        if (!enc->mjpeg_sws || mframe->format != enc->mjpeg_last_fmt) {
            if (enc->mjpeg_sws)
                sws_freeContext((struct SwsContext *)enc->mjpeg_sws);
            enc->mjpeg_sws = sws_getContext(
                mframe->width, mframe->height, mframe->format,
                enc->width, enc->height, AV_PIX_FMT_YUV420P,
                SWS_FAST_BILINEAR, NULL, NULL, NULL);
            enc->mjpeg_last_fmt = mframe->format;
        }
        sws_scale((struct SwsContext *)enc->mjpeg_sws,
                  (const uint8_t * const *)mframe->data, mframe->linesize,
                  0, mframe->height,
                  frame->data, frame->linesize);
    } else {
        /* Only MJPEG capture is supported; other formats are not used. */
        return -1;
    }

    frame->pts = enc->frame_count++;

    if (enc->force_idr) {
        frame->pict_type = AV_PICTURE_TYPE_I;
        enc->force_idr = 0;
    } else {
        frame->pict_type = AV_PICTURE_TYPE_NONE;
    }

    int ret = avcodec_send_frame(ctx, frame);
    if (ret < 0) return -1;

    ret = avcodec_receive_packet(ctx, pkt);
    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
        *out_size = 0;
        return 0;
    }
    if (ret < 0) return -1;

    *out_data = pkt->data;
    *out_size = pkt->size;
    *is_keyframe = (pkt->flags & AV_PKT_FLAG_KEY) ? 1 : 0;
    return pkt->size;
}

void p2p_video_encoder_close(p2p_video_encoder_t *enc)
{
    if (!enc) return;
    if (enc->pkt) av_packet_free((AVPacket **)&enc->pkt);
    if (enc->frame) av_frame_free((AVFrame **)&enc->frame);
    if (enc->codec_ctx) avcodec_free_context((AVCodecContext **)&enc->codec_ctx);
    if (enc->sws_ctx) sws_freeContext((struct SwsContext *)enc->sws_ctx);
    if (enc->mjpeg_pkt) av_packet_free((AVPacket **)&enc->mjpeg_pkt);
    if (enc->mjpeg_frame) av_frame_free((AVFrame **)&enc->mjpeg_frame);
    if (enc->mjpeg_ctx) avcodec_free_context((AVCodecContext **)&enc->mjpeg_ctx);
    if (enc->mjpeg_sws) sws_freeContext((struct SwsContext *)enc->mjpeg_sws);
    memset(enc, 0, sizeof(*enc));
}

/* ======== Video Decoder (H.264) ======== */

int p2p_video_decoder_open(p2p_video_decoder_t *dec)
{
    if (!dec) return -1;
    memset(dec, 0, sizeof(*dec));

    const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    if (!codec) {
        fprintf(stderr, "[vdec] H.264 decoder not found\n");
        return -1;
    }

    AVCodecContext *ctx = avcodec_alloc_context3(codec);
    if (!ctx) return -1;

    ctx->flags2 |= AV_CODEC_FLAG2_FAST;
    ctx->thread_count = 1;

    if (avcodec_open2(ctx, codec, NULL) < 0) {
        avcodec_free_context(&ctx);
        return -1;
    }

    dec->codec_ctx = ctx;
    dec->frame = av_frame_alloc();
    dec->pkt = av_packet_alloc();

    fprintf(stderr, "[vdec] H.264 decoder opened\n");
    return 0;
}

int p2p_video_decoder_decode(p2p_video_decoder_t *dec,
                             const uint8_t *nal_data, int nal_size,
                             uint8_t **y, uint8_t **u, uint8_t **v,
                             int *linesize_y, int *linesize_u, int *linesize_v,
                             int *width, int *height)
{
    if (!dec || !dec->codec_ctx) return -1;

    AVCodecContext *ctx = (AVCodecContext *)dec->codec_ctx;
    AVFrame *frame = (AVFrame *)dec->frame;
    AVPacket *pkt = (AVPacket *)dec->pkt;

    pkt->data = (uint8_t *)nal_data;
    pkt->size = nal_size;

    int ret = avcodec_send_packet(ctx, pkt);
    if (ret < 0) return -1;

    ret = avcodec_receive_frame(ctx, frame);
    if (ret == AVERROR(EAGAIN)) return 1;
    if (ret < 0) return -1;

    *y = frame->data[0];
    *u = frame->data[1];
    *v = frame->data[2];
    *linesize_y = frame->linesize[0];
    *linesize_u = frame->linesize[1];
    *linesize_v = frame->linesize[2];
    *width = frame->width;
    *height = frame->height;
    return 0;
}

void p2p_video_decoder_close(p2p_video_decoder_t *dec)
{
    if (!dec) return;
    if (dec->pkt) av_packet_free((AVPacket **)&dec->pkt);
    if (dec->frame) av_frame_free((AVFrame **)&dec->frame);
    if (dec->codec_ctx) avcodec_free_context((AVCodecContext **)&dec->codec_ctx);
    memset(dec, 0, sizeof(*dec));
}

/* ======== Audio Encoder (Opus) ======== */

int p2p_audio_encoder_open(p2p_audio_encoder_t *enc, const p2p_audio_encoder_config_t *cfg)
{
    if (!enc || !cfg) return -1;
    memset(enc, 0, sizeof(*enc));

    enc->sample_rate = cfg->sample_rate > 0 ? cfg->sample_rate : 48000;
    enc->channels = cfg->channels > 0 ? cfg->channels : 1;
    enc->frame_size = cfg->frame_size > 0 ? cfg->frame_size : 960;
    int bitrate = cfg->bitrate > 0 ? cfg->bitrate : 64000;

    const AVCodec *codec = avcodec_find_encoder(AV_CODEC_ID_OPUS);
    if (!codec) {
        fprintf(stderr, "[aenc] Opus encoder not found\n");
        return -1;
    }

    AVCodecContext *ctx = avcodec_alloc_context3(codec);
    if (!ctx) return -1;

    ctx->sample_rate = enc->sample_rate;
    ctx->bit_rate = bitrate;
    ctx->sample_fmt = AV_SAMPLE_FMT_S16;

    AVChannelLayout ch_layout;
    if (enc->channels == 1)
        ch_layout = (AVChannelLayout)AV_CHANNEL_LAYOUT_MONO;
    else
        ch_layout = (AVChannelLayout)AV_CHANNEL_LAYOUT_STEREO;
    av_channel_layout_copy(&ctx->ch_layout, &ch_layout);

    /* Try to open; if S16 not supported, try FLT */
    if (avcodec_open2(ctx, codec, NULL) < 0) {
        ctx->sample_fmt = AV_SAMPLE_FMT_FLT;
        if (avcodec_open2(ctx, codec, NULL) < 0) {
            fprintf(stderr, "[aenc] cannot open Opus encoder\n");
            avcodec_free_context(&ctx);
            return -1;
        }
    }

    AVFrame *frame = av_frame_alloc();
    frame->format = ctx->sample_fmt;
    av_channel_layout_copy(&frame->ch_layout, &ctx->ch_layout);
    frame->sample_rate = enc->sample_rate;
    frame->nb_samples = enc->frame_size;
    av_frame_get_buffer(frame, 0);

    enc->codec_ctx = ctx;
    enc->frame = frame;
    enc->pkt = av_packet_alloc();

    fprintf(stderr, "[aenc] Opus encoder opened: %dHz %dch frame_size=%d fmt=%s\n",
            enc->sample_rate, enc->channels, enc->frame_size,
            ctx->sample_fmt == AV_SAMPLE_FMT_S16 ? "S16" : "FLT");
    return 0;
}

int p2p_audio_encoder_encode(p2p_audio_encoder_t *enc,
                             const int16_t *samples, int num_samples,
                             uint8_t **out_data, int *out_size)
{
    if (!enc || !enc->codec_ctx) return -1;

    AVCodecContext *ctx = (AVCodecContext *)enc->codec_ctx;
    AVFrame *frame = (AVFrame *)enc->frame;
    AVPacket *pkt = (AVPacket *)enc->pkt;

    av_frame_make_writable(frame);
    frame->nb_samples = num_samples;

    if (ctx->sample_fmt == AV_SAMPLE_FMT_S16) {
        memcpy(frame->data[0], samples, num_samples * enc->channels * sizeof(int16_t));
    } else {
        /* Convert S16 -> FLT */
        float *dst = (float *)frame->data[0];
        for (int i = 0; i < num_samples * enc->channels; i++)
            dst[i] = samples[i] / 32768.0f;
    }

    int ret = avcodec_send_frame(ctx, frame);
    if (ret < 0) return -1;

    ret = avcodec_receive_packet(ctx, pkt);
    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
        *out_size = 0;
        return 0;
    }
    if (ret < 0) return -1;

    *out_data = pkt->data;
    *out_size = pkt->size;
    return pkt->size;
}

void p2p_audio_encoder_close(p2p_audio_encoder_t *enc)
{
    if (!enc) return;
    if (enc->pkt) av_packet_free((AVPacket **)&enc->pkt);
    if (enc->frame) av_frame_free((AVFrame **)&enc->frame);
    if (enc->codec_ctx) avcodec_free_context((AVCodecContext **)&enc->codec_ctx);
    memset(enc, 0, sizeof(*enc));
}

/* ======== Audio Decoder (Opus) ======== */

int p2p_audio_decoder_open(p2p_audio_decoder_t *dec, int sample_rate, int channels)
{
    if (!dec) return -1;
    memset(dec, 0, sizeof(*dec));

    const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_OPUS);
    if (!codec) {
        fprintf(stderr, "[adec] Opus decoder not found\n");
        return -1;
    }

    AVCodecContext *ctx = avcodec_alloc_context3(codec);
    if (!ctx) return -1;

    ctx->sample_rate = sample_rate > 0 ? sample_rate : 48000;
    AVChannelLayout ch_layout;
    if (channels <= 1)
        ch_layout = (AVChannelLayout)AV_CHANNEL_LAYOUT_MONO;
    else
        ch_layout = (AVChannelLayout)AV_CHANNEL_LAYOUT_STEREO;
    av_channel_layout_copy(&ctx->ch_layout, &ch_layout);

    ctx->request_sample_fmt = AV_SAMPLE_FMT_S16;

    if (avcodec_open2(ctx, codec, NULL) < 0) {
        avcodec_free_context(&ctx);
        return -1;
    }

    dec->codec_ctx = ctx;
    dec->frame = av_frame_alloc();
    dec->pkt = av_packet_alloc();

    fprintf(stderr, "[adec] Opus decoder opened: %dHz %dch\n",
            ctx->sample_rate, ctx->ch_layout.nb_channels);
    return 0;
}

int p2p_audio_decoder_decode(p2p_audio_decoder_t *dec,
                             const uint8_t *data, int size,
                             int16_t **out_samples, int *out_num_samples)
{
    if (!dec || !dec->codec_ctx) return -1;

    AVCodecContext *ctx = (AVCodecContext *)dec->codec_ctx;
    AVFrame *frame = (AVFrame *)dec->frame;
    AVPacket *pkt = (AVPacket *)dec->pkt;

    pkt->data = (uint8_t *)data;
    pkt->size = size;

    int ret = avcodec_send_packet(ctx, pkt);
    if (ret < 0) return -1;

    ret = avcodec_receive_frame(ctx, frame);
    if (ret == AVERROR(EAGAIN)) return 0;
    if (ret < 0) return -1;

    /* If decoder outputs FLT, convert to S16 in-place (reuse frame buffer) */
    if (ctx->sample_fmt == AV_SAMPLE_FMT_FLT || ctx->sample_fmt == AV_SAMPLE_FMT_FLTP) {
        /* Allocate a small static buffer for conversion */
        static int16_t s16_buf[48000]; /* enough for 1 second at 48kHz mono */
        int total = frame->nb_samples * ctx->ch_layout.nb_channels;
        if (total > 48000) total = 48000;

        if (ctx->sample_fmt == AV_SAMPLE_FMT_FLT) {
            const float *src = (const float *)frame->data[0];
            for (int i = 0; i < total; i++) {
                float v = src[i] * 32768.0f;
                if (v > 32767.0f) v = 32767.0f;
                if (v < -32768.0f) v = -32768.0f;
                s16_buf[i] = (int16_t)v;
            }
        } else {
            const float *src = (const float *)frame->data[0];
            for (int i = 0; i < frame->nb_samples; i++) {
                float v = src[i] * 32768.0f;
                if (v > 32767.0f) v = 32767.0f;
                if (v < -32768.0f) v = -32768.0f;
                s16_buf[i] = (int16_t)v;
            }
        }
        *out_samples = s16_buf;
    } else {
        *out_samples = (int16_t *)frame->data[0];
    }
    *out_num_samples = frame->nb_samples;
    return frame->nb_samples;
}

void p2p_audio_decoder_close(p2p_audio_decoder_t *dec)
{
    if (!dec) return;
    if (dec->pkt) av_packet_free((AVPacket **)&dec->pkt);
    if (dec->frame) av_frame_free((AVFrame **)&dec->frame);
    if (dec->codec_ctx) avcodec_free_context((AVCodecContext **)&dec->codec_ctx);
    memset(dec, 0, sizeof(*dec));
}
