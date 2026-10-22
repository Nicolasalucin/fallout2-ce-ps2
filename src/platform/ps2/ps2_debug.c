/*
 * Crash-resistant telemetry for the Fallout 2 PS2 port.
 *
 * Design constraint: the physical console can freeze at any instruction, so
 * the only useful log is one that is already on the medium before the freeze.
 * While the console mirror is in use every PS2_LOG() opens the file in append
 * mode, writes one line, flushes and closes it immediately; nothing buffered
 * survives in RAM. Once ps2_init_debug_log() runs (USB stack up) stdout and
 * stderr become a single unbuffered writer over the same file, so plain
 * printf() calls from the engine are captured too.
 */

#include "ps2_debug.h"
#include "ps2_time.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <kernel.h>
#include <loadfile.h>
#include <sifrpc.h>

#define PS2_DEBUG_PATH_MAX 128

/* Maximum size of the debug log file on USB (4 MB). After this, the redirect
 * is disabled and we fall back to the per-line logger so the stick doesn't
 * fill up. */
#define PS2_DEBUG_LOG_MAX_SIZE (4 * 1024 * 1024)

/* Lines emitted before the USB stack is up (ps2_debug_init() runs first in
 * ps2_boot()) are kept here and replayed once a destination opens. */
#define PS2_DEBUG_EARLY_SIZE 8192
static char s_early[PS2_DEBUG_EARLY_SIZE];
static int s_early_len;

/* Crash mode flag: after an exception, PS2_LOG() stops touching files. */
static int s_crash_mode = 0;

static char s_path[PS2_DEBUG_PATH_MAX];
static int s_probed;
static int s_rpc_ready;
static int s_mc_prepared;

/* Set once ps2_init_debug_log() has pointed stdout/stderr at the mass: log.
 * From that point on the redirected stream is the only writer of s_path. */
static int s_redirected;

/* Last ps2_debug_sync() (throttled by PS2_DEBUG_SYNC_MS). */
static uint32_t s_last_sync_ms;

/* Approximate number of bytes written to the redirected log (seeded with the
 * existing file size once). Used to enforce PS2_DEBUG_LOG_MAX_SIZE without a
 * stat() syscall on every line. */
static unsigned int s_log_bytes;

/* Sticky 4 MB cap flag: once set, no destination is probed or reopened for
 * the rest of the session, so a full USB stick cannot be filled by the
 * per-line logger re-selecting the same file. Console mirror stays active. */
static int s_log_capped;

/* Console descriptors saved before stdout/stderr are dup2()'d onto the log
 * file. If the redirect is disabled (USB pulled or 4 MB cap), restoring them
 * lets plain printf() fall back to the TTY instead of a closed/full file. */
static int s_console_stdout = -1;
static int s_console_stderr = -1;

static const char* const s_candidates[] = {
    "mass:/FALLOUT2/debug.log",
    "mass:/debug.log",
    "mass0:/FALLOUT2/debug.log",
    "mass0:/debug.log",
    "mc0:/FALLOUT2/debug.log",
};
static const int s_candidate_count = (int)(sizeof(s_candidates) / sizeof(s_candidates[0]));

static int debug_file_writable(const char* path)
{
    FILE* f = fopen(path, "a");
    if (f == NULL) {
        return 0;
    }

    fclose(f);
    return 1;
}

/* Memory cards need the FALLOUT2 directory and, on a raw-ELF boot, the card
 * drivers themselves (uLaunchELF/OPL usually leave MCMAN/MCSERV resident). */
static void debug_prepare_mc0(void)
{
    DIR* dir;

    if (s_mc_prepared) {
        return;
    }
    s_mc_prepared = 1;

    dir = opendir("mc0:");
    if (dir == NULL) {
        SifLoadModule("rom0:MCMAN", 0, NULL);
        SifLoadModule("rom0:MCSERV", 0, NULL);
        dir = opendir("mc0:");
    }

    if (dir != NULL) {
        closedir(dir);
    }

    mkdir("mc0:/FALLOUT2", 0755);
}

