#ifndef PS2_OSK_H_
#define PS2_OSK_H_

/*
 * On-screen keyboard.
 *
 * A modal, pad-driven overlay that types into the engine's editable text
 * widgets by injecting ordinary SDL key events - the same synthetic
 * stream the DualShock mapping uses - so the engine needs no knowledge of
 * it at all:
 *
 *   - It opens by itself whenever the engine enables text input
 *     (SDL_StartTextInput from beginTextInput(), i.e. the save-name
 *     prompt, the editor and the file dialogs) and closes on the matching
 *     SDL_StopTextInput.
 *   - L3 toggles it manually at any time.
 *   - While open it owns the whole pad: D-pad / left stick move the grid
 *     cursor, Cross types the selected character, Square is backspace,
 *     Start submits (Return) and Circle cancels (Escape).
 *   - Letters type uppercase (shift is held on injection), console
 *     save-name style; digits, '_', '-' and '.' type unshifted.
 *
 * The panel is composited by the video backend straight into the CT32
 * frame (see ps2_video_present); no engine buffer is ever touched.
 */

#ifdef __cplusplus
extern "C" {
#endif

void ps2_osk_open(void);
void ps2_osk_close(void);
int ps2_osk_is_open(void);

/* Feeds one pad poll while the OSK is open. `pressed` uses the same
 * active-high PAD_* layout as the libpad path, lx/ly are the raw left-stick
 * axes (0..255, 128 = centred). The OSK consumes the whole pad while open. */
void ps2_osk_input(unsigned int pressed, int lx, int ly, unsigned int now);

/* Composites the panel into a CT32 (A B G R) frame buffer. Cheap no-op
 * while the OSK is closed. */
void ps2_osk_render(void* fb, int width, int height);

#ifdef __cplusplus
}
#endif

#endif /* PS2_OSK_H_ */
