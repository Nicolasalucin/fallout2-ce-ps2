/*
 * PS2 log verbosity gate.
 *
 * The port prints a small set of one-shot boot lines (device probe, save
 * root, audio init) that are useful in every run. Everything that can fire
 * per file-open, per save-path remap or per pad poll goes through the
 * PS2_LOGV() macro instead and stays silent unless verbose logging is
 * explicitly enabled (ps2_log_set_verbose(1)), keeping the emulator log
 * readable while debugging other subsystems.
 *
 * In Release builds (CMAKE_BUILD_TYPE=Release), verbose logging is compiled
 * out entirely to eliminate branch overhead and printf calls from hot paths.
 * Define PS2_DEBUG_VERBOSE to force-enable in any build type.
 */

#ifndef PS2_LOG_H_
#define PS2_LOG_H_

#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

void ps2_log_set_verbose(int on);
int ps2_log_verbose(void);

#if defined(PS2_DEBUG_VERBOSE)
#define PS2_LOGV(...)                     \
    do {                                  \
        if (ps2_log_verbose()) {          \
            printf(__VA_ARGS__);          \
        }                                 \
    } while (0)
#else
#define PS2_LOGV(...) do { } while (0)
#endif

#ifdef __cplusplus
}
#endif

#endif /* PS2_LOG_H_ */
