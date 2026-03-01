#include "p2p_sdl_render.h"

#include <stdio.h>
#include <string.h>
#include <SDL2/SDL.h>

/* ======== SDL Init/Quit ======== */

int p2p_sdl_init(void)
{
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) < 0) {
        fprintf(stderr, "[sdl] SDL_Init failed: %s\n", SDL_GetError());
        return -1;
    }
    return 0;
}

void p2p_sdl_quit(void)
{
    SDL_Quit();
}

int p2p_sdl_poll_events(void)
{
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        if (ev.type == SDL_QUIT)
            return -1;
        if (ev.type == SDL_KEYDOWN && ev.key.keysym.sym == SDLK_ESCAPE)
            return -1;
    }
    return 0;
}

/* ======== SDL Video ======== */

int p2p_sdl_video_init(p2p_sdl_video_t *v, const char *title, int width, int height)
{
    if (!v) return -1;
    memset(v, 0, sizeof(*v));

    SDL_Window *win = SDL_CreateWindow(
        title ? title : "P2P Video",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        width, height,
        SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
    if (!win) {
        fprintf(stderr, "[sdl] CreateWindow failed: %s\n", SDL_GetError());
        return -1;
    }

    SDL_Renderer *ren = SDL_CreateRenderer(win, -1,
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!ren) {
        ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE);
    }
    if (!ren) {
        fprintf(stderr, "[sdl] CreateRenderer failed: %s\n", SDL_GetError());
        SDL_DestroyWindow(win);
        return -1;
    }

    SDL_Texture *tex = SDL_CreateTexture(ren,
        SDL_PIXELFORMAT_IYUV, SDL_TEXTUREACCESS_STREAMING,
        width, height);
    if (!tex) {
        fprintf(stderr, "[sdl] CreateTexture failed: %s\n", SDL_GetError());
        SDL_DestroyRenderer(ren);
        SDL_DestroyWindow(win);
        return -1;
    }

    v->window = win;
    v->renderer = ren;
    v->texture = tex;
    v->tex_width = width;
    v->tex_height = height;
    v->initialized = 1;

    fprintf(stderr, "[sdl] video display initialized: %dx%d\n", width, height);
    return 0;
}

int p2p_sdl_video_display(p2p_sdl_video_t *v,
                          const uint8_t *y, const uint8_t *u, const uint8_t *v_plane,
                          int linesize_y, int linesize_u, int linesize_v,
                          int width, int height)
{
    if (!v || !v->initialized) return -1;

    /* Recreate texture if resolution changed */
    if (width != v->tex_width || height != v->tex_height) {
        if (v->texture) SDL_DestroyTexture((SDL_Texture *)v->texture);
        v->texture = SDL_CreateTexture((SDL_Renderer *)v->renderer,
            SDL_PIXELFORMAT_IYUV, SDL_TEXTUREACCESS_STREAMING,
            width, height);
        if (!v->texture) return -1;
        v->tex_width = width;
        v->tex_height = height;
    }

    SDL_UpdateYUVTexture((SDL_Texture *)v->texture, NULL,
                         y, linesize_y,
                         u, linesize_u,
                         v_plane, linesize_v);

    SDL_RenderClear((SDL_Renderer *)v->renderer);
    SDL_RenderCopy((SDL_Renderer *)v->renderer, (SDL_Texture *)v->texture, NULL, NULL);
    SDL_RenderPresent((SDL_Renderer *)v->renderer);
    return 0;
}

void p2p_sdl_video_destroy(p2p_sdl_video_t *v)
{
    if (!v) return;
    if (v->texture) SDL_DestroyTexture((SDL_Texture *)v->texture);
    if (v->renderer) SDL_DestroyRenderer((SDL_Renderer *)v->renderer);
    if (v->window) SDL_DestroyWindow((SDL_Window *)v->window);
    memset(v, 0, sizeof(*v));
}

/* ======== SDL Audio ======== */

int p2p_sdl_audio_init(p2p_sdl_audio_t *a, int sample_rate, int channels)
{
    if (!a) return -1;
    memset(a, 0, sizeof(*a));

    SDL_AudioSpec want, have;
    memset(&want, 0, sizeof(want));
    want.freq = sample_rate > 0 ? sample_rate : 48000;
    want.format = AUDIO_S16SYS;
    want.channels = channels > 0 ? channels : 1;
    want.samples = 960; /* ~20ms buffer at 48kHz */

    SDL_AudioDeviceID dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (dev == 0) {
        fprintf(stderr, "[sdl] OpenAudioDevice failed: %s\n", SDL_GetError());
        return -1;
    }

    a->device_id = dev;
    a->sample_rate = have.freq;
    a->channels = have.channels;
    a->initialized = 1;

    SDL_PauseAudioDevice(dev, 0);

    fprintf(stderr, "[sdl] audio playback initialized: %dHz %dch\n",
            a->sample_rate, a->channels);
    return 0;
}

int p2p_sdl_audio_play(p2p_sdl_audio_t *a, const int16_t *samples, int num_samples)
{
    if (!a || !a->initialized) return -1;
    int bytes = num_samples * a->channels * sizeof(int16_t);
    return SDL_QueueAudio(a->device_id, samples, bytes);
}

void p2p_sdl_audio_destroy(p2p_sdl_audio_t *a)
{
    if (!a) return;
    if (a->device_id) {
        SDL_CloseAudioDevice(a->device_id);
    }
    memset(a, 0, sizeof(*a));
}
