/*
 * PS2 filesystem layer: device probing + case-insensitive path
 * resolution + block buffering for large datafiles.
 *
 * See ps2_fs.h for the design notes.
 */

#include "ps2_fs.h"

#include "ps2_save.h"
#include "ps2_storage.h"
#include "ps2_time.h"

#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <strings.h> /* strncasecmp */
#include <unistd.h>

#define PS2_FS_MAX_ROOTS 12
#define PS2_FS_ROOT_MAX 40
#define PS2_FS_PATH_MAX 256
#define PS2_FS_DAT_BUFSIZE (64 * 1024)

/* Fallback chain (after the ELF-relative roots, which ps2_fs_init()
 * prepends dynamically): mass: -> mass0: -> cdrom0: -> hdd0: -> host:. */
static const char* const s_candidates[] = {
    "mass:/FALLOUT2/",       /* USB stick / MX4SIO in a FALLOUT2 folder */
    "mass:/",                /* USB stick with the data at the root */
    "mass0:/FALLOUT2/",      /* numbered mass device */
    "cdrom0:/",              /* CD/DVD root */
    "cdrom0:/FALLOUT2/",     /* CD/DVD with the data in a FALLOUT2 folder */
    "hdd0:/FALLOUT2/",       /* internal HDD (PFS partition) */
    "host:local_assets/",    /* user layout: ELF and local_assets together */
    "host:../local_assets/", /* dev layout: ELF in build_ps2/ under the repo */
    "host:/",                /* PCSX2 host filesystem / ps2client ELF dir */
};

static char s_roots[PS2_FS_MAX_ROOTS][PS2_FS_ROOT_MAX];
static int s_root_count;
static int s_probed;

#ifdef PS2_DEV_SMOKE_TEST
/* Dev profiling: I/O call counters consumed by the smoke
 * harness. */
unsigned int ps2_fs_stat_fopen_calls;
unsigned int ps2_fs_stat_canonicalize_calls;
unsigned int ps2_fs_stat_dir_cache_hits;
unsigned int ps2_fs_stat_dir_scans;

void ps2_fs_stat_reset(void)
{
    ps2_fs_stat_fopen_calls = 0;
    ps2_fs_stat_canonicalize_calls = 0;
    ps2_fs_stat_dir_cache_hits = 0;
    ps2_fs_stat_dir_scans = 0;
}
#endif

/* opendir with a trailing-slash retry: device roots are accepted both as
 * "mass:" and "mass:/" depending on the driver. */
static DIR* fs_opendir(const char* dir)
{
    char tmp[PS2_FS_PATH_MAX];
    DIR* d;

    d = opendir(dir);
    if (d != NULL) {
        return d;
    }

    if (snprintf(tmp, sizeof(tmp), "%s/", dir) < (int)sizeof(tmp)) {
        d = opendir(tmp);
    }

    return d;
}

/* Copies `path` into `out` normalizing separators: backslashes to slashes,
 * leading "./" stripped. */
static void fs_normalize(const char* path, char* out, int out_size)
{
    int i = 0;

    while (*path == '.' && path[1] == '/') {
        path += 2;
    }

    while (*path != '\0' && i < out_size - 1) {
        char c = *path++;
        out[i++] = c == '\\' ? '/' : c;
    }

    out[i] = '\0';
}

/* A root is usable when both game datafiles resolve there: the engine needs
 * both, so accepting a root with only master.dat would just move the failure
 * deeper into the boot.
 *
 * The probe names are spelled the way the engine asks for them (lowercase)
 * and resolved through the readdir()-based canonicalizer, so the shipped case
 * does not matter: the Fallout 2 media uses lowercase names, the Fallout 1
 * media uppercase. Opening only a path that readdir() just confirmed also
 * keeps every failing open() away from the PCSX2 host-filesystem HLE. */
