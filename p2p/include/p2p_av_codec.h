#ifndef P2P_AV_CODEC_H
#define P2P_AV_CODEC_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Video Encoder (H.264 via libx264) ---- */

typedef struct {
    int width;
    int height;
    int fps;
    int bitrate;        /* bps, default 1000000 */
    int gop_size;       /* keyframe interval, default = fps (1 second) */
} p2p_video_encoder_config_t;

typedef struct {
    void *codec_ctx;    /* AVCodecContext* (H.264 encoder) */
    void *frame;        /* AVFrame*        */
    void *pkt;          /* AVPacket*       */
    void *sws_ctx;      /* SwsContext*     */
    void *mjpeg_ctx;    /* AVCodecContext* (MJPEG decoder for V4L2 MJPEG input) */
    void *mjpeg_pkt;    /* AVPacket*       */
    void *mjpeg_frame;  /* AVFrame*        */
    void *mjpeg_sws;    /* SwsContext* (MJPEG decoded pix -> YUV420P) */
    int   width;
    int   height;
    int   frame_count;
} p2p_video_encoder_t;

int  p2p_video_encoder_open(p2p_video_encoder_t *enc, const p2p_video_encoder_config_t *cfg);
/* Encode a raw frame. Input is YUYV or YUV420P depending on in_pixfmt.
   Returns encoded NAL data length, or 0 if no output yet, or -1 on error.
   *out_data points to internal buffer valid until next encode call.
   *is_keyframe set to 1 if output is IDR. */
int  p2p_video_encoder_encode(p2p_video_encoder_t *enc,
                              const uint8_t *raw_data, int raw_size,
                              int in_pixfmt, /* P2P_V4L2_PIX_YUYV or -1 for YUV420P */
                              uint8_t **out_data, int *out_size,
                              int *is_keyframe);
void p2p_video_encoder_close(p2p_video_encoder_t *enc);

/* ---- Video Decoder (H.264) ---- */

typedef struct {
    void *codec_ctx;    /* AVCodecContext* */
    void *frame;        /* AVFrame*        */
    void *pkt;          /* AVPacket*       */
} p2p_video_decoder_t;

int  p2p_video_decoder_open(p2p_video_decoder_t *dec);
/* Decode H.264 NAL data. Returns decoded YUV420P frame info.
   Returns 0 if frame decoded, 1 if need more data, -1 on error.
   y/u/v point to internal buffers valid until next decode call. */
int  p2p_video_decoder_decode(p2p_video_decoder_t *dec,
                              const uint8_t *nal_data, int nal_size,
                              uint8_t **y, uint8_t **u, uint8_t **v,
                              int *linesize_y, int *linesize_u, int *linesize_v,
                              int *width, int *height);
void p2p_video_decoder_close(p2p_video_decoder_t *dec);

/* ---- Audio Encoder (Opus via libopus/FFmpeg) ---- */

typedef struct {
    int sample_rate;    /* default 48000 */
    int channels;       /* default 1 */
    int bitrate;        /* default 64000 */
    int frame_size;     /* samples per channel per frame, default 960 (20ms) */
} p2p_audio_encoder_config_t;

typedef struct {
    void *codec_ctx;    /* AVCodecContext* */
    void *frame;        /* AVFrame*        */
    void *pkt;          /* AVPacket*       */
    int   frame_size;
    int   channels;
    int   sample_rate;
} p2p_audio_encoder_t;

int  p2p_audio_encoder_open(p2p_audio_encoder_t *enc, const p2p_audio_encoder_config_t *cfg);
/* Encode PCM S16LE samples. num_samples must equal frame_size.
   Returns encoded data length, 0 if buffering, -1 on error. */
int  p2p_audio_encoder_encode(p2p_audio_encoder_t *enc,
                              const int16_t *samples, int num_samples,
                              uint8_t **out_data, int *out_size);
void p2p_audio_encoder_close(p2p_audio_encoder_t *enc);

/* ---- Audio Decoder (Opus via FFmpeg) ---- */

typedef struct {
    void *codec_ctx;    /* AVCodecContext* */
    void *frame;        /* AVFrame*        */
    void *pkt;          /* AVPacket*       */
} p2p_audio_decoder_t;

int  p2p_audio_decoder_open(p2p_audio_decoder_t *dec, int sample_rate, int channels);
/* Decode Opus packet. Returns decoded PCM samples.
   *out_samples points to internal buffer (int16_t) valid until next call.
   Returns number of decoded samples per channel, or -1 on error. */
int  p2p_audio_decoder_decode(p2p_audio_decoder_t *dec,
                              const uint8_t *data, int size,
                              int16_t **out_samples, int *out_num_samples);
void p2p_audio_decoder_close(p2p_audio_decoder_t *dec);

#ifdef __cplusplus
}
#endif

#endif
