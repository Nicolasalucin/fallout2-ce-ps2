#ifndef PS2_SDL_INTERNAL_H_
#define PS2_SDL_INTERNAL_H_

/*
 * Internal interface between the SDL shim and the PS2 platform modules.
 *
 * The input layer (input_pad.c) uses these helpers to inject synthetic
 * events into the shim's event queue, and the audio layer hooks the
 * SDL_AudioStream API implemented in the shim.
 */

#include "SDL.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Push a fully built event into the queue (timestamp filled if zero). */
void ps2_sdl_push_event(const SDL_Event* event);

/* Convenience builders. `scancode` is an SDL_Scancode value. */
void ps2_sdl_post_key(int scancode, int down);
void ps2_sdl_post_mouse_move(int dx, int dy);
void ps2_sdl_post_mouse_button(int sdl_button, int down);
void ps2_sdl_post_mouse_wheel(int dx, int dy);
void ps2_sdl_post_window_event(int window_event);

#ifdef __cplusplus
}
#endif

#endif /* PS2_SDL_INTERNAL_H_ */