static void fs_validate_root(const char* root)
{
    static const char* const probes[] = { "master.dat", "critter.dat" };
    char probe[PS2_FS_PATH_MAX];
    int i;

    if (s_root_count >= PS2_FS_MAX_ROOTS) {
        return;
    }

    if (strlen(root) >= PS2_FS_ROOT_MAX) {
        printf("[ps2] fs: '%s' not available (root path too long)\n", root);
        return;
    }

    for (i = 0; i < (int)(sizeof(probes) / sizeof(probes[0])); i++) {
        int fd;

        if (snprintf(probe, sizeof(probe), "%s%s", root, probes[i]) >= (int)sizeof(probe)) {
            return;
        }

        if (ps2_fs_canonicalize(probe, 0) != 0) {
            printf("[ps2] fs: '%s' not available (%s)\n", root, probes[i]);
            return;
        }

        fd = open(probe, O_RDONLY);
        if (fd < 0) {
            printf("[ps2] fs: '%s' not available (%s unreadable)\n", root, probes[i]);
            return;
        }
        close(fd);
    }

    snprintf(s_roots[s_root_count], PS2_FS_ROOT_MAX, "%s", root);
    s_root_count++;
    printf("[ps2] fs: data root %d = '%s'\n", s_root_count, root);
}

void ps2_fs_init(void)
{
    const char* elf_dir;
    int i;

    if (s_probed) {
        return;
    }
    s_probed = 1;

    printf("[ps2] fs: probing game data devices...\n");

    /* the directory the ELF runs from comes first (media deployment),
     * then its local_assets/ subdirectory for the dev layouts. Both are
     * skipped when argv[0] carries no device path. */
    elf_dir = ps2_storage_elf_dir();
    if (elf_dir != NULL) {
        char elf_assets[PS2_FS_ROOT_MAX];

        fs_validate_root(elf_dir);
        if (snprintf(elf_assets, sizeof(elf_assets), "%slocal_assets/", elf_dir) < (int)sizeof(elf_assets)) {
            fs_validate_root(elf_assets);
        }
    }

    for (i = 0; i < (int)(sizeof(s_candidates) / sizeof(s_candidates[0])); i++) {
        fs_validate_root(s_candidates[i]);
    }

    if (s_root_count > 0) {
        printf("[ps2] storage: asset root detected at %s\n", s_roots[0]);
        return;
    }

    printf("[ps2] fs: WARNING no game data root found\n");

    /* Fail diagnostics: what does the PCSX2 host: root expose? */
    {
        DIR* d = opendir("host:");
        if (d == NULL) {
            d = opendir("host:/");
        }
        if (d == NULL) {
            printf("[ps2] fs: diag: opendir(host:) failed\n");
        } else {
            struct dirent* e;
            int n = 0;
            printf("[ps2] fs: diag: host: contains:");
            while ((e = readdir(d)) != NULL && n < 12) {
                printf(" [%s]", e->d_name);
                n++;
            }
            printf("\n");
            closedir(d);
        }
    }
}

int ps2_fs_root_count(void)
{
    ps2_fs_init();
    return s_root_count;
}

const char* ps2_fs_root_path(int index)
{
    if (index < 0 || index >= s_root_count) {
        return NULL;
    }
    return s_roots[index];
}

/* ---------------------------------------------------------------------------
 * Directory listing cache.
 *
 * Every path the engine resolves (art frames, message files, map data, save
 * slots, the 1000-entry patch%03d.dat probe) walks the path component by
 * component, and a component that is not found costs a full readdir() scan of
 * its parent directory. Over one boot that adds up to roughly 14.000
 * dopen()/readdir() RPC calls to the IOP: it makes the boot take ~10 s and on
 * PCSX2 it disturbs the host-filesystem HLE enough to break the EE's SIF RPC
 * bookkeeping (stray request completions, later seen as boot hangs/crashes).
 *
 * Keeping the last listings in EE memory removes that traffic: a resolution
 * costs no IOP call until the directory is written to (save files) or the
 * cached listing goes stale. Listings live in one fixed arena so that large
 * directories (data/ART/INTRFACE, the text dialog trees) are cached too; names
 * are stored compactly, and the total memory cost is bounded by the arena.
 */