static int debug_probe(void)
{
    int i;
    int over_cap = 0;

    if (s_log_capped) {
        return 0;
    }

    if (!s_rpc_ready) {
        sceSifInitRpc(0);
        s_rpc_ready = 1;
    }

    s_path[0] = '\0';

    for (i = 0; i < s_candidate_count; i++) {
        struct stat st;

        if (strncmp(s_candidates[i], "mc0:", 4) == 0) {
            debug_prepare_mc0();
        }

        if (!debug_file_writable(s_candidates[i])) {
            continue;
        }

        /* Never re-select a log that already reached the cap: otherwise the
         * per-line fallback would just keep appending to the same file. */
        if (stat(s_candidates[i], &st) == 0 && st.st_size > PS2_DEBUG_LOG_MAX_SIZE) {
            over_cap = 1;
            continue;
        }

        strncpy(s_path, s_candidates[i], sizeof(s_path) - 1);
        s_path[sizeof(s_path) - 1] = '\0';
        s_log_bytes = (stat(s_candidates[i], &st) == 0)
            ? (unsigned int)st.st_size : 0;
        return 1;
    }

    if (over_cap) {
        s_log_capped = 1;
    }

    return 0;
}

static int debug_write_line(uint32_t ms, const char* msg)
{
    FILE* f;
    unsigned int line_len;

    if (s_path[0] == '\0' || s_log_capped) {
        return 0;
    }

    line_len = (unsigned int)strlen(msg) + 32u;
    if (s_log_bytes + line_len > PS2_DEBUG_LOG_MAX_SIZE) {
        struct stat st;
        unsigned int size = s_log_bytes;

        if (stat(s_path, &st) == 0) {
            size = (unsigned int)st.st_size;
        }

        if (size > PS2_DEBUG_LOG_MAX_SIZE) {
            s_log_capped = 1;
            s_path[0] = '\0';
            return 0;
        }

        s_log_bytes = size;
    }

    f = fopen(s_path, "a");
    if (f == NULL) {
        return 0;
    }

    fprintf(f, "[ps2dbg] %8u ms | %s\n", (unsigned int)ms, msg);
    fflush(f);
    fclose(f);
    s_log_bytes += line_len;
    return 1;
}

static void debug_flush_early(void)
{
    FILE* f;
    unsigned int len = (unsigned int)s_early_len;

    if (len == 0 || s_path[0] == '\0' || s_log_capped) {
        return;
    }

    if (s_log_bytes + len > PS2_DEBUG_LOG_MAX_SIZE) {
        struct stat st;
        unsigned int size = s_log_bytes;

        if (stat(s_path, &st) == 0) {
            size = (unsigned int)st.st_size;
        }

        if (size + len > PS2_DEBUG_LOG_MAX_SIZE) {
            s_log_capped = 1;
            s_path[0] = '\0';
            s_early_len = 0;
            return;
        }

        s_log_bytes = size;
    }

    f = fopen(s_path, "a");
    if (f == NULL) {
        return;
    }

    fwrite(s_early, 1, (size_t)len, f);
    fflush(f);
    fclose(f);
    s_log_bytes += len;
    s_early_len = 0;
}

static void debug_buffer(uint32_t ms, const char* msg)
{
    int n;
    int room = (int)sizeof(s_early) - s_early_len;

    if (room <= 0) {
        return;
    }

    n = snprintf(s_early + s_early_len, (size_t)room, "[ps2dbg] %8u ms | %s\n", (unsigned int)ms, msg);
    if (n > 0 && n < room) {
        s_early_len += n;
    }
}

void ps2_debug_init(void)
{
    if (s_probed) {
        return;
    }
    s_probed = 1;

    if (debug_probe()) {
        debug_flush_early();
    }
}

void ps2_debug_reprobe(void)
{
    char previous[PS2_DEBUG_PATH_MAX];

    /* ps2_init_debug_log() already owns stdout/stderr and s_path; probing
     * again would only race the open log file. */
    if (s_redirected || s_log_capped) {
        return;
    }

    strncpy(previous, s_path, sizeof(previous) - 1);
    previous[sizeof(previous) - 1] = '\0';

    if (!debug_probe()) {
        return;
    }

    debug_flush_early();

    if (strcmp(previous, s_path) != 0) {
        ps2_debug_printf("[ps2] debug: log destination -> %s", s_path);
    }
}

static void debug_save_console(void)
{
    if (s_console_stdout < 0) {
        s_console_stdout = dup(STDOUT_FILENO);
    }
    if (s_console_stderr < 0) {
        s_console_stderr = dup(STDERR_FILENO);
    }
}

static void debug_restore_console(void)
{
    fflush(stdout);
    fflush(stderr);

    if (s_console_stdout >= 0) {
        dup2(s_console_stdout, STDOUT_FILENO);
        close(s_console_stdout);
        s_console_stdout = -1;
    }
    if (s_console_stderr >= 0) {
        dup2(s_console_stderr, STDERR_FILENO);
        close(s_console_stderr);
        s_console_stderr = -1;
    }
}

