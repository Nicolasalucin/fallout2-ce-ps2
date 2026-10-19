/*
 * Video backend for the Fallout PS2 port: GS hardware-indexed rendering.
 *
 * Pipeline per frame (fast path):
 *
 *   engine 8bpp 640x480 buffer (palette indices)
 *        |  gsKit_texture_upload (307 KB DMA to VRAM, writeback-flushed)
 *        v
 *   GS VRAM: T8 texture (TBW 10) + 256-entry CT32 CLUT
 *        |  full-screen textured quad, palette lookup in the GS
 *        v
 *   framebuffer flip on vsync -> NTSC 640x448 @ 60 Hz interlaced
 *
 * The GS does the palette lookup, so the EE neither converts 307K pixels per
 * frame nor moves the old 1.2 MB RGBA buffer over the GIF. GS_FILTER_NEAREST
 * handles the 480->448 downscale without resampling artifacts.
 *
 * The CLUT is 32-bit (CT32 entries in CSM2 storage) instead of the common
 * 16-bit CSM1 form: CSM2 keeps the engine palette's 8-bit channels intact and
 * never makes a palette slot transparent behind the engine's back.
 *
 * Fallback path: the OSK composites RGBA pixels, so while it is open the
 * frame is built with a CT32 texture (s_fb_rgba + s_texture32). Both are
 * created on demand and released when the keyboard closes, so normal play
 * never pays for them. s_palette doubles as the CPU lookup table there.
 */

#include "ps2_video.h"
#include "ps2_time.h"
#include "ps2_osk.h"
#include "sdl/ps2_sdl_internal.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

void ps2_video_debug_print_phases(void)
{
    printf("[ps2] present phases x%u: upload=%u ms, queue=%u ms, exec=%u ms, flip=%u ms\n",
        (unsigned)s_phase_count, (unsigned)s_phase_ms[0], (unsigned)s_phase_ms[1],
        (unsigned)s_phase_ms[2], (unsigned)s_phase_ms[3]);
    s_phase_ms[0] = s_phase_ms[1] = s_phase_ms[2] = s_phase_ms[3] = 0;
    s_phase_count = 0;
}

/* Palette LUT in GS CT32 style (0xAABBGGRR), rebuilt from the engine
 * palette by ps2_video_set_palette(). Used as the CPU lookup table. */
static u32 s_palette[256] __attribute__((aligned(64)));

/* Staging buffer used only if the engine ever presents a non-640x480 or
 * non-640-pitch surface (never happens today, kept as insurance).
 * 307 KB scratch for surfaces whose geometry does not match the screen.
 * Allocated on first use: every byte of static data shrinks the EE heap by
 * the same amount. */
static u8* s_staging;
static u8* s_staging_alloc;

/* Fast path: the 8-bit index frame is uploaded as a T8 (8-bit paletted)
 * texture and the GS performs the palette lookup through a 256-entry CT32
 * CLUT. The EE neither converts 307K pixels per frame nor moves 1.2 MB over
 * the GIF, and no RGBA frame buffer has to live in RAM. */
static GSTEXTURE s_texture;

/* On-screen keyboard path: the OSK composites RGBA pixels, so it needs a real
 * CT32 frame. Both the EE frame (1.2 MB) and its GS texture (1.2 MB of VRAM)
 * are created when the keyboard opens and released when it closes, which
 * keeps them out of normal play entirely. */
static GSTEXTURE s_texture32;
static u32* s_fb_rgba;
static u32* s_fb_rgba_alloc;

/* Lazily create the 64-byte aligned 8-bit staging buffer (one screen).
 * Returns it, or NULL when memory is exhausted. */
static u8* staging_ensure(void)
{
    const size_t stagingSize = (size_t)PS2_SCR_W * PS2_SCR_H;

    if (s_staging == NULL) {
        s_staging_alloc = (u8*)malloc(stagingSize + 64);
        if (s_staging_alloc == NULL) {
            return NULL;
        }
        s_staging = (u8*)(((uintptr_t)s_staging_alloc + 63u) & ~(uintptr_t)63u);
    }

    return s_staging;
}

/* Lazily create the RGBA frame and its GS texture for the OSK path.
 * Returns 1 when both are ready. */
