/*
 * PS2 log verbosity gate.
 *
 * The port prints a small set of one-shot boot lines (device probe, save
 * root, audio init) that are useful in every run. Everything that can fire
 * per file-open, per save-path remap or per pad poll goes through the
 * PS2_LOGV() macro instead and stays silent unless verbose logging is
 * explicitly enabled (ps2_log_set_verbose(1)), keeping the emulator log
 * readable while debugging other subsystems.
 */

#ifndef PS2_LOG_H_
#define PS2_LOG_H_

#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

void ps2_log_set_verbose(int on);
int ps2_log_verbose(void);

#define PS2_LOGV(...)                     \
    do {                                  \
        if (ps2_log_verbose()) {          \
            printf(__VA_ARGS__);          \
        }                                 \
    } while (0)

#ifdef __cplusplus
}
#endif

#endif /* PS2_LOG_H_ */