#define PS2_FS_DIR_CACHE_SLOTS 32
#define PS2_FS_DIR_CACHE_TTL_MS 4000
#define PS2_FS_DIR_CACHE_ARENA (192 * 1024)
#define PS2_FS_DIR_CACHE_NAME_MAX 255

typedef struct {
    char path[PS2_FS_PATH_MAX];
    unsigned int stamp; /* ps2_time_ms() when the listing was taken */
    int valid;
    int count; /* entries in the listing */
    int offset; /* first entry, as an offset into the arena */
    int bytes; /* arena bytes used by the listing */
} fs_dir_cache_slot_t;

/* Listing entries sit one after another in the arena: one kind byte
 * (PS2_FS_KIND_*) followed by the NUL-terminated file name. */
static fs_dir_cache_slot_t s_dir_cache[PS2_FS_DIR_CACHE_SLOTS];
static unsigned char s_dir_arena[PS2_FS_DIR_CACHE_ARENA];
static int s_dir_arena_used;
static int s_dir_cache_next;

/* Call after anything changed a directory: new files (saves), removed files,
 * new mods. Cheap enough (a few flags) to call on every write open. The arena
 * is rewound as well: no directory iterator can be live across a file open
 * (canonicalization parses paths only). */
static void fs_dir_cache_invalidate(void)
{
    int i;

    for (i = 0; i < PS2_FS_DIR_CACHE_SLOTS; i++) {
        s_dir_cache[i].valid = 0;
    }

    s_dir_arena_used = 0;
}

static unsigned char fs_dirent_kind(const struct dirent* entry)
{
    if (entry->d_type == DT_DIR) {
        return PS2_FS_KIND_DIR;
    }
    if (entry->d_type == DT_UNKNOWN) {
        return PS2_FS_KIND_UNKNOWN;
    }
    return PS2_FS_KIND_FILE;
}

static fs_dir_cache_slot_t* fs_dir_cache_lookup(const char* dir)
{
    unsigned int now = ps2_time_ms();
    int i;

    for (i = 0; i < PS2_FS_DIR_CACHE_SLOTS; i++) {
        fs_dir_cache_slot_t* slot = &s_dir_cache[i];

        if (!slot->valid || strcmp(slot->path, dir) != 0) {
            continue;
        }

        if ((unsigned int)(now - slot->stamp) > PS2_FS_DIR_CACHE_TTL_MS) {
            slot->valid = 0;
            return NULL;
        }

        return slot;
    }

    return NULL;
}

/* Lists `dir` into the next round-robin slot. Returns NULL when the directory
 * cannot be listed or does not fit in the arena (the caller then iterates it
 * directly). */
static fs_dir_cache_slot_t* fs_dir_cache_fill(const char* dir)
{
    fs_dir_cache_slot_t* slot;
    struct dirent* entry;
    DIR* d;
    int start;
    int count = 0;

    /* Reclaim the arena in one go once it is mostly used: listings are only
     * re-read on demand, so dropping all of them is cheap. */
    if (s_dir_arena_used > PS2_FS_DIR_CACHE_ARENA * 3 / 4) {
        fs_dir_cache_invalidate();
    }

    d = fs_opendir(dir);
    if (d == NULL) {
        return NULL;
    }

    start = s_dir_arena_used;

    while ((entry = readdir(d)) != NULL) {
        size_t len = strlen(entry->d_name) + 1;

        if (len > PS2_FS_DIR_CACHE_NAME_MAX
            || s_dir_arena_used + 1 + (int)len > PS2_FS_DIR_CACHE_ARENA) {
            count = -1;
            break;
        }

        s_dir_arena[s_dir_arena_used++] = fs_dirent_kind(entry);
        memcpy(&s_dir_arena[s_dir_arena_used], entry->d_name, len);
        s_dir_arena_used += (int)len;
        count++;
    }

    closedir(d);

    if (count < 0) {
        s_dir_arena_used = start;
        return NULL;
    }

    slot = &s_dir_cache[s_dir_cache_next];
    s_dir_cache_next = (s_dir_cache_next + 1) % PS2_FS_DIR_CACHE_SLOTS;
    slot->valid = 0;
    snprintf(slot->path, sizeof(slot->path), "%s", dir);
    slot->count = count;
    slot->offset = start;
    slot->bytes = s_dir_arena_used - start;
    slot->stamp = ps2_time_ms();
    slot->valid = 1;

    return slot;
}

