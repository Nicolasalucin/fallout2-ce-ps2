/*
 * SDL2 subset implementation for the Fallout PS2 port.
 *
 * The engine talks to SDL2 in a well-defined, small subset
 * (see ../sdl/SDL.h). This file implements that subset on top of native PS2
 * services, so every upstream engine file stays unmodified:
 *
 *   - Surfaces are real 8-bit indexed buffers, exactly like on desktop.
 *   - CREATE-RGBSURFACE-WITHFORMAT STORAGE TRICK: the engine creates a
 *     "RGB888 streaming texture surface" only to blit the 8-bit screen into
 *     it before upload. Storing that surface as 8bpp indexed data too makes
 *     the blit a plain memory copy and lets the GS hardware do the palette
 *     lookup through its CLUT (no CPU color conversion anywhere).
 *   - Presentation (SDL_RenderPresent) forwards the texture surface
 *     directly to the video backend (gsKit, GS_PSM_T8 + GS_PSM_CT32).
 *   - The event queue, mouse state and timers are driven by the input layer
 *     (input_pad.c) and pumped from SDL_PollEvent/SDL_PumpEvents.
 *   - Audio: the device callback runs on a dedicated EE thread
 *     (ps2_audio.h / audio_audsrv.c) that streams mixed PCM to the SPU2 via
 *     audsrv. SDL_AudioStream performs the sample/rate/channel conversion
 *     SDL would do, so the engine's mixing code stays untouched.
 *
 * There is no SDL library linked: these symbols ARE the implementation.
 */

#include "SDL.h"
#include "ps2_sdl_internal.h"

#include "../ps2_osk.h"

#include <ctype.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <delaythread.h>

#include "ps2_audio.h"
#include "ps2_input.h"
#include "ps2_time.h"
#include "ps2_video.h"

/* ---------------------------------------------------------------------- */
/* Subsystem init state                                                    */
/* ---------------------------------------------------------------------- */

static Uint32 s_init_flags;

int SDL_InitSubSystem(Uint32 flags)
{
    s_init_flags |= flags;
    return 0;
}

Uint32 SDL_WasInit(Uint32 flags)
{
    return s_init_flags & flags;
}

void SDL_QuitSubSystem(Uint32 flags)
{
    s_init_flags &= ~flags;
}

void SDL_Quit(void)
{
    s_init_flags = 0;
}

SDL_bool SDL_SetHint(const char* name, const char* value)
{
    (void)name;
    (void)value;
    return SDL_TRUE;
}

Uint32 SDL_GetTicks(void)
{
    return ps2_time_ms();
}

void SDL_Delay(Uint32 ms)
{
    uint64_t target = ps2_time_us() + (uint64_t)(ms != 0 ? ms : 1) * 1000;

    /* PS2 (EE): sleep the thread, never busy-spin. The kernel schedules
     * strictly by priority and does not preempt a spinning thread, so a
     * spin here starves every peer thread - in particular the audio worker,
     * which is the only thing that advances the engine's sound cursors.
     * The MVE player waits on those cursors, so a spin here deadlocks the
     * whole console as soon as the first movie frame with audio is shown.
     * Yielding every millisecond keeps frame pacing accurate and the audio
     * thread alive. */
    while (ps2_time_us() < target) {
        /* Long engine sleeps (fades, the MVE player's millisecond waits)
         * must keep feeding audsrv: the ring is only topped up from this
         * thread. The pump clamps its own rate, so a 1 ms tick costs
         * nothing. */
        ps2_audio_pump();
        DelayThread(1000);
    }
}

/* ---------------------------------------------------------------------- */
/* Opaque object definitions                                               */
/* ---------------------------------------------------------------------- */

struct SDL_Window {
    char title[256];
};

struct SDL_Renderer {
    int logical_w;
    int logical_h;
};

struct SDL_Texture {
    Uint32 format;
    int w;
    int h;
    const void* pixels;
    int pitch;
};

struct SDL_Cursor {
    int dummy;
};

struct SDL_AudioStream {
    Uint16 src_format;
    Uint8 src_channels;
    int src_rate;
    Uint16 dst_format;
    Uint8 dst_channels;
    int dst_rate;

    /* Input ring holding raw source bytes; conversion happens on demand in
     * SDL_AudioStreamGet, mirroring SDL's lazy conversion. */
    unsigned char* input;
    int input_capacity;
    int input_head; /* byte offset of the first valid byte */
    int input_len; /* number of valid bytes */

    /* 16.16 fixed point index (in source frames) of the next output frame. */
    int src_pos;
    int rate_step; /* (src_rate << 16) / dst_rate */

    int src_frame_bytes;
    int dst_frame_bytes;
    int passthrough;
};

/* ---------------------------------------------------------------------- */
/* Surfaces                                                                */
/* ---------------------------------------------------------------------- */

/*
 * Palettes are reference counted because the engine shares one palette
 * object between the screen surface and the movie surfaces
 * (SDL_SetSurfacePalette in movie playback). Freeing a surface must only
 * drop its reference, never release a palette still owned by another
 * surface: doing so used to free the screen palette and every later
 * SDL_SetPaletteColors wrote into recycled heap memory.
 */
static void palette_release(SDL_Palette* palette)
{
    if (palette == NULL) {
        return;
    }

    if (palette->refcount > 0) {
        palette->refcount--;
    }

    if (palette->refcount == 0) {
        free(palette->colors);
        free(palette);
    }
}

static void surface_destroy(SDL_Surface* surface)
{
    if (surface == NULL) {
        return;
    }

    if (surface->format != NULL) {
        palette_release(surface->format->palette);
        free(surface->format);
    }

    free(surface->pixels);
    free(surface);
}

