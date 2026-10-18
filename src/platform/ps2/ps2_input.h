#ifndef PS2_INPUT_H_
#define PS2_INPUT_H_

/*
 * Input backend contract for the Fallout PS2 port.
 *
 * The DualShock 2 pad is translated into synthetic SDL2 mouse/keyboard
 * events (see sdl/ps2_sdl_internal.h for the posting helpers). The engine's
 * own input logic (kb.cc, input.cc, mouse.cc) consumes these events
 * untouched.
 */

#ifdef __cplusplus
extern "C" {
#endif

void ps2_input_init(void);

/* Called by the SDL shim on every event pump (at most a few times a frame;
 * the pad is only actually polled once per frame internally). */
void ps2_input_poll(void);

/* Engine-side bridge (implemented in platform_compat_ps2.cc): 1 while a
 * turn-based battle is running. Used for the context-sensitive R1
 * mapping (attack outside combat, end-combat attempt inside). Reads only
 * a global, so it is safe before the engine boots. */
int ps2_platform_in_combat(void);

#ifdef PS2_DEV_SMOKE_TEST
/* Smoke-test hooks: drive the pad path on the real ELF without hardware.
 * `buttons` uses the same active-high PAD_* layout the libpad path uses;
 * release() falls back to the real pad. */
void ps2_input_smoke_press(unsigned int buttons);
void ps2_input_smoke_release(void);
/* 1 while a release sample (buttons==0) is queued but has not been polled
 * yet: a new press would overwrite it and merge two identical taps. */
int ps2_input_smoke_release_pending(void);
#endif

#ifdef __cplusplus
}
#endif

#endif /* PS2_INPUT_H_ */
