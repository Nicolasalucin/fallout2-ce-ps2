/*
 * PS2 save-slot redirection + PS2 browser metadata.
 *
 * See ps2_save.h for the design notes. The engine never learns about the
 * memory card: fs.c and the platform compat layer run every relative path
 * through ps2_save_remap() before resolving it, and compat_mkdir() asks for
 * the icon files to be materialized whenever a slot directory is created.
 *
 * The icon formats mirror real, browser-verified files:
 *   - icon.sys: 964-byte layout (magic, background gradient, light setup,
 *     68-byte Shift-JIS title at 0xC0, three icon filenames at 0x104)
 *   - icon.icn: "PS2 Icon Format v0.5" geometry (f16 vertices, one static
 *     animation frame, RLE-compressed uniform white texture) so the mesh
 *     shows only its amber vertex colors
 */

#include "ps2_save.h"

#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include <loadfile.h>
#include <sifrpc.h>

#include "ps2_fs.h"

#define PS2_SAVE_ROOT_MAX 80
#define PS2_SAVE_PATH_MAX 256

#define PS2_SAVE_ICN_VERTEX_SIZE 24
#define PS2_SAVE_ICN_VERTICES (12 * 3)

static int s_probed;
static char s_root[PS2_SAVE_ROOT_MAX];
static int s_is_mc;

/* ------------------------------------------------------------- helpers --- */

static void save_put_u16(unsigned char* p, int v)
{
    p[0] = (unsigned char)(v & 0xFF);
    p[1] = (unsigned char)((v >> 8) & 0xFF);
}

static void save_put_u32(unsigned char* p, unsigned int v)
{
    p[0] = (unsigned char)(v & 0xFF);
    p[1] = (unsigned char)((v >> 8) & 0xFF);
    p[2] = (unsigned char)((v >> 16) & 0xFF);
    p[3] = (unsigned char)((v >> 24) & 0xFF);
}

static void save_put_f32(unsigned char* p, float v)
{
    unsigned int u;
    memcpy(&u, &v, 4);
    save_put_u32(p, u);
}

#ifndef PS2_DEV_SMOKE_TEST
/* Only used by the device probe; smoke builds always take the host path. */
static int save_dir_exists(const char* dir)
{
    char tmp[PS2_SAVE_PATH_MAX];
    DIR* d;

    d = opendir(dir);
    if (d == NULL && snprintf(tmp, sizeof(tmp), "%s/", dir) < (int)sizeof(tmp)) {
        d = opendir(tmp);
    }
    if (d == NULL) {
        return 0;
    }

    closedir(d);
    return 1;
}
#endif

static void save_slot_dir(int slot, char* out, int out_size)
{
    if (s_is_mc) {
        snprintf(out, out_size, "%sFALLOUT2_S%02d", s_root, slot);
    } else {
        snprintf(out, out_size, "%sSLOT%02d", s_root, slot);
    }
}

/* Copies `path` into `out` collapsing the optional '/' right after the
 * device colon ("host:/assets" -> "host:assets"). fs.c's canonicalizer
 * rewrites "dev:dir" as "dev:/dir" while the root spellings keep "dev:dir",
 * so paths must be normalized before they are compared. */
static void save_normalize_device(const char* path, char* out, int out_size)
{
    int i = 0;

    while (*path != '\0' && i < out_size - 1) {
        char c = *path++;
        out[i++] = c;
        if (c == ':' && *path == '/') {
            path++;
        }
    }
    out[i] = '\0';
}

/* ---------------------------------------------------------- device init --- */

static void save_use_host(const char* why)
{
    const char* base;

    ps2_fs_init();
    base = ps2_fs_root_path(0);
    if (base == NULL) {
        base = "host:local_assets/";
    }

    snprintf(s_root, sizeof(s_root), "%ssaves/", base);
    s_is_mc = 0;

    /* Make sure the directory exists: path canonicalization walks every
     * intermediate component and would otherwise fail on the first save. */
    mkdir(s_root, 0755);

    printf("[ps2] save: %s -> host fallback '%s'\n", why, s_root);
}