/* Pixel buffers end up as GS DMA sources (T8 texture upload): keep them
 * 64-byte (cache line) aligned and zeroed like calloc. memalign memory is
 * free()-compatible on ps2sdk (gsKit relies on this too). */
static void* pixels_alloc(size_t size)
{
    void* pixels = memalign(64, size);
    if (pixels != NULL) {
        memset(pixels, 0, size);
    }
    return pixels;
}

static SDL_Surface* surface_create(int width, int height, int depth)
{
    if (width <= 0 || height <= 0) {
        return NULL;
    }

    SDL_Surface* surface = (SDL_Surface*)calloc(1, sizeof(SDL_Surface));
    SDL_PixelFormat* format = (SDL_PixelFormat*)calloc(1, sizeof(SDL_PixelFormat));
    if (surface == NULL || format == NULL) {
        free(surface);
        free(format);
        return NULL;
    }

    surface->format = format;
    surface->w = width;
    surface->h = height;
    surface->refcount = 1;

    if (depth == 8) {
        surface->pitch = width;
        surface->pixels = pixels_alloc((size_t)width * height);

        format->BitsPerPixel = 8;
        format->BytesPerPixel = 1;

        SDL_Palette* palette = (SDL_Palette*)calloc(1, sizeof(SDL_Palette));
        if (palette != NULL) {
            palette->ncolors = 256;
            palette->colors = (SDL_Color*)calloc(256, sizeof(SDL_Color));
            palette->refcount = 1;
        }
        format->palette = palette;
    } else {
        int bytes_per_pixel = depth / 8;
        if (bytes_per_pixel < 1) {
            bytes_per_pixel = 1;
        }
        surface->pitch = width * bytes_per_pixel;
        surface->pixels = pixels_alloc((size_t)surface->pitch * height);
        format->BitsPerPixel = (Uint8)depth;
        format->BytesPerPixel = (Uint8)bytes_per_pixel;
    }

    if (surface->pixels == NULL) {
        surface_destroy(surface);
        return NULL;
    }

    return surface;
}

SDL_Surface* SDL_CreateRGBSurface(Uint32 flags, int width, int height, int depth, Uint32 Rmask, Uint32 Gmask, Uint32 Bmask, Uint32 Amask)
{
    (void)flags;
    (void)Rmask;
    (void)Gmask;
    (void)Bmask;
    (void)Amask;

    return surface_create(width, height, depth);
}

/*
 * The engine only calls this to create its "texture surface" from the
 * SDL_PIXELFORMAT_RGB888 format reported by SDL_QueryTexture. That surface
 * is stored as 8bpp indexed data on this port: the engine blits the 8-bit
 * screen into it and the GS CLUT performs the palette lookup in hardware.
 */
SDL_Surface* SDL_CreateRGBSurfaceWithFormat(Uint32 flags, int width, int height, int depth, Uint32 format)
{
    (void)flags;
    (void)depth;
    (void)format;

    return surface_create(width, height, 8);
}

void SDL_FreeSurface(SDL_Surface* surface)
{
    surface_destroy(surface);
}

int SDL_LockSurface(SDL_Surface* surface)
{
    (void)surface;
    return 0;
}

void SDL_UnlockSurface(SDL_Surface* surface)
{
    (void)surface;
}

/*
 * Rectangle blit with clipping. The only formats the engine produces are
 * 8bpp paletted surfaces (plus 8->32 expansion kept as a safety net for the
 * MVE movie code paths).
 */
static int blit_rect(SDL_Surface* src, const SDL_Rect* srcrect, SDL_Surface* dst, const SDL_Rect* dstrect)
{
    int sx = 0;
    int sy = 0;
    int sw = src->w;
    int sh = src->h;
    int dx = 0;
    int dy = 0;

    if (srcrect != NULL) {
        sx = srcrect->x;
        sy = srcrect->y;
        sw = srcrect->w;
        sh = srcrect->h;
    }

    if (dstrect != NULL) {
        dx = dstrect->x;
        dy = dstrect->y;
    }

    /* Clip source against the source surface. */
    if (sx < 0) { dx -= sx; sw += sx; sx = 0; }
    if (sy < 0) { dy -= sy; sh += sy; sy = 0; }
    if (sx + sw > src->w) sw = src->w - sx;
    if (sy + sh > src->h) sh = src->h - sy;

    /* Clip against the destination surface. */
    if (dx < 0) { sx -= dx; sw += dx; dx = 0; }
    if (dy < 0) { sy -= dy; sh += dy; dy = 0; }
    if (dx + sw > dst->w) sw = dst->w - dx;
    if (dy + sh > dst->h) sh = dst->h - dy;

    if (sw <= 0 || sh <= 0) {
        return 0;
    }

    int src_bpp = src->format->BitsPerPixel;
    int dst_bpp = dst->format->BitsPerPixel;

    if (src_bpp == 8 && dst_bpp == 8) {
        for (int y = 0; y < sh; y++) {
            memcpy((Uint8*)dst->pixels + (size_t)(dy + y) * dst->pitch + dx,
                (const Uint8*)src->pixels + (size_t)(sy + y) * src->pitch + sx,
                (size_t)sw);
        }
        return 0;
    }

    if (src_bpp == 8 && dst_bpp == 32) {
        const SDL_Palette* palette = src->format->palette;
        if (palette == NULL) {
            return -1;
        }
        for (int y = 0; y < sh; y++) {
            const Uint8* src_row = (const Uint8*)src->pixels + (size_t)(sy + y) * src->pitch + sx;
            Uint32* dst_row = (Uint32*)((Uint8*)dst->pixels + (size_t)(dy + y) * dst->pitch) + dx;
            for (int x = 0; x < sw; x++) {
                const SDL_Color* c = &palette->colors[src_row[x]];
                dst_row[x] = ((Uint32)c->a << 24) | ((Uint32)c->r << 16) | ((Uint32)c->g << 8) | c->b;
            }
        }
        return 0;
    }

    if (src_bpp == 32 && dst_bpp == 32) {
        for (int y = 0; y < sh; y++) {
            memcpy((Uint8*)dst->pixels + (size_t)(dy + y) * dst->pitch + (size_t)dx * 4,
                (const Uint8*)src->pixels + (size_t)(sy + y) * src->pitch + (size_t)sx * 4,
                (size_t)sw * 4);
        }
        return 0;
    }

    printf("[sdl] unsupported blit %d bpp -> %d bpp\n", src_bpp, dst_bpp);
    return -1;
}

