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
    int   video_area_h; /* if >0, video renders to top video_area_h pixels only */
    int   initialized;
} p2p_sdl_video_t;

int  p2p_sdl_video_init(p2p_sdl_video_t *v, const char *title, int width, int height);
/* Update and render a YUV420P frame. Recreates texture if resolution changes. */
int  p2p_sdl_video_display(p2p_sdl_video_t *v,
                           const uint8_t *y, const uint8_t *u, const uint8_t *v_plane,
                           int linesize_y, int linesize_u, int linesize_v,
                           int width, int height);
/* Update texture (if y non-NULL), clear and copy to renderer; no present (for overlay). */
int  p2p_sdl_video_draw_frame(p2p_sdl_video_t *v,
                              const uint8_t *y, const uint8_t *u, const uint8_t *v_plane,
                              int linesize_y, int linesize_u, int linesize_v,
                              int width, int height);
void p2p_sdl_video_present(p2p_sdl_video_t *v);
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

/* Events filled by p2p_sdl_poll_events when non-NULL. key_sym is SDL_Keycode. */
typedef struct {
    int       quit;         /* 1 if quit requested (QUIT or Escape) */
    int       key_down;     /* 1 if a key was pressed this poll */
    uint32_t  key_sym;      /* SDL_Keycode (e.g. SDLK_RETURN, SDLK_a) */
    int       mouse_down;   /* 1 if mouse button was pressed this poll */
    int       mouse_x;
    int       mouse_y;
    uint8_t   mouse_button; /* 1=left, 2=middle, 3=right */
} p2p_sdl_events_t;

/* Process SDL events. If out_events is non-NULL, fill with last key/mouse. Returns 0 normally, -1 if quit requested. */
int  p2p_sdl_poll_events(p2p_sdl_events_t *out_events);

/* Draw a filled rectangle (e.g. for overlay backgrounds). renderer is SDL_Renderer*. */
void p2p_sdl_draw_rect(void *renderer, int x, int y, int w, int h,
                       uint8_t r, uint8_t g, uint8_t b, uint8_t a);

/* Initialize SDL subsystems (video + audio). Call once from main thread. */
int  p2p_sdl_init(void);
void p2p_sdl_quit(void);

#ifdef __cplusplus
}
#endif

#endif