void ps2_save_init(void)
{
    if (s_probed) {
        return;
    }
    s_probed = 1;

#ifdef PS2_DEV_SMOKE_TEST
    /* Dev/headless builds always save to the host so the files stay
     * inspectable from the build machine. */
    save_use_host("dev/smoke build");
#else
    /* The memory card fileio device needs MCMAN/MCSERV resident. The OSD
     * normally loads them, but a raw-ELF boot (PCSX2, a bare loader) starts
     * without them, exactly like the pad loads its own ROM modules. Failures
     * (already loaded, no card) are harmless. */
    SifInitRpc(0);
    SifLoadModule("rom0:SIO2MAN", 0, NULL);
    SifLoadModule("rom0:MCMAN", 0, NULL);
    SifLoadModule("rom0:MCSERV", 0, NULL);

    if (save_dir_exists("mc0:")) {
        snprintf(s_root, sizeof(s_root), "mc0:/");
        s_is_mc = 1;
        printf("[ps2] save: device mc0: (slots FALLOUT2_S01..FALLOUT2_S10)\n");
    } else if (save_dir_exists("mc1:")) {
        snprintf(s_root, sizeof(s_root), "mc1:/");
        s_is_mc = 1;
        printf("[ps2] save: device mc1: (slots FALLOUT2_S01..FALLOUT2_S10)\n");
    } else {
        save_use_host("no memory card");
    }
#endif
}

const char* ps2_save_root(void)
{
    ps2_save_init();
    return s_root;
}

/* --------------------------------------------------------------- remap --- */

int ps2_save_remap(const char* path, char* out, int out_size)
{
    const char* p;
    const char* rest;
    char dir[PS2_SAVE_PATH_MAX];
    int slot;
    int len;

    if (path == NULL || out == NULL || out_size <= 0) {
        return -1;
    }

    ps2_save_init();

    printf("[ps2] save_remap: in='%s'\n", path);

    p = path;
    if (strncasecmp(p, "data", 4) == 0 && (p[4] == '/' || p[4] == '\\')) {
        p += 5;
    }
    if (strncasecmp(p, "SAVEGAME", 8) != 0) {
        printf("[ps2] save_remap: no SAVEGAME prefix, p='%s'\n", p);
        return -1;
    }
    p += 8;

    if (*p == '\0') {
        /* The save root itself (the engine mkdir()s it before the slots). */
        snprintf(out, out_size, "%s", s_root);
        printf("[ps2] save_remap: root -> '%s'\n", out);
        return 0;
    }

    if (*p != '/' && *p != '\\') {
        printf("[ps2] save_remap: no separator after SAVEGAME, p='%s'\n", p);
        return -1;
    }
    p++;

    if (strncasecmp(p, "SLOT", 4) != 0
        || p[4] < '0' || p[4] > '9'
        || p[5] < '0' || p[5] > '9') {
        printf("[ps2] save_remap: bad SLOT, p='%s'\n", p);
        return -1;
    }
    slot = (p[4] - '0') * 10 + (p[5] - '0');
    if (slot < 1 || slot > PS2_SAVE_SLOT_COUNT) {
        printf("[ps2] save_remap: slot %d out of range\n", slot);
        return -1;
    }
    if (p[6] != '\0' && p[6] != '/' && p[6] != '\\') {
        printf("[ps2] save_remap: bad char after SLOT, p='%s'\n", p);
        return -1;
    }

    save_slot_dir(slot, dir, sizeof(dir));

    if (p[6] == '\0') {
        snprintf(out, out_size, "%s", dir);
        printf("[ps2] save_remap: slot dir -> '%s'\n", out);
        return 0;
    }

    len = snprintf(out, out_size, "%s/", dir);
    if (len < 0 || len >= out_size - 1) {
        return -1;
    }

    /* Append the remainder with normalized separators. */
    rest = p + 7;
    while (*rest != '\0') {
        if (len >= out_size - 1) {
            return -1;
        }
        out[len++] = (*rest == '\\') ? '/' : *rest;
        rest++;
    }
    out[len] = '\0';

    printf("[ps2] save_remap: full -> '%s'\n", out);
    return 0;
}