/* Opens path and points stdout/stderr at it. O_APPEND (no O_TRUNC) on
 * purpose: the per-line logger may have written the earliest boot lines to
 * this same file while the USB stack was still coming up; truncating here
 * would throw that boot record away. */
static int debug_redirect_open(const char* path)
{
    int fd;

    fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0) {
        return 0;
    }

    if (fd != STDOUT_FILENO && dup2(fd, STDOUT_FILENO) < 0) {
        close(fd);
        return 0;
    }
    if (fd != STDERR_FILENO && dup2(fd, STDERR_FILENO) < 0) {
        close(fd);
        return 0;
    }
    if (fd != STDOUT_FILENO && fd != STDERR_FILENO) {
        close(fd);
    }

    /* Unbuffered I/O: every printf() reaches the FAT32 file before the
     * console can freeze, so a black screen cannot swallow the last lines.
     * Best effort: if newlib refuses the mode change (the stream was used
     * before), ps2_debug_printf() still fflushes stdout per line. */
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    return 1;
}

static int debug_redirect_to(const char* path)
{
    /* Drain whatever the console mirror buffered before the dup2: those
     * lines belong to the TTY, not to the log file. */
    fflush(stdout);
    fflush(stderr);

    /* Keep a handle to the real console so the redirect can be undone when
     * the medium disappears or the log reaches its size cap. */
    debug_save_console();

    return debug_redirect_open(path);
}

int ps2_debug_sync(void)
{
    if (!s_redirected || s_path[0] == '\0') {
        return 0;
    }

    fflush(stdout);
    fflush(stderr);

    /* Both duplicated descriptors point at the same libcglue file entry; the
     * IOP close (and with it the vfat fs_close -> fat_updateSfn + sector
     * cache flush) only runs when the last reference goes away, so both have
     * to be closed. Reopening with O_APPEND picks up the now-correct size. */
    close(STDOUT_FILENO);
    close(STDERR_FILENO);

    if (!debug_redirect_open(s_path)) {
        /* Medium pulled or card removed while writing: fall back to the
         * per-line logger so the next PS2_LOG() can re-probe. Restore the
         * saved console handles so plain printf() never targets a closed
         * descriptor. */
        s_redirected = 0;
        debug_restore_console();
        return 0;
    }

    return 1;
}

void ps2_debug_tick(void)
{
    uint32_t now;

    if (!s_redirected) {
        return;
    }

    now = ps2_time_ms();
    if ((uint32_t)(now - s_last_sync_ms) < PS2_DEBUG_SYNC_MS) {
        return;
    }

    s_last_sync_ms = now;
    ps2_debug_sync();
}

void ps2_init_debug_log(void)
{
    static const char* const candidates[] = {
        "mass:/FALLOUT2/debug.log",
        "mass:/debug.log",
        "mass0:/FALLOUT2/debug.log",
        "mass0:/debug.log",
    };
    int candidate_count = (int)(sizeof(candidates) / sizeof(candidates[0]));
    int last_errno = 0;
    int capped = 0;
    int i;

    if (s_redirected || s_log_capped) {
        return;
    }

#ifdef PS2_DEV_SMOKE_TEST
    /* The smoke runner watches the PCSX2 EE console for its verdict; keep
     * the harness on the TTY even when a USB image is mounted. */
    printf("[ps2] debug: log redirect skipped (smoke harness keeps the TTY)\n");
    fflush(stdout);
    return;
#endif

    for (i = 0; i < candidate_count; i++) {
        struct stat st;

        /* Skip logs that already reached the 4 MB cap; write to the next
         * destination instead of growing a full stick. */
        if (stat(candidates[i], &st) == 0 && st.st_size > PS2_DEBUG_LOG_MAX_SIZE) {
            capped = 1;
            last_errno = EFBIG;
            continue;
        }

        if (!debug_redirect_to(candidates[i])) {
            last_errno = errno;
            continue;
        }

        s_redirected = 1;
        strncpy(s_path, candidates[i], sizeof(s_path) - 1);
        s_path[sizeof(s_path) - 1] = '\0';
        s_last_sync_ms = ps2_time_ms();

        {
            FILE* f = fopen(s_path, "a");

            if (f != NULL) {
                fseek(f, 0, SEEK_END);
                s_log_bytes = (unsigned int)ftell(f);
                fclose(f);
            } else {
                s_log_bytes = 0;
            }
        }

        /* Boot lines buffered while mass: was missing (no mc0 either) are
         * replayed into the just-opened file before anything else. */
        if (s_early_len > 0) {
            fwrite(s_early, 1, (size_t)s_early_len, stdout);
            s_early_len = 0;
        }

        printf("=== FALLOUT 2 CE PS2 - DEBUG LOG STARTED ===\n");
        fflush(stdout);
        PS2_LOG("[init] Hardware log redirect: OK ('%s')", s_path);
        return;
    }

    /* No mass: destination (hostfs run, memory-card-only boot or USB not
     * responding): keep the console mirror and the per-line logger. */
    if (capped) {
        s_log_capped = 1;
        printf("[init] Hardware log redirect: 4 MB cap reached, console-only\n");
        fflush(stdout);
        return;
    }

    printf("[init] Hardware log redirect: FAILED (errno=%d), keeping default TTY\n", last_errno);
    fflush(stdout);
}

