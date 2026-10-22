#ifndef PS2_EXIT_H_
#define PS2_EXIT_H_

/*
 * Clean boot-failure screen and return-to-loader path for Fallout 2.
 *
 * A raw-ELF launch has no operating system to return to: the port has to
 * re-enter the loader itself. ps2_exit_to_loader() tries the standard
 * boot.elf locations used by wLaunchELF/OPL and finally falls back to the
 * PS2 browser (rom0:OSDSYS) or Exit().
 */

#ifdef __cplusplus
extern "C" {
#endif

/* Draws a fatal message with libdebug's raw GS console, waits `hold_ms` so
 * the user can read it, then calls ps2_exit_to_loader(1). Never returns. */
void ps2_fatal_show(const char* title,
    const char* line1,
    const char* line2,
    const char* line3,
    int hold_ms) __attribute__((noreturn));

/* Flushes the debug log and returns to the loader/browser. In smoke builds
 * it exits the process so the harness reports the failure. Never returns. */
void ps2_exit_to_loader(int status) __attribute__((noreturn));

#ifdef __cplusplus
}
#endif

#endif /* PS2_EXIT_H_ */
