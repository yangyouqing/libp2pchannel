#ifndef P2P_SDL_RENDER_H
#define P2P_SDL_RENDER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- SDL2 Video Renderer ---- */

typedef struct {
    void *window;       /* SDL_Window*   */
    void *renderer;     /* SDL_Renderer* */
    void *texture;      /* SDL_Texture*  */
    int   tex_width;
    int   tex_height;
    int   initialized;
} p2p_sdl_video_t;

int  p2p_sdl_video_init(p2p_sdl_video_t *v, const char *title, int width, int height);
/* Update and render a YUV420P frame. Recreates texture if resolution changes. */
int  p2p_sdl_video_display(p2p_sdl_video_t *v,
                           const uint8_t *y, const uint8_t *u, const uint8_t *v_plane,
                           int linesize_y, int linesize_u, int linesize_v,
                           int width, int height);
void p2p_sdl_video_destroy(p2p_sdl_video_t *v);

/* ---- SDL2 Audio Playback ---- */

typedef struct {
    uint32_t device_id;  /* SDL_AudioDeviceID */
    int      sample_rate;
    int      channels;
    int      initialized;
} p2p_sdl_audio_t;

int  p2p_sdl_audio_init(p2p_sdl_audio_t *a, int sample_rate, int channels);
/* Queue PCM S16LE samples for playback */
int  p2p_sdl_audio_play(p2p_sdl_audio_t *a, const int16_t *samples, int num_samples);
void p2p_sdl_audio_destroy(p2p_sdl_audio_t *a);

/* ---- SDL2 Event Processing ---- */

/* Process SDL events. Returns 0 normally, -1 if quit requested. */
int  p2p_sdl_poll_events(void);

/* Initialize SDL subsystems (video + audio). Call once from main thread. */
int  p2p_sdl_init(void);
void p2p_sdl_quit(void);

#ifdef __cplusplus
}
#endif

#endif