static int video32_ensure(void)
{
    if (s_fb_rgba == NULL) {
        s_fb_rgba_alloc = (u32*)malloc((size_t)PS2_SCR_W * PS2_SCR_H * 4 + 64);
        if (s_fb_rgba_alloc == NULL) {
            return 0;
        }
        s_fb_rgba = (u32*)(((uintptr_t)s_fb_rgba_alloc + 63u) & ~(uintptr_t)63u);
    }

    if (s_texture32.Vram == 0) {
        s_texture32.Width = PS2_SCR_W;
        s_texture32.Height = PS2_SCR_H;
        s_texture32.PSM = GS_PSM_CT32;
        s_texture32.Filter = GS_FILTER_NEAREST;
        gsKit_setup_tbw(&s_texture32);
        s_texture32.Vram = gsKit_vram_alloc(s_gs,
            gsKit_texture_size(PS2_SCR_W, PS2_SCR_H, GS_PSM_CT32), GS_VRAM_BLOCKSIZE_256);
        if (s_texture32.Vram == 0) {
            free(s_fb_rgba_alloc);
            s_fb_rgba_alloc = NULL;
            s_fb_rgba = NULL;
            return 0;
        }
    }

    return 1;
}

/* The VRAM block is kept (gsKit has no per-block free and the OSK only opens
 * from menus); the 1.2 MB EE frame goes back to the engine heap. */
static void video32_release(void)
{
    free(s_fb_rgba_alloc);
    s_fb_rgba_alloc = NULL;
    s_fb_rgba = NULL;
}

void ps2_video_init(void)
{
    if (s_video_inited) {
        return;
    }
    s_video_inited = 1;

    dmaKit_init(D_CTRL_RELE_OFF, D_CTRL_MFD_OFF, D_CTRL_STS_UNSPEC,
        D_CTRL_STD_OFF, D_CTRL_RCYC_8, 1 << DMA_CHANNEL_GIF);
    dmaKit_chan_init(DMA_CHANNEL_GIF);

    /* PS2 port: gsKit's default render queues are 1 MB (oneshot) + 256 KB
     * (persistent) *per buffer*, i.e. ~2.5 MB of EE heap with double
     * buffering. This port draws a handful of primitives per frame (a clear
     * plus one screen quad; the OSK adds a few dozen), so small queues are
     * plenty and the freed heap goes to the engine. */
    s_gs = gsKit_init_global_custom(32 * 1024, 16 * 1024);

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
    s_texture.PSM = GS_PSM_T8;
    s_texture.ClutPSM = GS_PSM_CT32;
    s_texture.Clut = s_palette;
    /* CSM2: the CLUT in VRAM holds 32-bit entries, matching the CT32 palette
     * uploaded above. With CSM1 the GS reads the same bytes as 16-bit
     * entries; only palette slots that happen to be white survive and the
     * textured parts of the screen come out as colour noise. */
    s_texture.ClutStorageMode = GS_CLUT_STORAGE_CSM2;
    /* 480->448 downscale: NEAREST avoids resampling artifacts. */
    s_texture.Filter = GS_FILTER_NEAREST;
    gsKit_setup_tbw(&s_texture);
    s_texture.Vram = gsKit_vram_alloc(s_gs,
        gsKit_texture_size(PS2_SCR_W, PS2_SCR_H, GS_PSM_T8), GS_VRAM_BLOCKSIZE_256);
    s_texture.VramClut = gsKit_vram_alloc(s_gs,
        gsKit_texture_size(16, 16, GS_PSM_CT32), GS_VRAM_BLOCKSIZE_256);

    printf("[ps2] video: NTSC 640x448 60Hz, T8 tex @ %08x (TBW %u), CT32 CLUT @ %08x\n",
        (unsigned)s_texture.Vram, (unsigned)s_texture.TBW, (unsigned)s_texture.VramClut);
}

