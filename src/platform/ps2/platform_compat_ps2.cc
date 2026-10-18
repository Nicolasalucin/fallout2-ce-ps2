/*
 * PlayStation 2 implementation of the engine's platform compatibility layer.
 *
 * This file replaces src/platform_compat.cc in the PS2 build (the upstream
 * file is excluded by CMake while PS2=TRUE). It keeps the exact same API and
 * semantics, but resolves engine paths against the PS2 storage devices
 * through src/platform/ps2/fs.c instead of a POSIX working directory.
 *
 * Differences to the upstream POSIX implementation:
 *   - Every path goes through fs_normalize() semantics (backslashes, leading
 *     "./") and case-insensitive, component-wise resolution against the
 *     probed data roots.
 *   - compat_mkdir()/compat_remove()/compat_rename() operate on resolved
 *     device paths, because there is no process working directory to rely on.
 *   - compat_timeGetTime() uses the EE system timer (ps2_time.c) instead of
 *     std::chrono, which the EE libstdc++ does not back with a clock.
 */

#include "platform_compat.h"

#include <string.h>

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <SDL.h>

#include "combat.h"
#include "ps2_fs.h"
#include "ps2_save.h"
#include "ps2_time.h"

namespace fallout {

int compat_stricmp(const char* string1, const char* string2)
{
    return SDL_strcasecmp(string1, string2);
}

int compat_strnicmp(const char* string1, const char* string2, size_t size)
{
    return SDL_strncasecmp(string1, string2, size);
}

char* compat_strupr(char* string)
{
    return SDL_strupr(string);
}

char* compat_strlwr(char* string)
{
    return SDL_strlwr(string);
}

char* compat_itoa(int value, char* buffer, int radix)
{
    return SDL_itoa(value, buffer, radix);
}

void compat_splitpath(const char* path, char* drive, char* dir, char* fname, char* ext)
{
    const char* driveStart = path;
    if (path[0] == '/' && path[1] == '/') {
        path += 2;
        while (*path != '\0' && *path != '/' && *path != '.') {
            path++;
        }
    }

    if (drive != nullptr) {
        size_t driveSize = path - driveStart;
        if (driveSize > COMPAT_MAX_DRIVE - 1) {
            driveSize = COMPAT_MAX_DRIVE - 1;
        }
        strncpy(drive, path, driveSize);
        drive[driveSize] = '\0';
    }

    const char* dirStart = path;
    const char* fnameStart = path;
    const char* extStart = nullptr;

    const char* end = path;
    while (*end != '\0') {
        if (*end == '/') {
            fnameStart = end + 1;
        } else if (*end == '.') {
            extStart = end;
        }
        end++;
    }

    if (extStart == nullptr) {
        extStart = end;
    }

    if (dir != nullptr) {
        size_t dirSize = fnameStart - dirStart;
        if (dirSize > COMPAT_MAX_DIR - 1) {
            dirSize = COMPAT_MAX_DIR - 1;
        }
        strncpy(dir, path, dirSize);
        dir[dirSize] = '\0';
    }

    if (fname != nullptr) {
        size_t fileNameSize = extStart - fnameStart;
        if (fileNameSize > COMPAT_MAX_FNAME - 1) {
            fileNameSize = COMPAT_MAX_FNAME - 1;
        }
        strncpy(fname, fnameStart, fileNameSize);
        fname[fileNameSize] = '\0';
    }

    if (ext != nullptr) {
        size_t extSize = end - extStart;
        if (extSize > COMPAT_MAX_EXT - 1) {
            extSize = COMPAT_MAX_EXT - 1;
        }
        strncpy(ext, extStart, extSize);
        ext[extSize] = '\0';
    }
}

void compat_makepath(char* path, const char* drive, const char* dir, const char* fname, const char* ext)
{
    path[0] = '\0';

    if (drive != nullptr) {
        if (*drive != '\0') {
            strcpy(path, drive);
            path = strchr(path, '\0');

            if (path[-1] == '/') {
                path--;
            } else {
                *path = '/';
            }
        }
    }

    if (dir != nullptr) {
        if (*dir != '\0') {
            if (*dir != '/' && *path == '/') {
                path++;
            }

            strcpy(path, dir);
            path = strchr(path, '\0');

            if (path[-1] == '/') {
                path--;
            } else {
                *path = '/';
            }
        }
    }

    if (fname != nullptr && *fname != '\0') {
        if (*fname != '/' && *path == '/') {
            path++;
        }

        strcpy(path, fname);
        path = strchr(path, '\0');
    } else {
        if (*path == '/') {
            path++;
        }
    }

    if (ext != nullptr) {
        if (*ext != '\0') {
            if (*ext != '.') {
                *path++ = '.';
            }

            strcpy(path, ext);
            path = strchr(path, '\0');
        }
    }

    *path = '\0';
}

long compat_tell(int fileHandle)
{
    return lseek(fileHandle, 0, SEEK_CUR);
}

long compat_filelength(int fd)
{
    long originalOffset = lseek(fd, 0, SEEK_CUR);
    lseek(fd, 0, SEEK_SET);
    long filesize = lseek(fd, 0, SEEK_END);
    lseek(fd, originalOffset, SEEK_SET);
    return filesize;
}

int compat_mkdir(const char* path)
{
    char resolved[COMPAT_MAX_PATH];
    if (ps2_fs_resolve(path, resolved, sizeof(resolved), 1) != 0) {
        return -1;
    }

    int rc = mkdir(resolved, 0755);

    /* Creating a save-slot directory materializes the PS2 browser metadata
     * (icon.sys + icon.icn) inside it. */
    ps2_save_slot_ensure_meta(resolved);

    /* The parent listing was cached while resolving [path], before the
     * directory existed. */
    ps2_fs_invalidate_parent_of(resolved);

    return rc;
}

unsigned int compat_timeGetTime(void)
{
    /* EE system timer in milliseconds (ps2_time.c). */
    return (unsigned int)ps2_time_ms();
}

FILE* compat_fopen(const char* path, const char* mode)
{
    return ps2_fs_fopen(path, mode);
}

/* True for a resolved memory-card path ("mc0:/..." / "mc1:/..."). */
static bool is_mc_path(const char* path)
{
    return SDL_strncasecmp(path, "mc0:", 4) == 0 || SDL_strncasecmp(path, "mc1:", 4) == 0;
}

gzFile compat_gzopen(const char* path, const char* mode)
{
    char resolved[COMPAT_MAX_PATH];
    int writable = strchr(mode, 'w') != nullptr
        || strchr(mode, 'a') != nullptr
        || strchr(mode, '+') != nullptr;

    if (ps2_fs_resolve(path, resolved, sizeof(resolved), writable) != 0) {
        return nullptr;
    }

    gzFile stream = gzopen(resolved, mode);

    /* Same transient memory-card window as in the C stdio layer (fs.c):
     * retry before giving up. The path was just resolved through a
     * readdir() listing, so a missing file is not the cause. */
    if (stream == nullptr && is_mc_path(resolved)) {
        static const unsigned int waits_ms[] = { 25, 50, 100, 200 };

        for (size_t i = 0; stream == nullptr && i < sizeof(waits_ms) / sizeof(waits_ms[0]); i++) {
            unsigned int start = ps2_time_ms();
            while ((unsigned int)(ps2_time_ms() - start) < waits_ms[i]) {
            }
            printf("[ps2] fs: retrying gzopen %s (attempt %u)\n", resolved, (unsigned int)(i + 2));
            stream = gzopen(resolved, mode);
        }

        if (stream == nullptr) {
            printf("[ps2] fs: gzopen failed after retries: %s (%s)\n", resolved, mode);
        }
    }

    return stream;
}

char* compat_fgets(char* buffer, int maxCount, FILE* stream)
{
    buffer = fgets(buffer, maxCount, stream);

    if (buffer != nullptr) {
        size_t len = strlen(buffer);
        if (len >= 2 && buffer[len - 1] == '\n' && buffer[len - 2] == '\r') {
            buffer[len - 2] = '\n';
            buffer[len - 1] = '\0';
        }
    }

    return buffer;
}

char* compat_gzgets(gzFile stream, char* buffer, int maxCount)
{
    buffer = gzgets(stream, buffer, maxCount);

    if (buffer != nullptr) {
        size_t len = strlen(buffer);
        if (len >= 2 && buffer[len - 1] == '\n' && buffer[len - 2] == '\r') {
            buffer[len - 2] = '\n';
            buffer[len - 1] = '\0';
        }
    }

    return buffer;
}

int compat_remove(const char* path)
{
    char resolved[COMPAT_MAX_PATH];
    if (ps2_fs_resolve(path, resolved, sizeof(resolved), 0) != 0) {
        return -1;
    }

    /* ps2sdk's fileio.h documents fioRemove as broken: its RPC dispatch
     * lacks a break and falls through into "mkdir", so every successful
     * remove() leaves an empty directory with the same name. Remove that
     * phantom as well; rmdir() fails harmlessly when the path is gone. */
    int rc = remove(resolved);
    rmdir(resolved);

    /* The parent listing still names the removed file. */
    ps2_fs_invalidate_parent_of(resolved);

    return rc;
}

/* Copy fallback for compat_rename().
 *
 * ps2sdk's fileio backend registers no rename() handler, so the C library
 * fails the call locally with ENOSYS before issuing any RPC (no rename ever
 * reaches the emulator log). Copy the bytes and delete the source instead. */
static int compat_rename_copy(const char* resolvedOld, const char* resolvedNew)
{
    /* 16 KiB static buffer: large enough to amortize the SIF RPC round
     * trips, small enough to keep off the EE main-thread stack. */
    static char buffer[16 * 1024];

    FILE* source = fopen(resolvedOld, "rb");
    if (source == nullptr) {
        printf("[ps2] fs: rename fallback: cannot open source %s\n", resolvedOld);
        return -1;
    }

    FILE* dest = fopen(resolvedNew, "wb");
    if (dest == nullptr) {
        fclose(source);
        printf("[ps2] fs: rename fallback: cannot create %s\n", resolvedNew);
        return -1;
    }

    int rc = 0;
    for (;;) {
        size_t readCount = fread(buffer, 1, sizeof(buffer), source);
        if (readCount > 0 && fwrite(buffer, 1, readCount, dest) != readCount) {
            rc = -1;
            break;
        }
        if (readCount < sizeof(buffer)) {
            if (ferror(source)) {
                rc = -1;
            }
            break;
        }
    }

    /* Close the destination before touching the source: the delete below
     * must not race the last sector flush. */
    if (fclose(dest) != 0) {
        rc = -1;
    }
    fclose(source);

    if (rc == 0) {
        /* compat_remove() also clears the phantom directory the broken
         * fioRemove leaves behind and drops the parent listing. */
        rc = compat_remove(resolvedOld);
        if (rc != 0) {
            /* Source still in place: the rename did not happen, so drop the
             * copy to keep the tree all-or-nothing. */
            remove(resolvedNew);
            rmdir(resolvedNew);
        }
    } else {
        /* Do not leave a half-written copy behind. */
        remove(resolvedNew);
        rmdir(resolvedNew);
    }

    if (rc != 0) {
        printf("[ps2] fs: rename fallback failed: %s -> %s\n", resolvedOld, resolvedNew);
    }

    return rc;
}

int compat_rename(const char* oldFileName, const char* newFileName)
{
    char resolvedOld[COMPAT_MAX_PATH];
    if (ps2_fs_resolve(oldFileName, resolvedOld, sizeof(resolvedOld), 0) != 0) {
        return -1;
    }

    char resolvedNew[COMPAT_MAX_PATH];
    if (ps2_fs_resolve(newFileName, resolvedNew, sizeof(resolvedNew), 1) != 0) {
        return -1;
    }

    /* Renaming a file onto itself is a POSIX no-op, and the copy fallback
     * must not touch the file in that case. */
    if (strcmp(resolvedOld, resolvedNew) == 0) {
        return 0;
    }

    int rc = rename(resolvedOld, resolvedNew);

    if (rc != 0) {
        rc = compat_rename_copy(resolvedOld, resolvedNew);
    }

    if (rc == 0) {
        /* Both parent listings changed. */
        ps2_fs_invalidate_parent_of(resolvedOld);
        ps2_fs_invalidate_parent_of(resolvedNew);
    }

    return rc;
}

void compat_windows_path_to_native(char* path)
{
    char* pch = path;
    while (*pch != '\0') {
        if (*pch == '\\') {
            *pch = '/';
        }
        pch++;
    }
}

void compat_resolve_path(char* path)
{
    /* Intentionally does nothing on PS2.
     *
     * The upstream POSIX implementation fixes the spelling (case) of each
     * component in place, which keeps the string length unchanged - callers
     * like gameConfigResolvePath() pass buffers whose size is exactly the
     * length of the path (config values are allocated with internal_strdup),
     * so writing anything longer here trashes the heap.
     *
     * A root-prefixed rewrite is not needed either: every engine file
     * operation goes through ps2_fs_fopen()/compat_fopen(), which resolves
     * relative paths against the probed data roots (and redirects save slots
     * to the memory card). The only callers that need a resolved path are the
     * opendir()-based directory scans; file_find.cc resolves those itself
     * through ps2_fs_resolve(). */
}

int compat_access(const char* path, int mode)
{
    /* F_OK probes only ask for existence. Answering them without opening
     * anything keeps the engine's 1000-entry patch-file scan
     * (gameDbInit -> "patch%03d.dat") free of failed open() calls, which
     * are what upsets the PCSX2 host-filesystem HLE. */
    if (mode == F_OK) {
        return ps2_fs_exists(path) ? 0 : -1;
    }

    char resolved[COMPAT_MAX_PATH];
    if (ps2_fs_resolve(path, resolved, sizeof(resolved), (mode & W_OK) != 0) != 0) {
        return -1;
    }

    return access(resolved, mode);
}

char* compat_strdup(const char* string)
{
    return SDL_strdup(string);
}

// It's a replacement for compat_filelength(fileno(stream)) on platforms without
// fileno defined.
long getFileSize(FILE* stream)
{
    long originalOffset = ftell(stream);
    fseek(stream, 0, SEEK_END);
    long filesize = ftell(stream);
    fseek(stream, originalOffset, SEEK_SET);
    return filesize;
}

// Bridge for the context-sensitive R1 mapping (input_pad.c decides between
// attack and end-combat attempt). C linkage so the plain-C platform layer can
// call it; reads a global, safe before boot.
extern "C" int ps2_platform_in_combat(void)
{
    return isInCombat() ? 1 : 0;
}

} // namespace fallout