typedef struct {
    const fs_dir_cache_slot_t* slot; /* cached listing, when available */
    int index;
    int cursor; /* arena offset of the next entry (cached listings) */
    DIR* dir; /* direct iteration, when the directory is not cacheable */
} fs_dir_iter_t;

static int fs_dir_iter_begin(fs_dir_iter_t* it, const char* dir)
{
    const fs_dir_cache_slot_t* slot = fs_dir_cache_lookup(dir);

    it->slot = NULL;
    it->index = 0;
    it->cursor = 0;
    it->dir = NULL;

    if (slot == NULL) {
#ifdef PS2_DEV_SMOKE_TEST
        ps2_fs_stat_dir_scans++;
#endif
        slot = fs_dir_cache_fill(dir);
    } else {
#ifdef PS2_DEV_SMOKE_TEST
        ps2_fs_stat_dir_cache_hits++;
#endif
    }

    if (slot != NULL) {
        it->slot = slot;
        it->cursor = slot->offset;
        return 0;
    }

    it->dir = fs_opendir(dir);
    if (it->dir == NULL) {
        return -1;
    }

    return 0;
}

/* Returns the next entry name and stores its kind, or NULL at the end. */
static const char* fs_dir_iter_next(fs_dir_iter_t* it, unsigned char* kind)
{
    if (it->slot != NULL) {
        const char* name;

        if (it->index >= it->slot->count) {
            return NULL;
        }

        *kind = s_dir_arena[it->cursor];
        name = (const char*)&s_dir_arena[it->cursor + 1];
        it->cursor += 1 + (int)strlen(name) + 1;
        it->index++;

        return name;
    }

    {
        struct dirent* entry = readdir(it->dir);
        if (entry == NULL) {
            return NULL;
        }

        *kind = fs_dirent_kind(entry);
        return entry->d_name;
    }
}

static void fs_dir_iter_end(fs_dir_iter_t* it)
{
    if (it->dir != NULL) {
        closedir(it->dir);
        it->dir = NULL;
    }
}

static int fs_canonicalize_kind(char* path, int allow_missing_last, int* out_kind);

int ps2_fs_canonicalize(char* path, int allow_missing_last)
{
    return fs_canonicalize_kind(path, allow_missing_last, NULL);
}

/* Canonicalization that also reports the kind of the last component, taken
 * from its readdir() entry (d_type), so callers can tell files from
 * directories without opening anything. */