int SDL_BlitSurface(SDL_Surface* src, const SDL_Rect* srcrect, SDL_Surface* dst, SDL_Rect* dstrect)
{
    if (src == NULL || dst == NULL) {
        return -1;
    }

    return blit_rect(src, srcrect, dst, dstrect);
}

/*
 * The engine maintains a single logical game palette (the screen palette);
 * every SDL_SetPaletteColors call animates that palette (full sets and fade
 * steps alike), and movie frames share it via SDL_SetSurfacePalette. We
 * mirror every update into a global palette and re-upload the CLUT once per
 * frame while it is dirty.
 */
static SDL_Color s_game_palette[256];
static int s_palette_dirty = 1;

int SDL_SetPaletteColors(SDL_Palette* palette, const SDL_Color* colors, int firstcolor, int ncolors)
{
    if (palette == NULL || colors == NULL || firstcolor < 0 || ncolors < 0 || firstcolor + ncolors > palette->ncolors) {
        return -1;
    }

    memcpy(&palette->colors[firstcolor], colors, (size_t)ncolors * sizeof(SDL_Color));

    if (firstcolor + ncolors <= 256) {
        memcpy(&s_game_palette[firstcolor], colors, (size_t)ncolors * sizeof(SDL_Color));
        s_palette_dirty = 1;

        {
            /* Palette timeline for the PS2 CLUT diagnostics: a checksum of
             * the resulting mirror plus the updated range lets the host
             * correlate "who changed the palette" with the frame captures. */
            const unsigned char* bytes = (const unsigned char*)s_game_palette;
            unsigned int crc = 2166136261u;
            for (int i = 0; i < 768; i++) {
                crc ^= bytes[i];
                crc *= 16777619u;
            }
            ps2_video_palette_note("set", firstcolor, ncolors, crc);
        }
    }

    return 0;
}

int SDL_SetSurfacePalette(SDL_Surface* surface, SDL_Palette* palette)
{
    if (surface == NULL || surface->format == NULL) {
        return -1;
    }

    if (surface->format->palette == palette) {
        return 0;
    }

    palette_release(surface->format->palette);

    surface->format->palette = palette;
    if (palette != NULL) {
        palette->refcount++;
    }

    return 0;
}

/* ---------------------------------------------------------------------- */
/* Window / renderer / texture                                             */
/* ---------------------------------------------------------------------- */

static SDL_Texture* s_last_texture;

SDL_Window* SDL_CreateWindow(const char* title, int x, int y, int w, int h, Uint32 flags)
{
    (void)x;
    (void)y;
    (void)w;
    (void)h;
    (void)flags;

    SDL_Window* window = (SDL_Window*)calloc(1, sizeof(SDL_Window));
    if (window != NULL && title != NULL) {
        snprintf(window->title, sizeof(window->title), "%s", title);
    }
    return window;
}

void SDL_DestroyWindow(SDL_Window* window)
{
    free(window);
}

void SDL_SetWindowTitle(SDL_Window* window, const char* title)
{
    if (window != NULL && title != NULL) {
        snprintf(window->title, sizeof(window->title), "%s", title);
    }
}

SDL_Renderer* SDL_CreateRenderer(SDL_Window* window, int index, Uint32 flags)
{
    (void)window;
    (void)index;
    (void)flags;

    ps2_video_init();

    return (SDL_Renderer*)calloc(1, sizeof(SDL_Renderer));
}

void SDL_DestroyRenderer(SDL_Renderer* renderer)
{
    free(renderer);
    ps2_video_shutdown();
}

int SDL_RenderSetLogicalSize(SDL_Renderer* renderer, int w, int h)
{
    if (renderer != NULL) {
        renderer->logical_w = w;
        renderer->logical_h = h;
    }
    return 0;
}

SDL_Texture* SDL_CreateTexture(SDL_Renderer* renderer, Uint32 format, int access, int w, int h)
{
    (void)renderer;
    (void)access;

    SDL_Texture* texture = (SDL_Texture*)calloc(1, sizeof(SDL_Texture));
    if (texture != NULL) {
        texture->format = format;
        texture->w = w;
        texture->h = h;
    }
    return texture;
}

void SDL_DestroyTexture(SDL_Texture* texture)
{
    if (texture == s_last_texture) {
        s_last_texture = NULL;
    }
    free(texture);
}

int SDL_QueryTexture(SDL_Texture* texture, Uint32* format, int* access, int* w, int* h)
{
    if (texture == NULL) {
        return -1;
    }

    if (format != NULL) {
        *format = SDL_PIXELFORMAT_RGB888;
    }
    if (access != NULL) {
        *access = SDL_TEXTUREACCESS_STREAMING;
    }
    if (w != NULL) {
        *w = texture->w;
    }
    if (h != NULL) {
        *h = texture->h;
    }
    return 0;
}

int SDL_UpdateTexture(SDL_Texture* texture, const SDL_Rect* rect, const void* pixels, int pitch)
{
    (void)rect;

    if (texture == NULL) {
        return -1;
    }

    texture->pixels = pixels;
    texture->pitch = pitch;
    s_last_texture = texture;

    return 0;
}

