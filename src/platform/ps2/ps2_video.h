#ifndef PS2_VIDEO_H_
#define PS2_VIDEO_H_

/*
 * Video backend contract for the Fallout PS2 port.
 *
 * The engine works on a single 8-bit indexed 640x480 buffer. This module
 * presents it using the GS as a 256-color indexed surface:
 *
 *   - the frame buffer is uploaded to VRAM as a GS_PSM_T8 texture
 *     (streamed via DMA every frame, zero CPU color conversion);
 *   - the palette lives in a GS_PSM_CT32 CLUT that is uploaded together
 *     with the texture (1 KB, negligible);
 *   - a full-screen textured quad is drawn each frame and flipped on vsync
 *     (NTSC 640x448, 60 Hz interlaced).
 *
 * Called from the SDL shim (sdl_shim.c), which forwards:
 *   - SDL_CreateRenderer   -> ps2_video_init
 *   - SDL_DestroyRenderer  -> ps2_video_shutdown
 *   - SDL_RenderPresent    -> ps2_video_present
 *   - palette updates      -> ps2_video_set_palette
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void ps2_video_init(void);
void ps2_video_shutdown(void);

/* Present an 8-bit indexed frame (screen buffer owned by the engine). */
void ps2_video_present(const void* pixels, int width, int height, int pitch);

/* Update the hardware CLUT. `rgba_colors` points to 256 uint32 entries in
 * GS CT32 layout: alpha<<24 | blue<<16 | green<<8 | red. */
void ps2_video_set_palette(const void* rgba_colors);

/* Boot-time diagnostic: shows a test pattern (color bars, border, moving
 * column, rotating palette) for a few seconds, prints GS timing probes and
 * logs the pad transitions. Compiled in always, run only when the build
 * defines PS2_DEV_BOOT_SELFTEST (see main.cc). */
void ps2_video_selftest(void);

/* Boot diagnostics: prints accumulated per-phase present() timings. */
void ps2_video_debug_print_phases(void);

#ifdef __cplusplus
}
#endif

#endif /* PS2_VIDEO_H_ */
