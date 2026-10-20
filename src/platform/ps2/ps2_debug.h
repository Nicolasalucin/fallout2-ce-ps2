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
 * PCSX2 note: there is deliberately no "host:" fallback. PCSX2's hostfs HLE
 * can wedge on the open/append/close-per-line pattern under strict emulation;
 * under the emulator the console mirror (captured by tools/run_smoke.sh into
 * build_ps2/pcsx2_strict.log) carries the same lines.
 */

#ifdef __cplusplus
extern "C" {
#endif

/* Probes the destination chain. Safe to call before ps2_storage_init();
 * early lines are buffered until a device is available. */
void ps2_debug_init(void);

/* Re-probes the chain, preferring mass: again. Call after the storage
 * drivers have been brought up and the VFS roots are known. */
void ps2_debug_reprobe(void);

/* Writes one line (printf format) to the current destination. Each call does
 * fopen("a") + vfprintf + fflush + fclose. Also mirrors the line to stdout so
 * PCSX2's strict console log keeps a copy. */
void ps2_debug_printf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

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