int SDL_RenderClear(SDL_Renderer* renderer)
{
    (void)renderer;
    return 0;
}

int SDL_RenderCopy(SDL_Renderer* renderer, SDL_Texture* texture, const SDL_Rect* srcrect, const SDL_Rect* dstrect)
{
    (void)renderer;
    (void)texture;
    (void)srcrect;
    (void)dstrect;
    return 0;
}

void SDL_RenderPresent(SDL_Renderer* renderer)
{
    (void)renderer;

    if (s_palette_dirty) {
        Uint32 rgba[256];
        for (int i = 0; i < 256; i++) {
            rgba[i] = ((Uint32)s_game_palette[i].a << 24)
                | ((Uint32)s_game_palette[i].b << 16)
                | ((Uint32)s_game_palette[i].g << 8)
                | (Uint32)s_game_palette[i].r;
        }
        ps2_video_set_palette(rgba);
        s_palette_dirty = 0;
    }

    if (s_last_texture != NULL && s_last_texture->pixels != NULL) {
        ps2_video_present(s_last_texture->pixels, s_last_texture->w, s_last_texture->h, s_last_texture->pitch);
    }

    /* Stream finished audio chunks to audsrv from this (engine) thread; the
     * mixer runs on its own thread and never touches the SIF (see
     * ps2_audio.h). Doing it here guarantees a feed even on frames where
     * the engine skips the event pump. */
    ps2_audio_pump();
}

/* ---------------------------------------------------------------------- */
/* Cursor                                                                  */
/* ---------------------------------------------------------------------- */

static int s_cursor_visible = 1;
static SDL_Cursor s_system_cursor = { 0 };

int SDL_ShowCursor(int toggle)
{
    int previous = s_cursor_visible;
    if (toggle >= 0) {
        s_cursor_visible = toggle;
    }
    return previous;
}

SDL_Cursor* SDL_CreateSystemCursor(int id)
{
    (void)id;
    return &s_system_cursor;
}

SDL_Cursor* SDL_GetCursor(void)
{
    return &s_system_cursor;
}

void SDL_SetCursor(SDL_Cursor* cursor)
{
    (void)cursor;
}

void SDL_FreeCursor(SDL_Cursor* cursor)
{
    /* Only the static system cursor exists; nothing to free. */
    (void)cursor;
}

/* ---------------------------------------------------------------------- */
/* Events                                                                  */
/* ---------------------------------------------------------------------- */

#define EVENT_QUEUE_SIZE 64

static SDL_Event s_event_queue[EVENT_QUEUE_SIZE];
static int s_event_head;
static int s_event_count;
static int s_startup_events_posted;

/* Keyboard state as exposed by SDL_GetKeyboardState(): one entry per
 * scancode. Both the pad/OSK event path below and SDL_PushEvent keep it up to
 * date, so polling readers and event readers agree. */
static Uint8 s_keyboard_state[SDL_NUM_SCANCODES];

static void sdl_keyboard_state_set(int scancode, int down)
{
    if (scancode >= 0 && scancode < SDL_NUM_SCANCODES) {
        s_keyboard_state[scancode] = down ? SDL_PRESSED : SDL_RELEASED;
    }
}

/* Mouse state, shared between the event builders below and
 * SDL_GetRelativeMouseState/SDL_PollEvent consumers. */
static Sint32 s_mouse_xrel;
static Sint32 s_mouse_yrel;
static Sint32 s_mouse_x;
static Sint32 s_mouse_y;
static Uint32 s_mouse_buttons;

void ps2_sdl_push_event(const SDL_Event* event)
{
    SDL_Event e = *event;

    if (s_event_count == EVENT_QUEUE_SIZE) {
        /* Drop the oldest event; newest input must keep flowing. */
        s_event_head = (s_event_head + 1) % EVENT_QUEUE_SIZE;
        s_event_count--;
    }

    s_event_queue[(s_event_head + s_event_count) % EVENT_QUEUE_SIZE] = e;
    s_event_count++;
}

int SDL_PushEvent(SDL_Event* event)
{
    if (event == NULL) {
        return -1;
    }

    /* sfall's keyboard helpers build key events with only `type` and
     * `keysym.scancode` initialized (see sfall_kb_press_key), which leaves
     * `key.state` indeterminate for the engine's event handler. Deriving the
     * state from the event type makes the press/release pair deterministic
     * and keeps the polling state (SDL_GetKeyboardState) in sync. */
    if (event->type == SDL_KEYDOWN || event->type == SDL_KEYUP) {
        event->key.state = event->type == SDL_KEYDOWN ? SDL_PRESSED : SDL_RELEASED;
        sdl_keyboard_state_set(event->key.keysym.scancode, event->key.state);
    }

    ps2_sdl_push_event(event);
    return 0;
}

static int event_pop(SDL_Event* event)
{
    if (s_event_count <= 0) {
        return 0;
    }

    *event = s_event_queue[s_event_head];
    s_event_head = (s_event_head + 1) % EVENT_QUEUE_SIZE;
    s_event_count--;
    return 1;
}

void ps2_sdl_post_key(int scancode, int down)
{
    SDL_Event e;
    memset(&e, 0, sizeof(e));
    e.type = down ? SDL_KEYDOWN : SDL_KEYUP;
    e.key.state = down ? SDL_PRESSED : SDL_RELEASED;
    e.key.keysym.scancode = (SDL_Scancode)scancode;
    sdl_keyboard_state_set(scancode, down);
    ps2_sdl_push_event(&e);
}

