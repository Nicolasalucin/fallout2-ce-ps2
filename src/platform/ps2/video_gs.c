/*
 * Video backend for the Fallout 2 PS2 port: GS hardware-indexed rendering.
 *
 * Pipeline per frame (Option A - matches Fallout 1):
 *
 *   engine 8bpp 640x480 buffer
 *        |  CPU palette lookup through s_palette (LUT, ~1 ms on the EE)
 *        v
 *   s_fb_rgba: CT32 640x480 (1.2 MB, 64-byte aligned)
 *        |  gsKit_texture_upload (DMA to VRAM, writeback-flushed)
 *        v
 *   GS VRAM: CT32 texture (no CLUT at all)
 *        |  full-screen textured quad
 *        v
 *   framebuffer flip on vsync -> NTSC 640x448 @ 60 Hz interlaced
 *
 * Doing the palette lookup on the EE (Option A) removes the GS CLUT,
 * the CSM1 swizzle question and the T8 TBW pitfalls from the equation.
 * The texture uses GS_FILTER_NEAREST for the 480->448 downscale; with a
 * CT32 texture a linear filter would now also be color-correct if wanted.
 */

#include "ps2_video.h"
#include "ps2_time.h"
#include "ps2_osk.h"
#include "sdl/ps2_sdl_internal.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include <gsKit.h>
#include <dmaKit.h>
#include <gsToolkit.h>
#include <kernel.h>

#define PS2_SCR_W 640
#define PS2_SCR_H 480
#define PS2_OUT_W 640
#define PS2_OUT_H 448

static int s_video_inited;
static int s_present_logged;
static GSGLOBAL* s_gs;
static GSTEXTURE s_texture;

/* Per-phase accumulators (ms) for boot diagnostics; reset by the selftest. */
static u32 s_phase_ms[4];
static u32 s_phase_count;

/* Palette LUT in GS CT32 style (0xAABBGGRR), rebuilt from the engine
 * palette by ps2_video_set_palette(). Used as the CPU lookup table. */
static u32 s_palette[256] __attribute__((aligned(64)));

/* Staging buffer used only if the engine ever presents a non-640x480 or
 * non-640-pitch surface (never happens today, kept as insurance). */
static u8 s_staging[PS2_SCR_W * PS2_SCR_H] __attribute__((aligned(64)));

/* CT32 framebuffer uploaded to the GS every frame: index -> palette RGBA.
 * 1.2 MB in BSS, 64-byte aligned for the GIF DMAC. */
static u32 s_fb_rgba[PS2_SCR_W * PS2_SCR_H] __attribute__((aligned(64)));

void ps2_video_init(void)
{
    if (s_video_inited) {
        return;
    }
    s_video_inited = 1;

    dmaKit_init(D_CTRL_RELE_OFF, D_CTRL_MFD_OFF, D_CTRL_STS_UNSPEC,
        D_CTRL_STD_OFF, D_CTRL_RCYC_8, 1 << DMA_CHANNEL_GIF);
    dmaKit_chan_init(DMA_CHANNEL_GIF);

    s_gs = gsKit_init_global();

    s_gs->Mode = GS_MODE_NTSC;
    s_gs->Interlace = GS_INTERLACED;
    s_gs->Field = GS_FIELD;
    s_gs->Width = PS2_SCR_W;
    s_gs->Height = PS2_OUT_H;
    s_gs->PSM = GS_PSM_CT32;
    s_gs->PSMZ = GS_PSMZ_16S;
    s_gs->ZBuffering = GS_SETTING_OFF;
    s_gs->DoubleBuffering = GS_SETTING_ON;
    s_gs->Dithering = GS_SETTING_OFF;
    s_gs->PrimAlphaEnable = GS_SETTING_OFF;

    gsKit_init_screen(s_gs);
    gsKit_mode_switch(s_gs, GS_ONESHOT);

    memset(s_palette, 0, sizeof(s_palette));

    memset(&s_texture, 0, sizeof(s_texture));
    s_texture.Width = PS2_SCR_W;
    s_texture.Height = PS2_SCR_H;
    s_texture.PSM = GS_PSM_CT32;
    /* 480->448 downscale: NEAREST avoids resampling artifacts. With CT32
     * the data is real color, so a linear filter would be correct too. */
    s_texture.Filter = GS_FILTER_NEAREST;
    gsKit_setup_tbw(&s_texture);
    s_texture.Vram = gsKit_vram_alloc(s_gs,
        gsKit_texture_size(PS2_SCR_W, PS2_SCR_H, GS_PSM_CT32), GS_VRAM_BLOCKSIZE_256);
    s_texture.Mem = (u32*)s_fb_rgba;

    printf("[ps2] video: NTSC 640x448 60Hz, CT32 tex @ %08x (TBW %u), CPU palette LUT\n",
        (unsigned)s_texture.Vram, (unsigned)s_texture.TBW);
}