/* ----------------------------------------------------- browser metadata --- */

/* Builds the 964-byte icon.sys: standard layout with a Fallout-style amber
 * background gradient, the default light setup used by published saves and
 * the title "Fallout 2" / "- Slot NN" (line break before byte 9). */
static void save_build_icon_sys(unsigned char* b, int slot)
{
    /* 1/sqrt(3), the light direction used by real icon.sys files. */
    const float one3 = 0.57735026f;
    static const unsigned int corners[4][3] = {
        { 0x48, 0x30, 0x10 }, /* upper left */
        { 0x38, 0x24, 0x0C }, /* upper right */
        { 0x28, 0x1A, 0x08 }, /* lower left */
        { 0x18, 0x10, 0x04 }, /* lower right */
    };
    int i;

    memset(b, 0, PS2_SAVE_ICON_SYS_SIZE);

    memcpy(b, "PS2D", 4);
    save_put_u16(b + 0x04, 0);
    save_put_u16(b + 0x06, 9);     /* line 2 of the title starts here */
    save_put_u32(b + 0x0C, 0x60);  /* background transparency */

    for (i = 0; i < 4; i++) {
        save_put_u32(b + 0x10 + i * 0x10 + 0, corners[i][0]);
        save_put_u32(b + 0x10 + i * 0x10 + 4, corners[i][1]);
        save_put_u32(b + 0x10 + i * 0x10 + 8, corners[i][2]);
    }

    /* Light directions and colors + ambient light. */
    save_put_f32(b + 0x50, one3);
    save_put_f32(b + 0x54, one3);
    save_put_f32(b + 0x58, one3);
    save_put_f32(b + 0x60, one3);
    save_put_f32(b + 0x64, -one3);
    save_put_f32(b + 0x68, -one3);
    save_put_f32(b + 0x70, -one3);
    save_put_f32(b + 0x74, -one3);
    save_put_f32(b + 0x78, one3);
    for (i = 0x80; i <= 0xB0; i += 0x10) {
        save_put_f32(b + i + 0x0, 0.5f);
        save_put_f32(b + i + 0x4, 0.5f);
        save_put_f32(b + i + 0x8, 0.5f);
    }

    /* Title (ASCII is valid Shift-JIS). */
    memcpy(b + 0xC0, "Fallout 2", 9);
    snprintf((char*)b + 0xC0 + 9, 68 - 9, "- Slot %02d", slot);

    /* Normal / copy / delete icon filenames. */
    memcpy(b + 0x104, "icon.icn", 9);
    memcpy(b + 0x144, "icon.icn", 9);
    memcpy(b + 0x184, "icon.icn", 9);
}

static void save_build_icon_icn_vertex(unsigned char* p, const short* c, const short* n)
{
    /* Vertex coordinate (f16) + per-vertex lighting flag. */
    save_put_u16(p + 0x00, c[0]);
    save_put_u16(p + 0x02, c[1]);
    save_put_u16(p + 0x04, c[2]);
    save_put_u16(p + 0x06, 0x0400); /* same flag value as real icon files */

    /* Normal (f16) + flag. */
    save_put_u16(p + 0x08, n[0]);
    save_put_u16(p + 0x0A, n[1]);
    save_put_u16(p + 0x0C, n[2]);
    save_put_u16(p + 0x0E, 0x0400);

    /* Texture coordinates (unused by the uniform texture) + amber color. */
    save_put_u16(p + 0x10, 0);
    save_put_u16(p + 0x12, 0);
    p[0x14] = 0xFF;
    p[0x15] = 0xB0;
    p[0x16] = 0x40;
    p[0x17] = 0xFF;
}

/* Builds the icon.icn: a static amber cube (12 triangles, no indexing) with
 * a single-run RLE white texture so only the vertex colors show. */