static int fs_canonicalize_kind(char* path, int allow_missing_last, int* out_kind)
{
#ifdef PS2_DEV_SMOKE_TEST
    ps2_fs_stat_canonicalize_calls++;
#endif
    char dir[PS2_FS_PATH_MAX];
    const char* colon;
    const char* p;
    size_t dir_len;

    if (out_kind != NULL) {
        *out_kind = PS2_FS_KIND_UNKNOWN;
    }

    colon = strchr(path, ':');
    if (colon == NULL) {
        return -1;
    }

    /* Mount point: "dev:" or "dev:/" depending on the original path. */
    p = colon + 1;
    if (*p == '/') {
        p++;
        dir_len = (size_t)(p - path);
    } else {
        dir_len = (size_t)(colon + 1 - path);
    }
    if (dir_len >= sizeof(dir)) {
        return -1;
    }
    memcpy(dir, path, dir_len);
    dir[dir_len] = '\0';

    while (*p != '\0') {
        const char* sep = strchr(p, '/');
        size_t comp_len = sep != NULL ? (size_t)(sep - p) : strlen(p);
        char match[PS2_FS_PATH_MAX];
        int found = 0;
        int last = sep == NULL;
        fs_dir_iter_t it;
        unsigned char kind = PS2_FS_KIND_UNKNOWN;
        const char* name;

        if (comp_len == 0) {
            p = sep + 1;
            continue;
        }

        if (comp_len == 1 && p[0] == '.') {
            p = last ? p + 1 : sep + 1;
            continue;
        }

        if (comp_len == 2 && p[0] == '.' && p[1] == '.') {
            /* Parent traversal: keep verbatim, no directory match. */
            if (dir_len + 4 > sizeof(dir)) {
                return -1;
            }
            if (dir[dir_len - 1] != '/') {
                dir[dir_len++] = '/';
            }
            dir[dir_len++] = '.';
            dir[dir_len++] = '.';
            dir[dir_len] = '\0';
            p = last ? p + comp_len : sep + 1;
            continue;
        }

        if (fs_dir_iter_begin(&it, dir) != 0) {
            return -1;
        }

        match[0] = '\0';
        while ((name = fs_dir_iter_next(&it, &kind)) != NULL) {
            if (strlen(name) == comp_len
                && strncasecmp(p, name, comp_len) == 0) {
                snprintf(match, sizeof(match), "%s", name);
                found = 1;
                break;
            }
        }
        fs_dir_iter_end(&it);

        /* Missing component: keep the requested spelling when the caller
         * allows creating the file, otherwise fail the resolution.
         *
         * Note: there is deliberately no direct ALL-CAPS probe open() here.
         * The readdir() scan above already resolves existing names
         * case-insensitively on every device type the port uses (FAT32,
         * ISO9660, host, PFS), so a blind uppercase open() could only add a
         * failed open for genuinely missing files -- and on PCSX2 a failing
         * open() corrupts the host filesystem HLE (SIF RPC state, the VM
         * crashes ~100 ms later). Bisection confirmed it. */
        if (!found) {
            if (!(last && allow_missing_last)) {
                return -1;
            }
            /* Keep the requested spelling for not-yet-created files. */
            memcpy(match, p, comp_len);
            match[comp_len] = '\0';
        } else if (last && out_kind != NULL) {
            *out_kind = kind;
        }

        if (dir_len + 1 + strlen(match) + 1 > sizeof(dir)) {
            return -1;
        }
        if (dir[dir_len - 1] != '/') {
            dir[dir_len++] = '/';
        }
        strcpy(dir + dir_len, match);
        dir_len += strlen(match);

        p = last ? p + comp_len : sep + 1;
    }

    if (dir_len + 1 > sizeof(dir)) {
        return -1;
    }
    strcpy(path, dir);

    return 0;
}

static FILE* fs_do_open(const char* path, const char* mode, int writable)
{
    FILE* f = fopen(path, mode);

    if (f != NULL && !writable) {
        size_t len = strlen(path);
        if (len > 4 && strcasecmp(path + len - 4, ".dat") == 0) {
            /* Block-read buffering for the big datafiles. */
            setvbuf(f, NULL, _IOFBF, PS2_FS_DAT_BUFSIZE);
        }
    }

    return f;
}

int ps2_fs_resolve_kind(const char* path, char* out, int out_size, int writable, int* out_kind)
{
    char norm[PS2_FS_PATH_MAX];
    char candidate[PS2_FS_PATH_MAX];
    int i;

    if (out_kind != NULL) {
        *out_kind = PS2_FS_KIND_UNKNOWN;
    }

    ps2_fs_init();

    if (out_size < PS2_FS_PATH_MAX) {
        return -1;
    }

    fs_normalize(path, norm, sizeof(norm));
    if (norm[0] == '\0') {
        return -1;
    }

    /* engine save slots (SAVEGAME\SLOTxx\...) are redirected to the save
     * device before any data-root probing. */
    {
        char remapped[PS2_FS_PATH_MAX];
        if (ps2_save_remap(norm, remapped, sizeof(remapped)) == 0) {
            if (fs_canonicalize_kind(remapped, writable, out_kind) != 0) {
                return -1;
            }
            strcpy(out, remapped);
            return 0;
        }
    }

    if (strchr(norm, ':') != NULL) {
        if (fs_canonicalize_kind(norm, writable, out_kind) != 0) {
            return -1;
        }
        strcpy(out, norm);
        return 0;
    }

    for (i = 0; i < s_root_count; i++) {
        snprintf(candidate, sizeof(candidate), "%s%s", s_roots[i], norm);
        if (fs_canonicalize_kind(candidate, writable, out_kind) != 0) {
            continue;
        }
        strcpy(out, candidate);
        return 0;
    }

    return -1;
}