void ps2_video_shutdown(void)
{
    /* The engine exits right after this; the GS state is not reused. */
    s_video_inited = 0;
}

static void capture_maybe_dump(const u8* indices, int osk);

void ps2_video_present(const void* pixels, int width, int height, int pitch)
{
    if (!s_video_inited) {
        ps2_video_init();
    }

    if (pixels == NULL) {
        return;
    }

    if (!s_present_logged) {
        s_present_logged = 1;
        printf("[ps2] video: first present mem=%p pitch=%d, 64B aligned: %s\n",
            (void*)pixels, pitch, ((uintptr_t)pixels & 63u) == 0 ? "yes" : "no");
    }

    const u8* indices;
    if (width != PS2_SCR_W || height != PS2_SCR_H || pitch != PS2_SCR_W) {
        /* Normalize exotic surfaces into the staging buffer. */
        int rows = height < PS2_SCR_H ? height : PS2_SCR_H;
        int cols = width < PS2_SCR_W ? width : PS2_SCR_W;
        memset(s_staging, 0, sizeof(s_staging));
        for (int y = 0; y < rows; y++) {
            memcpy(s_staging + (size_t)y * PS2_SCR_W,
                (const u8*)pixels + (size_t)y * pitch, (size_t)cols);
        }
        indices = s_staging;
    } else {
        indices = (const u8*)pixels;
    }

    {
        u32 t0 = ps2_time_ms();
        u32 t1, t2, t3, t4;

        /* Dev capture: dump the frame that is about to be presented (the
         * palette snapshot is taken right here, so the pair is coherent). */
        capture_maybe_dump(indices, ps2_osk_is_open());

        /* Frame-buffer lifetime barrier: gsKit_texture_send() already waits
         * for the GIF DMA to finish before returning, but make the contract
         * explicit here because the next step overwrites s_fb_rgba while the
         * DMAC may still be reading it. */
        dmaKit_wait_fast();

        /* Paletted -> CT32 on the EE (Option A): one LUT read per pixel.
         * s_palette is already in GS CT32 byte order (A B G R), so every
         * entry can be stored verbatim. No GS CLUT involved.
         *
         * Loop unrolled 8x for EE (MIPS R5900) superscalar dispatch.
         * Indices are 8-bit; loads are byte-wide, stores are 32-bit aligned.
         * `restrict` lets GCC schedule the LUT loads and the 32-bit stores
         * without conservative alias barriers. */
        u32* restrict dst = s_fb_rgba;
        const u32* restrict lut = s_palette;
        const u8* restrict src = indices;
        const int pixel_count = PS2_SCR_W * PS2_SCR_H;
        int i = 0;
        for (; i + 7 < pixel_count; i += 8) {
            dst[i + 0] = lut[src[i + 0]];
            dst[i + 1] = lut[src[i + 1]];
            dst[i + 2] = lut[src[i + 2]];
            dst[i + 3] = lut[src[i + 3]];
            dst[i + 4] = lut[src[i + 4]];
            dst[i + 5] = lut[src[i + 5]];
            dst[i + 6] = lut[src[i + 6]];
            dst[i + 7] = lut[src[i + 7]];
        }
        for (; i < pixel_count; i++) {
            dst[i] = lut[src[i]];
        }

        /* F8: composite the on-screen keyboard over the freshly built
         * frame (engine buffers untouched; no-op while closed). */
        if (ps2_osk_is_open()) {
            ps2_osk_render(s_fb_rgba, PS2_SCR_W, PS2_SCR_H);
        }

        s_texture.Mem = (u32*)s_fb_rgba;

        /* Write back exactly the framebuffer range that the GIF DMAC will
         * fetch. The texture spans 640*480*4 = 1,228,800 bytes; flushing the
         * whole D-cache here would also write back mixer/file buffers for no
         * benefit (gsKit_texture_send does its own writeback as a safety net,
         * so this explicit call is the precise one that matters). */
        SyncDCache((void*)s_fb_rgba, (void*)(s_fb_rgba + pixel_count));
        gsKit_texture_upload(s_gs, &s_texture);
        t1 = ps2_time_ms();

        gsKit_clear(s_gs, GS_SETREG_RGBAQ(0, 0, 0, 0, 0));
        gsKit_prim_sprite_texture_3d(s_gs, &s_texture,
            0.0f, 0.0f, 1, 0.0f, 0.0f,
            (float)PS2_OUT_W, (float)PS2_OUT_H, 1, (float)PS2_SCR_W, (float)PS2_SCR_H,
            GS_SETREG_RGBAQ(0x80, 0x80, 0x80, 0x80, 0));
        t2 = ps2_time_ms();

        gsKit_queue_exec(s_gs);
        t3 = ps2_time_ms();

        gsKit_sync_flip(s_gs);
        t4 = ps2_time_ms();

        s_phase_ms[0] += t1 - t0;
        s_phase_ms[1] += t2 - t1;
        s_phase_ms[2] += t3 - t2;
        s_phase_ms[3] += t4 - t3;
        s_phase_count++;
    }
}