void ps2_sdl_post_mouse_move(int dx, int dy)
{
    if (dx == 0 && dy == 0) {
        return;
    }

    s_mouse_xrel += dx;
    s_mouse_yrel += dy;

    /* Keep the absolute cursor inside the game's 640x480 surface. The
     * engine consumes relative deltas and clips its own cursor, so only
     * the absolute state (event coordinates, SDL_GetMouseState-style
     * readers) is bounded here; the deltas pass through unchanged. */
    s_mouse_x += dx;
    s_mouse_y += dy;
    if (s_mouse_x < 0) {
        s_mouse_x = 0;
    } else if (s_mouse_x > 639) {
        s_mouse_x = 639;
    }
    if (s_mouse_y < 0) {
        s_mouse_y = 0;
    } else if (s_mouse_y > 479) {
        s_mouse_y = 479;
    }

    SDL_Event e;
    memset(&e, 0, sizeof(e));
    e.type = SDL_MOUSEMOTION;
    e.motion.x = s_mouse_x;
    e.motion.y = s_mouse_y;
    e.motion.xrel = dx;
    e.motion.yrel = dy;
    ps2_sdl_push_event(&e);
}

void ps2_sdl_post_mouse_button(int sdl_button, int down)
{
    if (sdl_button < 1 || sdl_button > 32) {
        return;
    }

    if (down) {
        s_mouse_buttons |= SDL_BUTTON(sdl_button);
    } else {
        s_mouse_buttons &= ~(Uint32)SDL_BUTTON(sdl_button);
    }

    SDL_Event e;
    memset(&e, 0, sizeof(e));
    e.type = down ? SDL_MOUSEBUTTONDOWN : SDL_MOUSEBUTTONUP;
    e.button.button = (Uint8)sdl_button;
    e.button.state = down ? SDL_PRESSED : SDL_RELEASED;
    e.button.x = s_mouse_x;
    e.button.y = s_mouse_y;
    ps2_sdl_push_event(&e);
}

void ps2_sdl_post_mouse_wheel(int dx, int dy)
{
    SDL_Event e;
    memset(&e, 0, sizeof(e));
    e.type = SDL_MOUSEWHEEL;
    e.wheel.x = dx;
    e.wheel.y = dy;
    ps2_sdl_push_event(&e);
}

void ps2_sdl_post_window_event(int window_event)
{
    SDL_Event e;
    memset(&e, 0, sizeof(e));
    e.type = SDL_WINDOWEVENT;
    e.window.event = (Uint8)window_event;
    ps2_sdl_push_event(&e);
}

/* ---------------------------------------------------------------------- */
/* Mouse / keyboard state                                                  */
/* ---------------------------------------------------------------------- */

static int s_text_input_active;

Uint32 SDL_GetRelativeMouseState(int* x, int* y)
{
    if (x != NULL) {
        *x = s_mouse_xrel;
    }
    if (y != NULL) {
        *y = s_mouse_yrel;
    }

    s_mouse_xrel = 0;
    s_mouse_yrel = 0;

    return s_mouse_buttons;
}

SDL_Keymod SDL_GetModState(void)
{
    /* PS2 has no lock keys; the pad never reports modifiers. */
    return KMOD_NONE;
}

const Uint8* SDL_GetKeyboardState(int* numkeys)
{
    if (numkeys != NULL) {
        *numkeys = SDL_NUM_SCANCODES;
    }

    return s_keyboard_state;
}

int SDL_SetRelativeMouseMode(SDL_bool enabled)
{
    (void)enabled;
    return 0;
}

void SDL_StartTextInput(void)
{
    s_text_input_active = 1;

    /* the engine just enabled an editable text widget (save-name
     * prompt, editor, file dialogs); summon the on-screen keyboard. */
    ps2_osk_open();
}

void SDL_StopTextInput(void)
{
    s_text_input_active = 0;
    ps2_osk_close();
}

/* ---------------------------------------------------------------------- */
/* Timers                                                                  */
/* ---------------------------------------------------------------------- */

#define TIMER_SLOTS 16

typedef struct TimerSlot {
    SDL_TimerID id;
    Uint32 interval;
    Uint32 next_time;
    Uint32 (*callback)(Uint32 interval, void* param);
    void* param;
    int active;
} TimerSlot;

static TimerSlot s_timers[TIMER_SLOTS];
static SDL_TimerID s_next_timer_id = 1;

SDL_TimerID SDL_AddTimer(Uint32 interval, Uint32 (*callback)(Uint32 interval, void* param), void* param)
{
    if (interval == 0) {
        interval = 1;
    }

    for (int i = 0; i < TIMER_SLOTS; i++) {
        if (!s_timers[i].active) {
            s_timers[i].id = s_next_timer_id++;
            s_timers[i].interval = interval;
            s_timers[i].callback = callback;
            s_timers[i].param = param;
            s_timers[i].next_time = ps2_time_ms() + interval;
            s_timers[i].active = 1;
            return s_timers[i].id;
        }
    }

    return 0;
}

SDL_bool SDL_RemoveTimer(SDL_TimerID id)
{
    for (int i = 0; i < TIMER_SLOTS; i++) {
        if (s_timers[i].active && s_timers[i].id == id) {
            s_timers[i].active = 0;
            return SDL_TRUE;
        }
    }

    return SDL_FALSE;
}

/* Timers run on the main thread when pumping events (see note in SDL.h). */
static void pump_timers(void)
{
    Uint32 now = ps2_time_ms();

    for (int i = 0; i < TIMER_SLOTS; i++) {
        if (s_timers[i].active && (Sint32)(now - s_timers[i].next_time) >= 0) {
            Uint32 interval = s_timers[i].interval;
            Uint32 (*callback)(Uint32, void*) = s_timers[i].callback;
            void* param = s_timers[i].param;

            Uint32 next = callback(interval, param);

            /* The callback may have removed itself. */
            if (s_timers[i].active) {
                if (next == 0) {
                    s_timers[i].active = 0;
                } else {
                    s_timers[i].interval = next;
                    s_timers[i].next_time = now + next;
                }
            }
        }
    }
}