void ps2_video_shutdown(void)
{
    /* The engine exits right after this; the GS state is not reused. */
    s_video_inited = 0;

    video32_release();

    free(s_staging_alloc);
    s_staging_alloc = NULL;
    s_staging = NULL;
}

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
        const size_t stagingSize = (size_t)PS2_SCR_W * PS2_SCR_H;
        int rows = height < PS2_SCR_H ? height : PS2_SCR_H;
        int cols = width < PS2_SCR_W ? width : PS2_SCR_W;

        indices = staging_ensure();
        if (indices == NULL) {
            return;
        }

        memset(s_staging, 0, stagingSize);
        for (int y = 0; y < rows; y++) {
            memcpy(s_staging + (size_t)y * PS2_SCR_W,
                (const u8*)pixels + (size_t)y * pitch, (size_t)cols);
        }
    } else {
        indices = (const u8*)pixels;
    }

    {
        u32 t0 = ps2_time_ms();
        u32 t1, t2, t3, t4;
        GSTEXTURE* tex;
        int osk = ps2_osk_is_open();

        if (osk && video32_ensure()) {
            /* On-screen keyboard: the OSK blends real colors, so the frame is
             * built as RGBA on the EE and uploaded as CT32. */
            u32* dst = s_fb_rgba;
            const u32* lut = s_palette;
            for (int i = 0; i < PS2_SCR_W * PS2_SCR_H; i++) {
                dst[i] = lut[indices[i]];
            }

            ps2_osk_render(s_fb_rgba, PS2_SCR_W, PS2_SCR_H);

            s_texture32.Mem = (u32*)s_fb_rgba;
            tex = &s_texture32;
        } else {
            /* Normal path: upload the 8-bit indices and let the GS look the
             * palette up in its CLUT. No per-pixel work on the EE and 307 KB
             * per frame instead of 1.2 MB. */
            if (((uintptr_t)indices & 15u) != 0) {
                /* GIF DMA wants a 16-byte aligned source. The engine's screen
                 * is 64-byte aligned today, so this is insurance. */
                u8* staged = staging_ensure();
                if (staged == NULL) {
                    return;
                }
                memcpy(staged, indices, (size_t)PS2_SCR_W * PS2_SCR_H);
                indices = staged;
            }

            s_texture.Mem = (u32*)indices;
            s_texture.Clut = s_palette;
            tex = &s_texture;
        }

        if (!osk && s_fb_rgba != NULL) {
            /* Keyboard closed: hand its 1.2 MB frame back to the engine
             * heap (the transfer of the frame it was used for is done). */
            video32_release();
        }

        /* Write back the frame before the GIF DMAC fetches it (cached RAM;
         * gsKit also flushes inside gsKit_texture_upload()). */
        FlushCache(0);
        gsKit_texture_upload(s_gs, tex);
        t1 = ps2_time_ms();

        gsKit_clear(s_gs, GS_SETREG_RGBAQ(0, 0, 0, 0, 0));
        gsKit_prim_sprite_texture_3d(s_gs, tex,
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

        /* The engine's palette carries no usable alpha (the screen buffer is
         * an opaque software composite) and a CSM2 CLUT texel whose alpha is
         * below 0x80 is dropped by the GS, so every entry is written opaque.
         * 0x80 is also the GS's clamp for a 1.0 alpha. */
        s_palette[i] = (color & 0x00FFFFFF) | 0x80000000u;
    }
}

/* Boot self-test pattern helpers. The base pattern is only rebuilt when the
 * palette rotation changes (every 30 frames); per-frame work is limited to
 * repainting the two 8px strips used by the moving column. */
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
    /* Heap, not BSS: the self-test runs at most once per boot, and 300 KB of
     * static data would otherwise be subtracted from the engine heap for the
     * whole session (every byte of .bss shrinks the EE heap). */
    u8* pattern = (u8*)malloc((size_t)PS2_SCR_W * PS2_SCR_H);
    static u32 test_palette[256];
    int last_col = 0;
    int last_rotation = -1;
    int rotation;

    if (pattern == NULL) {
        printf("[ps2] selftest: skipped (out of memory)\n");
        return;
    }

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
     * layer is polled and its transitions are logged. Bring-up harness only
     * (PS2_DEV_BOOT_SELFTEST builds): normal boots go straight to the game. */
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

    /* The last probe uploaded from `pattern`; drop the source pointer before
     * releasing the scratch (the next present sets it again). */
    s_texture.Mem = NULL;
    free(pattern);
}