void ps2_video_set_palette(const void* rgba_colors)
{
    const u32* src = (const u32*)rgba_colors;

    if (src == NULL) {
        return;
    }

    for (int i = 0; i < 256; i++) {
        u32 color = src[i];
        u32 alpha = (color >> 24) & 0xFF;
        if (alpha > 0x80) {
            /* The GS clamps alpha at 0x80 (1.0). */
            alpha = 0x80;
        }
        s_palette[i] = (color & 0x00FFFFFF) | (alpha << 24);
    }
}

/* ---------------------------------------------------------------------- */
/* Boot self-test pattern helpers                                         */
/* ---------------------------------------------------------------------- */

static u8 selftest_pixel(int x, int y, int rotation)
{
    if (x < 2 || x >= PS2_SCR_W - 2 || y < 2 || y >= PS2_SCR_H - 2) {
        return 1; /* white border */
    }
    if (y >= 40 && y < 440) {
        int bar = x / (PS2_SCR_W / 8);
        if (bar > 7) {
            bar = 7;
        }
        return (u8)(2 + ((bar + rotation) % 8));
    }
    return (u8)(32 + (y * 63 / (PS2_SCR_H - 1))); /* gray gradient */
}

static void selftest_build(u8* pattern, int rotation)
{
    for (int y = 0; y < PS2_SCR_H; y++) {
        u8* row = pattern + (size_t)y * PS2_SCR_W;
        for (int x = 0; x < PS2_SCR_W; x++) {
            row[x] = selftest_pixel(x, y, rotation);
        }
    }
}

static void selftest_strip(u8* pattern, int col, int rotation)
{
    for (int y = 20; y < 460; y++) {
        u8* row = pattern + (size_t)y * PS2_SCR_W;
        for (int x = col; x < col + 8 && x < PS2_SCR_W; x++) {
            row[x] = selftest_pixel(x, y, rotation);
        }
    }
}

static void selftest_column(u8* pattern, int col)
{
    for (int y = 20; y < 460; y++) {
        u8* row = pattern + (size_t)y * PS2_SCR_W;
        for (int x = col; x < col + 8 && x < PS2_SCR_W; x++) {
            row[x] = 1; /* white column */
        }
    }
}

/*
 * Boot self-test: draws a pattern that exercises every part of the path
 * (texture streaming, CLUT uploads, vsync flip) and compares 120 vsyncs
 * against the EE millisecond timer. 120 frames at 60 Hz must take ~2000 ms;
 * a large deviation would indicate a wrong timer divisor (see ps2_time.c).
 */