static int save_build_icon_icn(unsigned char* b)
{
    static const short corners[8][3] = {
        { -2048, -2048, 2048 },  /* 0 front-bottom-left */
        { 2048, -2048, 2048 },   /* 1 front-bottom-right */
        { 2048, 2048, 2048 },    /* 2 front-top-right */
        { -2048, 2048, 2048 },   /* 3 front-top-left */
        { -2048, -2048, -2048 }, /* 4 back-bottom-left */
        { 2048, -2048, -2048 },  /* 5 back-bottom-right */
        { 2048, 2048, -2048 },   /* 6 back-top-right */
        { -2048, 2048, -2048 },  /* 7 back-top-left */
    };
    static const short normals[6][3] = {
        { 0, 0, 4096 },     /* +Z */
        { 0, 0, -4096 },    /* -Z */
        { -4096, 0, 0 },    /* -X */
        { 4096, 0, 0 },     /* +X */
        { 0, -4096, 0 },    /* -Y */
        { 0, 4096, 0 },     /* +Y */
    };
    static const struct {
        unsigned char v[3];
        unsigned char n;
    } faces[12] = {
        { { 0, 1, 2 }, 0 }, { { 0, 2, 3 }, 0 }, /* +Z */
        { { 5, 4, 7 }, 1 }, { { 5, 7, 6 }, 1 }, /* -Z */
        { { 4, 0, 3 }, 2 }, { { 4, 3, 7 }, 2 }, /* -X */
        { { 1, 5, 6 }, 3 }, { { 1, 6, 2 }, 3 }, /* +X */
        { { 4, 5, 1 }, 4 }, { { 4, 1, 0 }, 4 }, /* -Y */
        { { 3, 2, 6 }, 5 }, { { 3, 6, 7 }, 5 }, /* +Y */
    };
    unsigned char* v;
    unsigned char* anim;
    int f;
    int k;

    memset(b, 0, PS2_SAVE_ICON_ICN_SIZE);

    /* Icon header. */
    save_put_u32(b + 0x00, 0x00010000);     /* file id */
    save_put_u32(b + 0x04, 1);              /* animation shapes per vertex */
    save_put_u32(b + 0x08, 0x0E);           /* texture type: RLE compressed */
    save_put_u32(b + 0x0C, 0x3F800000);     /* reserved (1.0f) */
    save_put_u32(b + 0x10, PS2_SAVE_ICN_VERTICES);

    v = b + 0x14;
    for (f = 0; f < 12; f++) {
        for (k = 0; k < 3; k++) {
            save_build_icon_icn_vertex(v, corners[faces[f].v[k]], normals[faces[f].n]);
            v += PS2_SAVE_ICN_VERTEX_SIZE;
        }
    }

    /* Animation segment: one frame showing shape 0 (nothing animates). */
    anim = v;
    save_put_u32(anim + 0x00, 1);    /* id tag */
    save_put_u32(anim + 0x04, 1);    /* frame length */
    save_put_f32(anim + 0x08, 0.0f); /* animation speed */
    save_put_u32(anim + 0x0C, 0);    /* play offset */
    save_put_u32(anim + 0x10, 1);    /* number of frames */
    save_put_u32(anim + 0x14, 0);    /* frame 0: shape id */
    save_put_u32(anim + 0x18, 1);    /* frame 0: key count */
    save_put_f32(anim + 0x1C, 0.0f); /* key time */
    save_put_f32(anim + 0x20, 1.0f); /* key value */

    /* Texture segment: single-run RLE, uniform white. */
    save_put_u32(anim + 0x24, 4);      /* compressed data size */
    save_put_u16(anim + 0x28, 16384);  /* replicate 16384 texels */
    save_put_u16(anim + 0x2A, 0x7FFF); /* white (5-5-5) */

    return 0;
}

