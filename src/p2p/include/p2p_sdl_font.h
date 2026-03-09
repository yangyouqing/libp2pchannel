#ifndef P2P_SDL_FONT_H
#define P2P_SDL_FONT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 8x8 bitmap font rendering (no SDL_ttf). renderer is SDL_Renderer*. */
void p2p_sdl_draw_text(void *renderer, int x, int y, const char *text,
                      int scale, uint8_t cr, uint8_t cg, uint8_t cb);

#ifdef __cplusplus
}
#endif

#endif