void ps2_video_selftest(void)
{
    static u8 pattern[PS2_SCR_W * PS2_SCR_H] __attribute__((aligned(64)));
    static u32 test_palette[256];
    int last_col = 0;
    int last_rotation = -1;
    int rotation;

    ps2_video_init();

    /* Palette: 0 black, 1 white, 2..9 bright colors, 32..95 gray ramp. */
    for (int i = 0; i < 256; i++) {
        test_palette[i] = 0x80000000u | ((u32)i << 16) | ((u32)i << 8) | (u32)i; /* dark blue-ish default */
    }
    test_palette[0] = 0x80000000u;
    test_palette[1] = 0x80FFFFFFu;
    static const u32 bar_colors[8] = {
        0x000000FFu, /* red */
        0x0000FF00u, /* green */
        0x00FF0000u, /* blue */
        0x0000FFFFu, /* yellow */
        0x00FF00FFu, /* magenta */
        0x00FFFF00u, /* cyan */
        0x008080FFu, /* orange */
        0x00FF80FFu, /* purple */
    };
    for (int i = 0; i < 8; i++) {
        test_palette[2 + i] = 0x80000000u | bar_colors[i];
    }
    for (int i = 0; i < 64; i++) {
        u32 g = (u32)(i * 255 / 63);
        test_palette[32 + i] = 0x80000000u | (g << 16) | (g << 8) | g;
    }

    printf("[ps2] selftest: starting (timer %u ms)\n", (unsigned)ps2_time_ms());

    for (int frame = 0; frame < 120; frame++) {
        int col = (frame * 8) % PS2_SCR_W;

        /* Drive the platform input layer while the engine is not running
         * yet (boot demo); the engine pumps events by itself later. */
        SDL_PumpEvents();

        rotation = frame / 30;
        if (rotation != last_rotation) {
            selftest_build(pattern, rotation);
            last_rotation = rotation;
        } else {
            selftest_strip(pattern, last_col, rotation); /* erase previous column */
        }
        selftest_column(pattern, col);

        ps2_video_set_palette(test_palette);
        ps2_video_present(pattern, PS2_SCR_W, PS2_SCR_H, PS2_SCR_W);

        last_col = col;
    }

    printf("[ps2] selftest: 120 frames done (timer %u ms, expected ~2000)\n",
        (unsigned)ps2_time_ms());
    ps2_video_debug_print_phases();

    /* Timing probes: attribute the per-frame cost between vsync waits, DMA
     * upload, draw/flip and pure CPU work. Used to locate 30fps lock. */
    {
        u32 t0, t1;

        t0 = ps2_time_ms();
        for (int i = 0; i < 120; i++) {
            gsKit_vsync_wait();
        }
        t1 = ps2_time_ms();
        printf("[ps2] probe: vsync_wait x120 -> %u ms\n", (unsigned)(t1 - t0));

        t0 = ps2_time_ms();
        s_texture.Mem = (u32*)pattern;
        for (int i = 0; i < 120; i++) {
            gsKit_texture_upload(s_gs, &s_texture);
        }
        t1 = ps2_time_ms();
        printf("[ps2] probe: upload x120 -> %u ms\n", (unsigned)(t1 - t0));

        t0 = ps2_time_ms();
        for (int i = 0; i < 120; i++) {
            gsKit_clear(s_gs, GS_SETREG_RGBAQ(0, 0, 0, 0, 0));
            gsKit_prim_sprite_texture_3d(s_gs, &s_texture,
                0.0f, 0.0f, 1, 0.0f, 0.0f,
                (float)PS2_OUT_W, (float)PS2_OUT_H, 1, (float)PS2_SCR_W, (float)PS2_SCR_H,
                GS_SETREG_RGBAQ(0x80, 0x80, 0x80, 0x80, 0));
            gsKit_queue_exec(s_gs);
            gsKit_sync_flip(s_gs);
        }
        t1 = ps2_time_ms();
        printf("[ps2] probe: draw+flip x120 -> %u ms\n", (unsigned)(t1 - t0));

        t0 = ps2_time_ms();
        for (int i = 0; i < 120; i++) {
            u8* p = pattern;
            for (int y = 0; y < PS2_SCR_H; y++) {
                u8 row_base = (u8)(32 + (y * 63 / (PS2_SCR_H - 1)));
                for (int x = 0; x < PS2_SCR_W; x++) {
                    p[x] = row_base;
                }
                p += PS2_SCR_W;
            }
        }
        t1 = ps2_time_ms();
        printf("[ps2] probe: pattern(%d px) x120 -> %u ms\n", PS2_SCR_W * PS2_SCR_H,
            (unsigned)(t1 - t0));

        /* Raw GS throughput: upload+draw with no vsync sync at all. */
        t0 = ps2_time_ms();
        for (int i = 0; i < 120; i++) {
            s_texture.Mem = (u32*)pattern;
            gsKit_texture_upload(s_gs, &s_texture);
            gsKit_clear(s_gs, GS_SETREG_RGBAQ(0, 0, 0, 0, 0));
            gsKit_prim_sprite_texture_3d(s_gs, &s_texture,
                0.0f, 0.0f, 1, 0.0f, 0.0f,
                (float)PS2_OUT_W, (float)PS2_OUT_H, 1, (float)PS2_SCR_W, (float)PS2_SCR_H,
                GS_SETREG_RGBAQ(0x80, 0x80, 0x80, 0x80, 0));
            gsKit_queue_exec(s_gs);
        }
        t1 = ps2_time_ms();
        printf("[ps2] probe: upload+draw (no vsync) x120 -> %u ms\n", (unsigned)(t1 - t0));

        /* Upload impact on vsync arrival: no draw in between. */
        t0 = ps2_time_ms();
        for (int i = 0; i < 120; i++) {
            s_texture.Mem = (u32*)pattern;
            gsKit_texture_upload(s_gs, &s_texture);
            gsKit_vsync_wait();
        }
        t1 = ps2_time_ms();
        printf("[ps2] probe: upload+vsync x120 -> %u ms\n", (unsigned)(t1 - t0));
    }

    /* Input window: keep presenting at 60 Hz while pumping events so the pad
     * layer is polled and its transitions are logged. Temporary F3 harness;
     * F8 replaces this with the real boot/mod menu. */
    {
        u32 t0 = ps2_time_ms();

        /* Bring back the color bars (the pattern probe above leaves only the
         * gray ramp): they exercise palette indices 2..9 and 16/17 through
         * the CPU LUT, so they double as a visual check of the palette order. */
        selftest_build(pattern, 0);

        printf("[ps2] selftest: input window (600 frames)\n");
        for (int frame = 0; frame < 600; frame++) {
            SDL_PumpEvents();
            /* Full present path (LUT conversion included) at 60 Hz so the
             * color bars on screen double as a palette-order check. */
            ps2_video_present(pattern, PS2_SCR_W, PS2_SCR_H, PS2_SCR_W);
        }
        printf("[ps2] selftest: input window done (%u ms)\n", (unsigned)(ps2_time_ms() - t0));
    }
}