/* ---------------------------------------------------------------------- */
/* Pumping                                                                 */
/* ---------------------------------------------------------------------- */

static void sdl_pump(void)
{
    if (!s_startup_events_posted) {
        s_startup_events_posted = 1;

        /* The engine treats the window as inactive until it receives a
         * focus event; fake it so input processing is enabled from boot. */
        ps2_sdl_post_window_event(SDL_WINDOWEVENT_FOCUS_GAINED);
        ps2_sdl_post_window_event(SDL_WINDOWEVENT_EXPOSED);
    }

    ps2_input_poll();
    ps2_audio_pump();
    pump_timers();
}

void SDL_PumpEvents(void)
{
    sdl_pump();
}

int SDL_PollEvent(SDL_Event* event)
{
    sdl_pump();

    if (event == NULL) {
        return s_event_count > 0;
    }

    return event_pop(event);
}

void SDL_FlushEvents(Uint32 minType, Uint32 maxType)
{
    SDL_Event kept[EVENT_QUEUE_SIZE];
    int kept_count = 0;

    for (int i = 0; i < s_event_count; i++) {
        SDL_Event* e = &s_event_queue[(s_event_head + i) % EVENT_QUEUE_SIZE];
        if (e->type < minType || e->type >= maxType) {
            kept[kept_count++] = *e;
        }
    }

    memcpy(s_event_queue, kept, (size_t)kept_count * sizeof(SDL_Event));
    s_event_head = 0;
    s_event_count = kept_count;
}

/* ---------------------------------------------------------------------- */
/* String helpers (used by platform_compat.cc)                             */
/* ---------------------------------------------------------------------- */

int SDL_strcasecmp(const char* str1, const char* str2)
{
    while (*str1 != '\0' && *str2 != '\0') {
        int c1 = tolower((unsigned char)*str1);
        int c2 = tolower((unsigned char)*str2);
        if (c1 != c2) {
            return c1 - c2;
        }
        str1++;
        str2++;
    }
    return tolower((unsigned char)*str1) - tolower((unsigned char)*str2);
}

int SDL_strncasecmp(const char* str1, const char* str2, size_t len)
{
    while (len > 0 && *str1 != '\0' && *str2 != '\0') {
        int c1 = tolower((unsigned char)*str1);
        int c2 = tolower((unsigned char)*str2);
        if (c1 != c2) {
            return c1 - c2;
        }
        str1++;
        str2++;
        len--;
    }
    if (len == 0) {
        return 0;
    }
    return tolower((unsigned char)*str1) - tolower((unsigned char)*str2);
}

char* SDL_strupr(char* str)
{
    for (char* p = str; *p != '\0'; p++) {
        *p = (char)toupper((unsigned char)*p);
    }
    return str;
}

char* SDL_strlwr(char* str)
{
    for (char* p = str; *p != '\0'; p++) {
        *p = (char)tolower((unsigned char)*p);
    }
    return str;
}

char* SDL_itoa(int value, char* str, int radix)
{
    if (radix == 10) {
        sprintf(str, "%d", value);
    } else if (radix == 16) {
        sprintf(str, "%x", value);
    } else if (radix == 8) {
        sprintf(str, "%o", value);
    } else {
        /* Generic radix conversion. */
        char* p = str;
        char tmp[36];
        int i = 0;
        unsigned int u = (value < 0 && radix == 10) ? (unsigned int)-value : (unsigned int)value;
        if (value == 0) {
            tmp[i++] = '0';
        }
        while (u != 0 && i < (int)sizeof(tmp)) {
            int digit = u % radix;
            tmp[i++] = (digit < 10) ? (char)('0' + digit) : (char)('a' + digit - 10);
            u /= radix;
        }
        if (value < 0 && radix == 10) {
            *p++ = '-';
        }
        while (i > 0) {
            *p++ = tmp[--i];
        }
        *p = '\0';
    }
    return str;
}

char* SDL_strdup(const char* str)
{
    size_t len = strlen(str) + 1;
    char* copy = (char*)malloc(len);
    if (copy != NULL) {
        memcpy(copy, str, len);
    }
    return copy;
}

void SDL_free(void* mem)
{
    free(mem);
}

/* ---------------------------------------------------------------------- */
/* Logging / dialogs                                                       */
/* ---------------------------------------------------------------------- */

void SDL_LogMessageV(int category, int priority, const char* fmt, va_list ap)
{
    (void)category;
    (void)priority;

    vprintf(fmt, ap);
    putchar('\n');
}

void SDL_ShowSimpleMessageBox(Uint32 flags, const char* title, const char* message, SDL_Window* window)
{
    (void)flags;
    (void)window;

    printf("[messagebox] %s: %s\n", title != NULL ? title : "", message != NULL ? message : "");
}

/* ---------------------------------------------------------------------- */
/* Audio: audsrv PCM device + SDL_AudioStream conversion              */
/* ---------------------------------------------------------------------- */

#define PS2_SDL_AUDIO_DEVICE_ID 1

static int s_audio_device_open;
static SDL_AudioCallback s_audio_device_callback;
static void* s_audio_device_userdata;

static void ps2_sdl_audio_mix(void* userdata, unsigned char* stream, int len)
{
    (void)userdata;

    if (s_audio_device_callback != NULL) {
        s_audio_device_callback(s_audio_device_userdata, stream, len);
    }
}