int ps2_fs_resolve(const char* path, char* out, int out_size, int writable)
{
    return ps2_fs_resolve_kind(path, out, out_size, writable, NULL);
}

FILE* ps2_fs_fopen(const char* path, const char* mode)
{
    char norm[PS2_FS_PATH_MAX];
    char candidate[PS2_FS_PATH_MAX];
    int writable;
    int i;

    ps2_fs_init();

    if (path == NULL || mode == NULL) {
        return NULL;
    }

    fs_normalize(path, norm, sizeof(norm));
    if (norm[0] == '\0') {
        return NULL;
    }

    writable = strchr(mode, 'w') != NULL
        || strchr(mode, 'a') != NULL
        || strchr(mode, '+') != NULL;

    if (writable) {
        /* The engine is creating a file: a later directory scan must see it
         * (save slots, debug.log, mods). */
        fs_dir_cache_invalidate();
    }

#ifdef PS2_DEV_SMOKE_TEST
    ps2_fs_stat_fopen_calls++;
#endif

    /* save-slot files live on the save device, not in the data roots. */
    {
        char remapped[PS2_FS_PATH_MAX];
        if (ps2_save_remap(norm, remapped, sizeof(remapped)) == 0) {
            if (ps2_fs_canonicalize(remapped, writable) != 0) {
                return NULL;
            }
            return fs_do_open(remapped, mode, writable);
        }
    }

    if (strchr(norm, ':') != NULL) {
        if (ps2_fs_canonicalize(norm, writable) != 0) {
            return NULL;
        }
        return fs_do_open(norm, mode, writable);
    }

    for (i = 0; i < s_root_count; i++) {
        FILE* f;

        snprintf(candidate, sizeof(candidate), "%s%s", s_roots[i], norm);
        if (ps2_fs_canonicalize(candidate, writable) != 0) {
            continue;
        }

        f = fs_do_open(candidate, mode, writable);
        if (f != NULL) {
            return f;
        }
    }

    return NULL;
}

int ps2_fs_directory_exists(const char* path)
{
    char resolved[PS2_FS_PATH_MAX];
    int kind = PS2_FS_KIND_UNKNOWN;
    DIR* dir;

    if (ps2_fs_resolve_kind(path, resolved, sizeof(resolved), 0, &kind) != 0) {
        return 0;
    }

    /* Resolution accepts files and directories alike. The kind comes from
     * the readdir() entry type, so a file is never handed to opendir(); on
     * PCSX2 a failed dopen() (which is what opendir() of a file produces)
     * corrupts the host filesystem HLE. */
    if (kind != PS2_FS_KIND_UNKNOWN) {
        return kind == PS2_FS_KIND_DIR;
    }

    /* Driver without entry types: fall back to the opendir() probe. */
    dir = opendir(resolved);
    if (dir == NULL) {
        return 0;
    }

    closedir(dir);
    return 1;
}

int ps2_fs_exists(const char* path)
{
    char resolved[PS2_FS_PATH_MAX];

    /* Resolution never opens the target; on success the file is known to
     * exist. Used by compat_access(F_OK), which the engine calls in tight
     * loops (the patch-file scan in gameDbInit is 1000 probes), so the cost
     * here is one readdir() pass per path component and no failed open(). */
    return ps2_fs_resolve(path, resolved, sizeof(resolved), 0) == 0;
}