/* ---------------------------------------------------------------------- */
/* Palette/CLUT diagnostics and frame capture (development diagnostics)    */
/* ---------------------------------------------------------------------- */

static unsigned int s_palette_generation;
static int s_palette_trace_on;
static unsigned int s_palette_trace_last_ms;
static unsigned int s_palette_trace_last_crc;
static int s_palette_trace_skipped;

#define PS2_CAPTURE_NAME_MAX 64
#define PS2_CAPTURE_ROOT_MAX 64

static char s_capture_name[PS2_CAPTURE_NAME_MAX];
static char s_capture_root[PS2_CAPTURE_ROOT_MAX];
static int s_capture_remaining;
static int s_capture_stride;
static int s_capture_wait;
static int s_capture_index;

static void capture_maybe_dump(const u8* indices, int osk);

void ps2_video_debug_print_phases(void)
{
    printf("[ps2] present phases x%u: upload=%u ms, queue=%u ms, exec=%u ms, flip=%u ms\n",
        (unsigned)s_phase_count, (unsigned)s_phase_ms[0], (unsigned)s_phase_ms[1],
        (unsigned)s_phase_ms[2], (unsigned)s_phase_ms[3]);
    s_phase_ms[0] = s_phase_ms[1] = s_phase_ms[2] = s_phase_ms[3] = 0;
    s_phase_count = 0;
}

void ps2_video_diagnostics(int enabled)
{
    s_palette_trace_on = enabled != 0;
    s_palette_trace_last_ms = 0;
    s_palette_trace_last_crc = 0;
    s_palette_trace_skipped = 0;
}