SDL_AudioDeviceID SDL_OpenAudioDevice(const char* device, int iscapture, const SDL_AudioSpec* desired, SDL_AudioSpec* obtained, int allowed_changes)
{
    int freq = 0;

    (void)device;
    (void)allowed_changes;

    if (desired == NULL || desired->callback == NULL || desired->freq <= 0 || iscapture != 0) {
        return (SDL_AudioDeviceID)-1;
    }

    /* The engine follows up with PauseAudioDevice(dev, 0) to start playback. */
    if (ps2_audio_open(desired->freq, desired->samples, ps2_sdl_audio_mix, NULL, &freq) != 0) {
        /* No audsrv/IOP audio available. Return -1 (what audioEngineInit
         * checks) so the engine disables sound instead of hanging on a
         * callback that would never run. */
        return (SDL_AudioDeviceID)-1;
    }

    s_audio_device_open = 1;
    s_audio_device_callback = desired->callback;
    s_audio_device_userdata = desired->userdata;

    if (obtained != NULL) {
        memset(obtained, 0, sizeof(*obtained));
        obtained->freq = freq;
        obtained->format = AUDIO_S16SYS;
        obtained->channels = 2;
        obtained->silence = 0;
        obtained->samples = desired->samples;
        obtained->callback = desired->callback;
        obtained->userdata = desired->userdata;
    }

    return PS2_SDL_AUDIO_DEVICE_ID;
}

void SDL_CloseAudioDevice(SDL_AudioDeviceID dev)
{
    if (dev != PS2_SDL_AUDIO_DEVICE_ID || !s_audio_device_open) {
        return;
    }

    s_audio_device_open = 0;
    s_audio_device_callback = NULL;
    s_audio_device_userdata = NULL;

    ps2_audio_close();
}

void SDL_PauseAudioDevice(SDL_AudioDeviceID dev, int pause_on)
{
    if (dev != PS2_SDL_AUDIO_DEVICE_ID || !s_audio_device_open) {
        return;
    }

    ps2_audio_pause(pause_on);
}

/* ---- SDL_AudioStream conversion ---------------------------------------- */

static int audio_format_bytes(Uint16 format)
{
    if (format == AUDIO_S8 || format == AUDIO_U8) {
        return 1;
    }

    return 2;
}

static int audio_sample_to_s16(const unsigned char* sample, Uint16 format)
{
    if (format == AUDIO_S8) {
        return (int)(Sint8)sample[0] << 8;
    }

    if (format == AUDIO_U8) {
        return ((int)sample[0] - 128) << 8;
    }

    return (int)(Sint16)(sample[0] | ((int)sample[1] << 8));
}

static void audio_ring_read(const SDL_AudioStream* stream, int offset, unsigned char* dst, int count)
{
    int start = (stream->input_head + offset) % stream->input_capacity;
    int first = stream->input_capacity - start;

    if (first >= count) {
        memcpy(dst, stream->input + start, (size_t)count);
    } else {
        memcpy(dst, stream->input + start, (size_t)first);
        memcpy(dst + first, stream->input, (size_t)(count - first));
    }
}

static void audio_stream_read_frame(const SDL_AudioStream* stream, int frame_index, int channels[2])
{
    unsigned char frame[8];
    int sample_bytes = audio_format_bytes(stream->src_format);

    audio_ring_read(stream, frame_index * stream->src_frame_bytes, frame, stream->src_frame_bytes);

    if (stream->src_channels == 1) {
        channels[0] = audio_sample_to_s16(frame, stream->src_format);
        channels[1] = channels[0];
    } else {
        channels[0] = audio_sample_to_s16(frame, stream->src_format);
        channels[1] = audio_sample_to_s16(frame + sample_bytes, stream->src_format);
    }
}

static int audio_stream_write_frame(unsigned char* dst, const SDL_AudioStream* stream, int left, int right)
{
    if (stream->dst_format == AUDIO_S16LSB) {
        if (stream->dst_channels == 1) {
            int value = (left + right) / 2;

            if (value > 32767) {
                value = 32767;
            } else if (value < -32768) {
                value = -32768;
            }

            dst[0] = (unsigned char)(value & 0xFF);
            dst[1] = (unsigned char)((value >> 8) & 0xFF);
            return 2;
        }

        dst[0] = (unsigned char)(left & 0xFF);
        dst[1] = (unsigned char)((left >> 8) & 0xFF);
        dst[2] = (unsigned char)(right & 0xFF);
        dst[3] = (unsigned char)((right >> 8) & 0xFF);
        return 4;
    }

    /* Defensive fallbacks (the device format is always S16). */
    {
        int value = (left + right) / 2;

        if (stream->dst_format == AUDIO_U8) {
            value = ((value >> 8) + 128);
            if (value < 0) {
                value = 0;
            } else if (value > 255) {
                value = 255;
            }
            dst[0] = (unsigned char)value;
            return 1;
        }

        value >>= 8;
        if (value > 127) {
            value = 127;
        } else if (value < -128) {
            value = -128;
        }
        dst[0] = (unsigned char)value;
        return 1;
    }
}

SDL_AudioStream* SDL_NewAudioStream(Uint16 src_format, Uint8 src_channels, int src_rate, Uint16 dst_format, Uint8 dst_channels, int dst_rate)
{
    SDL_AudioStream* stream = (SDL_AudioStream*)calloc(1, sizeof(SDL_AudioStream));
    if (stream == NULL) {
        return NULL;
    }

    stream->src_format = src_format;
    stream->src_channels = src_channels > 0 ? src_channels : 1;
    stream->src_rate = src_rate;
    stream->dst_format = dst_format;
    stream->dst_channels = dst_channels > 0 ? dst_channels : 1;
    stream->dst_rate = dst_rate;

    stream->input_capacity = 8192;
    stream->input = (unsigned char*)malloc((size_t)stream->input_capacity);
    if (stream->input == NULL) {
        free(stream);
        return NULL;
    }

    stream->src_frame_bytes = audio_format_bytes(src_format) * stream->src_channels;
    stream->dst_frame_bytes = audio_format_bytes(dst_format) * stream->dst_channels;

    stream->rate_step = (src_rate > 0 && dst_rate > 0)
        ? (int)(((unsigned long long)src_rate << 16) / (unsigned int)dst_rate)
        : (1 << 16);
    if (stream->rate_step <= 0) {
        stream->rate_step = 1 << 16;
    }

    stream->passthrough = src_format == dst_format
        && src_channels == dst_channels
        && src_rate == dst_rate;

    return stream;
}