static void save_write_file(const char* path, const unsigned char* data, int size)
{
    FILE* f = fopen(path, "wb");

    if (f == NULL) {
        printf("[ps2] save: WARNING cannot write '%s'\n", path);
        return;
    }

    fwrite(data, 1, size, f);
    fclose(f);
}

static void save_write_meta(const char* dir, int slot)
{
    char path[PS2_SAVE_PATH_MAX];
    unsigned char sys[PS2_SAVE_ICON_SYS_SIZE];
    unsigned char icn[PS2_SAVE_ICON_ICN_SIZE];
    FILE* f;

    /* Already generated: repeated saves into the same slot must not rewrite
     * the icon files. */
    snprintf(path, sizeof(path), "%s/icon.sys", dir);
    f = fopen(path, "rb");
    if (f != NULL) {
        fclose(f);
        return;
    }

    if (save_build_icon_icn(icn) != 0) {
        return;
    }
    save_build_icon_sys(sys, slot);

    snprintf(path, sizeof(path), "%s/icon.sys", dir);
    save_write_file(path, sys, sizeof(sys));

    snprintf(path, sizeof(path), "%s/icon.icn", dir);
    save_write_file(path, icn, sizeof(icn));

    printf("[ps2] save: slot %02d browser metadata written in '%s'\n", slot, dir);
}

void ps2_save_slot_ensure_meta(const char* resolved_path)
{
    char normalized[PS2_SAVE_PATH_MAX];
    int slot;

    if (resolved_path == NULL) {
        return;
    }

    ps2_save_init();

    /* canonicalize() may rewrite "dev:dir" as "dev:/dir"; fold both forms. */
    save_normalize_device(resolved_path, normalized, sizeof(normalized));

    for (slot = 1; slot <= PS2_SAVE_SLOT_COUNT; slot++) {
        char dir[PS2_SAVE_PATH_MAX];
        char dir_normalized[PS2_SAVE_PATH_MAX];
        size_t n;
        char c;

        save_slot_dir(slot, dir, sizeof(dir));
        save_normalize_device(dir, dir_normalized, sizeof(dir_normalized));
        n = strlen(dir_normalized);
        if (strncasecmp(normalized, dir_normalized, n) != 0) {
            continue;
        }
        c = normalized[n];
        if (c != '\0' && c != '/') {
            continue;
        }

        save_write_meta(dir, slot);
        return;
    }
}

/* -------------------------------------------------------- dev helpers --- */

int ps2_save_slot_wipe(int slot)
{
    char dir[PS2_SAVE_PATH_MAX];
    char resolved[PS2_SAVE_PATH_MAX];
    DIR* d;
    struct dirent* e;
    int removed = 0;

    if (slot < 1 || slot > PS2_SAVE_SLOT_COUNT) {
        return -1;
    }

    ps2_save_init();

    save_slot_dir(slot, dir, sizeof(dir));

    /* The on-card spelling may differ in case; try to canonicalize first. */
    snprintf(resolved, sizeof(resolved), "%s", dir);
    if (ps2_fs_canonicalize(resolved, 0) != 0) {
        snprintf(resolved, sizeof(resolved), "%s", dir);
    }

    d = opendir(resolved);
    if (d == NULL) {
        return 0; /* nothing to wipe */
    }

    while ((e = readdir(d)) != NULL) {
        char path[PS2_SAVE_PATH_MAX];

        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) {
            continue;
        }
        if (snprintf(path, sizeof(path), "%s/%s", resolved, e->d_name) < (int)sizeof(path)) {
            /* ps2sdk's fileio.h documents fioRemove as broken: it falls
             * through into "mkdir", so each remove() would replace the file
             * with an empty directory. Drop that phantom right away. */
            remove(path);
            rmdir(path);
            removed++;
        }
    }
    closedir(d);

    remove(resolved);
    rmdir(resolved);

    /* The wiped names are gone now; a listing cached while resolving them
     * would still return them. */
    ps2_fs_invalidate_dir_cache();

    printf("[ps2] save: wiped slot %02d (%d files)\n", slot, removed);
    return 0;
}