void ps2_video_palette_note(const char* what, int first, int count, unsigned int crc)
{
    unsigned int now;

    s_palette_generation++;

    if (!s_palette_trace_on) {
        return;
    }

    now = ps2_time_ms();

    /* A fade rewrites the palette every frame and a movie rewrites ranges
     * per frame. Keep the timeline readable: fold identical states and cap
     * the rate at ~50 lines/s, reporting how many updates were folded. */
    if (crc == s_palette_trace_last_crc && (now - s_palette_trace_last_ms) < 1000) {
        s_palette_trace_skipped++;
        return;
    }

    if (s_palette_trace_last_ms != 0 && (now - s_palette_trace_last_ms) < 20) {
        s_palette_trace_skipped++;
        return;
    }

    printf("[ps2pal] %u ms %s first=%d n=%d gen=%u crc=%08x skipped=%d\n",
        now, what != NULL ? what : "set", first, count, s_palette_generation,
        crc, s_palette_trace_skipped);

    s_palette_trace_last_ms = now;
    s_palette_trace_last_crc = crc;
    s_palette_trace_skipped = 0;
}

/* Pick the first writable capture root and create it if needed. */
static int capture_root_ensure(void)
{
    static const char* const candidates[] = {
        "host:/caps/",          /* PCSX2 host filesystem (development) */
        "mass:/FALLOUT2/caps/", /* USB stick / MX4SIO */
        "mc0:/FALLOUT2/caps/",  /* memory card */
    };

    if (s_capture_root[0] != '\0') {
        return 1;
    }

    for (int i = 0; i < (int)(sizeof(candidates) / sizeof(candidates[0])); i++) {
        char dir[PS2_CAPTURE_ROOT_MAX];
        char probe[PS2_CAPTURE_ROOT_MAX + 16];
        size_t len;
        FILE* f;

        snprintf(dir, sizeof(dir), "%s", candidates[i]);
        len = strlen(dir);
        if (len > 0 && dir[len - 1] == '/') {
            dir[len - 1] = '\0';
        }

        mkdir(dir, 0777);

        snprintf(probe, sizeof(probe), "%s/probe.tmp", dir);
        f = fopen(probe, "wb");
        if (f == NULL) {
            continue;
        }
        fclose(f);
        remove(probe);

        snprintf(s_capture_root, sizeof(s_capture_root), "%s/", dir);
        printf("[ps2pal] capture root: %s\n", s_capture_root);
        return 1;
    }

    printf("[ps2pal] capture: no writable root (host:, mass:, mc0:)\n");
    return 0;
}

static void capture_write(const char* suffix, const void* data, size_t size)
{
    char path[PS2_CAPTURE_ROOT_MAX + PS2_CAPTURE_NAME_MAX + 32];
    FILE* f;

    snprintf(path, sizeof(path), "%s%s_%d.%s", s_capture_root, s_capture_name, s_capture_index, suffix);
    f = fopen(path, "wb");
    if (f == NULL) {
        return;
    }
    if (data != NULL && size != 0) {
        fwrite(data, 1, size, f);
    }
    fclose(f);
}

static void capture_maybe_dump(const u8* indices, int osk)
{
    char meta[160];
    int length;

    if (s_capture_remaining <= 0 || indices == NULL) {
        return;
    }

    if (s_capture_wait > 0) {
        s_capture_wait--;
        return;
    }

    if (!capture_root_ensure()) {
        s_capture_remaining = 0;
        return;
    }

    capture_write("idx", indices, (size_t)PS2_SCR_W * PS2_SCR_H);
    capture_write("pal", s_palette, sizeof(s_palette));

    length = snprintf(meta, sizeof(meta),
        "name=%s index=%d osk=%d gen=%u vram=%08x\n",
        s_capture_name, s_capture_index, osk, s_palette_generation,
        (unsigned)s_texture.Vram);
    capture_write("txt", meta, (size_t)length);

    printf("[ps2pal] capture %s_%d (osk=%d gen=%u)\n",
        s_capture_name, s_capture_index, osk, s_palette_generation);

    s_capture_index++;
    s_capture_remaining--;
    s_capture_wait = s_capture_stride - 1;
}

int ps2_video_capture(const char* name, int count, int stride)
{
    /* Any request replaces (and therefore cancels) the pending sequence. */
    s_capture_remaining = 0;

    if (name == NULL || name[0] == '\0' || count <= 0) {
        return -1;
    }

    if (!s_palette_trace_on) {
        /* Captures are part of the opt-in diagnostics; a normal game session
         * must not write 300 KB files behind the player's back. */
        return -1;
    }

    snprintf(s_capture_name, sizeof(s_capture_name), "%s", name);
    s_capture_remaining = count;
    s_capture_stride = stride > 0 ? stride : 1;
    s_capture_wait = 0;
    s_capture_index = 0;
    return 0;
}