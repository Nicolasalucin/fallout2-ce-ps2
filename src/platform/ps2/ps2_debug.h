#ifndef PS2_DEBUG_H_
#define PS2_DEBUG_H_

/*
 * Crash-resistant telemetry for the Fallout 2 PS2 port.
 *
 * Every PS2_LOG() line is written to a real file with open/append/flush/close
 * per call, so the very last line survives a console freeze, a black screen
 * or an emulator hang. Destinations are probed in this order:
 *
 *   mass:/FALLOUT2/debug.log   (USB/MX4SIO - the normal hardware target)
 *   mc0:/FALLOUT2/debug.log    (memory card fallback)
 *
 * The memory card directory is created on demand (and MCMAN/MCSERV are
 * loaded if a raw-ELF boot left them out). Lines emitted before mass: is
 * mounted (USB driver bring-up happens later) are buffered in RAM and
 * flushed as soon as a destination becomes writable.
 *
 * ps2_init_debug_log() additionally points stdout/stderr at the mass: log
 * once the USB stack is up, so every plain printf() from the platform and
 * from the engine lands in the same file, not only PS2_LOG lines. Call it
 * only after ps2_storage_init(): opening mass: before usbd/usbmass have been
 * loaded fails with ENODEV.
 *
 * IMPORTANT (usbhdfsd write-back cache): a plain fflush() on the redirected
 * stdio does NOT put the bytes on the stick. The ps2sdk vfat driver keeps
 * dirty sectors in a 128 KiB RAM cache and only writes the directory-entry
 * size in fs_close(); after a freeze the file would look empty (0 bytes) in
 * a PC card reader even though the data is in cached/unlinked clusters.
 * ps2_debug_sync() therefore closes both stdio descriptors (which drops the
 * last reference in libcglue and runs fs_close() -> fat_updateSfn() plus
 * scache_flushSectors()) and reopens the file with O_APPEND, so the log is
 * really on the medium. ps2_debug_tick() applies the same sync at most once
 * every PS2_DEBUG_SYNC_MS and is cheap enough to call from the frame loop
 * and from every PS2_LOG line; a crash loses at most the last few seconds.
 *
 * PCSX2 note: there is deliberately no "host:" fallback. PCSX2's hostfs HLE
 * can wedge on the open/append/close-per-line pattern under strict emulation;
 * under the emulator the console mirror (captured by tools/run_smoke.sh into
 * build_ps2/pcsx2_strict.log) carries the same lines. When PCSX2 is
 * configured with a USB mass-storage image, mass: is a real device and the
 * redirect works there too.
 *
 * Caveat: the engine's own debug mode (debug.mode=log) opens debug.log in
 * the data root with "wt". On a USB install that resolves to this very file,
 * so do not enable both at once; the engine would truncate the platform log
 * and the two writers would fight over the FAT chain.
 */

#ifdef __cplusplus
extern "C" {
#endif

/* Probes the destination chain. Safe to call before ps2_storage_init();
 * early lines are buffered until a device is available. */
void ps2_debug_init(void);

/* Re-probes the chain, preferring mass: again. Call after the storage
 * drivers have been brought up and the VFS roots are known. No-op once
 * ps2_init_debug_log() has taken over stdout/stderr. */
void ps2_debug_reprobe(void);

/* Redirects stdout and stderr to the mass: debug log and switches both to
 * unbuffered mode (setvbuf(_IONBF)). Must run only after ps2_storage_init()
 * has loaded the USB driver and confirmed that mass: responds; before that
 * the open fails with ENODEV. Tries mass:/FALLOUT2/debug.log first and
 * mass:/debug.log second, so a FAT32 stick with the data at the root still
 * logs. When no mass: destination opens, the console mirror and the
 * per-line logger stay in place. */
void ps2_init_debug_log(void);

/* Writes one line (printf format) to the current destination. Before the
 * redirect each call does fopen("a") + vfprintf + fflush + fclose and mirrors
 * the line to stdout; once ps2_init_debug_log() is active stdout/stderr are
 * the log file itself, so the line is just printf() + fflush() + the
 * throttled ps2_debug_tick(). */
void ps2_debug_printf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

/* Forces the redirected log onto the medium: closes stdout/stderr (running
 * the FAT driver's close path, which writes the file size and flushes its
 * sector cache) and reopens the log in append mode. Call at critical events
 * (before level loads, around video/audio bring-up, on fatal paths). Returns
 * 1 while the log is still redirected, 0 when there is nothing to sync or
 * the medium disappeared. No-op before ps2_init_debug_log(). */
int ps2_debug_sync(void);

/* Rate-limited ps2_debug_sync(): cheap enough to call on every frame; the
 * medium is only touched once every PS2_DEBUG_SYNC_MS. */
void ps2_debug_tick(void);

/* Minimum interval between two automatic syncs. A black screen therefore
 * loses at most this many seconds of post-redirect output. */
#define PS2_DEBUG_SYNC_MS 5000

/* Current log path, or "" when nothing is writable yet. */
const char* ps2_debug_path(void);

#define PS2_LOG(...) ps2_debug_printf(__VA_ARGS__)

/* PS2_LOGV() lives in ps2_log.h: a single verbosity gate shared by every
 * PS2 translation unit (runtime toggle in debug builds, compiled out in
 * Release). Keep this header free of a second definition so files that
 * include both headers do not get conflicting macros. */

#ifdef __cplusplus
}
#endif

#endif /* PS2_DEBUG_H_ */
