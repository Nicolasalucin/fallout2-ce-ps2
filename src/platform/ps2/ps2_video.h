/*
 * Video backend contract for the Fallout PS2 port.
 *
 * The engine works on a single 8-bit indexed 640x480 buffer. This module
 * presents it using the GS as a 32-bit color surface (Option A, matches FO1):
 *
 *   - the frame buffer is converted to CT32 on the EE via CPU palette LUT
 *     (1.2 MB RGBA frame, zero GS CLUT, zero swizzle issues);
 *   - the CT32 texture is uploaded via DMA every frame;
 *   - a full-screen textured quad is drawn each frame and flipped on vsync
 *     (NTSC 640x448, 60 Hz interlaced).
 *
 * Called from the SDL shim (sdl_shim.c), which forwards:
 *   - SDL_CreateRenderer   -> ps2_video_init
 *   - SDL_DestroyRenderer  -> ps2_video_shutdown
 *   - SDL_RenderPresent    -> ps2_video_present
 *   - palette updates      -> ps2_video_set_palette
 */

#ifndef PS2_VIDEO_H_
#define PS2_VIDEO_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void ps2_video_init(void);
void ps2_video_shutdown(void);

/* Present an 8-bit indexed frame (screen buffer owned by the engine). */
void ps2_video_present(const void* pixels, int width, int height, int pitch);

/* Update the CPU palette LUT (GS CT32 layout: alpha<<24 | blue<<16 | green<<8 | red).
 * The LUT is used by the EE to convert 8bpp indices to CT32 each frame. */
void ps2_video_set_palette(const void* rgba_colors);

/* Boot-time diagnostic: shows a test pattern (color bars, border, moving
 * column, rotating palette) for a few seconds, prints GS timing probes and
 * logs the pad transitions. Compiled in always, run only when the build
 * defines PS2_DEV_BOOT_SELFTEST (see main.cc). */
void ps2_video_selftest(void);

/* Boot diagnostics: prints accumulated per-phase present() timings. */
void ps2_video_debug_print_phases(void);

/* Palette timeline tracing + frame captures (development diagnostics).
 * Off by default; the smoke suite turns it on. While enabled, palette
 * changes are printed with a checksum (throttled) and frame captures
 * requested with ps2_video_capture() are honored. */
void ps2_video_diagnostics(int enabled);

/* Record a palette update coming from the SDL shim. `crc` is a checksum of
 * the resulting 256-entry mirror, used to correlate the timeline. */
void ps2_video_palette_note(const char* what, int first, int count, unsigned int crc);

/* Dump the next `count` presented frames, `stride` presents apart, as
 * <name>_<i>.idx (8-bit indices) + <name>_<i>.pal (256 CT32 entries) +
 * <name>_<i>.txt (metadata) under the first writable capture root
 * (host:/caps, mass:/FALLOUT2/caps, mc0:/FALLOUT2/caps). Host side:
 * tools/ps2_shot.py turns the pair into a PNG. Returns 0 when the request
 * was accepted. */
int ps2_video_capture(const char* name, int count, int stride);

#ifdef __cplusplus
}
#endif

#endif /* PS2_VIDEO_H_ */