const char* ps2_debug_path(void)
{
    return s_path;
}

void ps2_debug_printf(const char* fmt, ...)
{
    char msg[512];
    uint32_t ms;
    va_list ap;

    if (fmt == NULL) {
        return;
    }

    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    ms = ps2_time_ms();

    if (s_crash_mode) {
        /* After an exception, don't touch files - just mirror to console. */
        printf("[ps2dbg] %8u ms | %s\n", (unsigned int)ms, msg);
        fflush(stdout);
        return;
    }

    if (s_redirected) {
        /* stdout/stderr are the mass: log now and are unbuffered. One
         * writer only: reopening the file per line here would race the IOP
         * FAT driver's file position and could overwrite earlier lines. */
        printf("[ps2dbg] %8u ms | %s\n", (unsigned int)ms, msg);
        fflush(stdout);

        /* Check the approximate log size cap (4 MB). The actual file size is
         * only queried when the counter crosses the threshold, so the normal
         * path costs no extra I/O. */
        s_log_bytes += (unsigned int)strlen(msg) + 32u;
        if (s_log_bytes > PS2_DEBUG_LOG_MAX_SIZE) {
            struct stat st;
            unsigned int size = s_log_bytes;

            if (stat(s_path, &st) == 0) {
                size = (unsigned int)st.st_size;
            }

            if (size > PS2_DEBUG_LOG_MAX_SIZE) {
                s_redirected = 0;
                s_log_capped = 1;
                s_path[0] = '\0';
                debug_restore_console();
                printf("[ps2] debug: log size cap reached (%u bytes), console-only\n",
                       size);
                fflush(stdout);
                return;
            }

            /* Counter drifted (file was rotated/truncated): resync it. */
            s_log_bytes = size;
        }

        ps2_debug_tick();
        return;
    }

    /* Mirror to the console: the strict PCSX2 run and the hardware serial
     * adapter both benefit from seeing the same lines in order. */
    printf("[ps2dbg] %8u ms | %s\n", (unsigned int)ms, msg);
    fflush(stdout);

    if (s_path[0] == '\0') {
        /* Boot-time lines (before the USB stack exists) are replayed by
         * ps2_debug_reprobe() once ps2_storage_init() has run. Do not probe
         * on every line: loading MCMAN/MCSERV repeatedly during storage
         * bring-up would be pointless and slow. */
        debug_buffer(ms, msg);
        return;
    }

    if (!debug_write_line(ms, msg)) {
        /* Log cap reached: console mirror only, no probing/buffering. */
        if (s_log_capped) {
            return;
        }

        /* USB stick pulled, card removed, host: gone: re-probe and retry. */
        if (debug_probe()) {
            debug_flush_early();
            debug_write_line(ms, msg);
        } else {
            debug_buffer(ms, msg);
        }
    }
}

/* Enters crash mode: after this point, PS2_LOG() will not touch files,
 * only mirror to console. Called from the exception handler. */
void ps2_debug_enter_crash_mode(void)
{
    if (s_crash_mode) {
        return;
    }

    ps2_debug_flush_all();
    s_crash_mode = 1;
}

/* Flushes stdio buffers (best effort). Kept free of fs_close/reopen: this
 * runs from the exception vector, where the IOP may already be wedged. */
void ps2_debug_flush_all(void)
{
    fflush(stdout);
    fflush(stderr);
}