int SDL_AudioStreamPut(SDL_AudioStream* stream, const void* buf, int len)
{
    const unsigned char* src = (const unsigned char*)buf;
    int space;

    if (stream == NULL || buf == NULL || len < 0) {
        return -1;
    }

    if (len == 0) {
        return 0;
    }

    if (len > stream->input_capacity) {
        src += len - stream->input_capacity;
        len = stream->input_capacity;
    }

    space = stream->input_capacity - stream->input_len;

    while (space < len) {
        /* Ring full (should not happen with the engine's put/get rhythm):
         * drop the oldest frame to make room. */
        int drop_frames = 1;
        int drop_bytes = drop_frames * stream->src_frame_bytes;

        stream->input_head = (stream->input_head + drop_bytes) % stream->input_capacity;
        stream->input_len -= drop_bytes;

        if (stream->src_pos >= (drop_frames << 16)) {
            stream->src_pos -= drop_frames << 16;
        } else {
            stream->src_pos = 0;
        }

        space = stream->input_capacity - stream->input_len;
    }

    {
        int tail = (stream->input_head + stream->input_len) % stream->input_capacity;
        int first = stream->input_capacity - tail;

        if (first >= len) {
            memcpy(stream->input + tail, src, (size_t)len);
        } else {
            memcpy(stream->input + tail, src, (size_t)first);
            memcpy(stream->input, src + first, (size_t)(len - first));
        }

        stream->input_len += len;
    }

    return 0;
}

int SDL_AudioStreamGet(SDL_AudioStream* stream, void* buf, int len)
{
    unsigned char* out = (unsigned char*)buf;
    int written = 0;

    if (stream == NULL || buf == NULL || len < 0) {
        return -1;
    }

    if (len == 0) {
        return 0;
    }

    if (stream->passthrough) {
        int count = stream->input_len < len ? stream->input_len : len;

        if (count > 0) {
            audio_ring_read(stream, 0, out, count);
            stream->input_head = (stream->input_head + count) % stream->input_capacity;
            stream->input_len -= count;
        }

        return count;
    }

    while (written + stream->dst_frame_bytes <= len) {
        int in_frames = stream->input_len / stream->src_frame_bytes;
        int base = stream->src_pos >> 16;
        int frac = stream->src_pos & 0xFFFF;
        int current[2];
        int left;
        int right;

        if (base >= in_frames) {
            break;
        }

        if (frac != 0 && base + 1 >= in_frames) {
            /* Wait for the next input frame to interpolate against. */
            break;
        }

        audio_stream_read_frame(stream, base, current);

        if (frac == 0) {
            left = current[0];
            right = current[1];
        } else {
            int next[2];

            audio_stream_read_frame(stream, base + 1, next);
            left = current[0] + (int)(((next[0] - current[0]) * frac) >> 16);
            right = current[1] + (int)(((next[1] - current[1]) * frac) >> 16);
        }

        written += audio_stream_write_frame(out + written, stream, left, right);
        stream->src_pos += stream->rate_step;
    }

    /* Release consumed input frames (src_pos counts them from the head). */
    {
        int consumed = stream->src_pos >> 16;
        int in_frames = stream->input_len / stream->src_frame_bytes;

        if (consumed > in_frames) {
            consumed = in_frames;
        }

        if (consumed > 0) {
            int bytes = consumed * stream->src_frame_bytes;

            stream->input_head = (stream->input_head + bytes) % stream->input_capacity;
            stream->input_len -= bytes;
            stream->src_pos -= consumed << 16;
        }
    }

    return written;
}

void SDL_FreeAudioStream(SDL_AudioStream* stream)
{
    if (stream == NULL) {
        return;
    }

    free(stream->input);
    free(stream);
}

void SDL_MixAudioFormat(Uint8* dst, const Uint8* src, SDL_AudioFormat format, Uint32 len, int volume)
{
    if (volume == 0 || dst == NULL || src == NULL) {
        return;
    }

    if (volume > SDL_MIX_MAXVOLUME) {
        volume = SDL_MIX_MAXVOLUME;
    }

    if (format == AUDIO_S16SYS) {
        Sint16* d = (Sint16*)dst;
        const Sint16* s = (const Sint16*)src;
        Uint32 count = len / 2;
        for (Uint32 i = 0; i < count; i++) {
            Sint32 mixed = (Sint32)d[i] + (Sint32)(((Sint32)s[i] * volume) / SDL_MIX_MAXVOLUME);
            if (mixed > 32767) {
                mixed = 32767;
            } else if (mixed < -32768) {
                mixed = -32768;
            }
            d[i] = (Sint16)mixed;
        }
    } else if (format == AUDIO_U8) {
        Uint8* d = dst;
        const Uint8* s = src;
        for (Uint32 i = 0; i < len; i++) {
            Sint32 mixed = (Sint32)d[i] + (((Sint32)s[i] - 128) * volume) / SDL_MIX_MAXVOLUME;
            if (mixed > 255) {
                mixed = 255;
            } else if (mixed < 0) {
                mixed = 0;
            }
            d[i] = (Uint8)mixed;
        }
    } else {
        /* Unknown format: straight copy without mixing. */
        memcpy(dst, src, len);
    }
}
