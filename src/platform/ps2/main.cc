/*
 * PlayStation 2 entry point (replaces src/win32.cc on this port).
 *
 * win32.cc can't be used here because the platform must be brought up
 * (timing, input, I/O modules, audio) before the engine starts, and because it
 * defines the desktop SDL entry point. This file re-declares the global that
 * win32.cc owns (gProgramIsActive) and forwards to falloutMain.
 *
 * The engine treats itself as inactive (and thus ignores input) until it
 * receives a window focus event; on PS2 the window is always focused, so the
 * flag is set at boot (the SDL shim also posts a FOCUS_GAINED event for
 * consistency with the desktop flow).
 */

#include "win32.h"

#include <stdio.h>

#include <SDL.h>

#include "main.h"
#include "ps2_video.h"

extern "C" int ps2_boot(const char* argv0);

namespace fallout {

bool gProgramIsActive = true;

} // namespace fallout

int main(int argc, char* argv[])
{
    if (ps2_boot(argc > 0 ? argv[0] : NULL) != 0) {
        printf("[ps2] boot failed\n");
        return 1;
    }

    /* Bring-up diagnostic (color bars, GS timing probes, pad transition log).
     * It costs about 20 seconds and is not part of the game, so it is off
     * unless a build asks for it with -DPS2_DEV_BOOT_SELFTEST=1. */
#ifdef PS2_DEV_BOOT_SELFTEST
    ps2_video_selftest();
#endif

    /* The desktop entry point hides the OS cursor at startup; the engine draws
     * its own. Same call, same initial state, so the cursor logic in
     * mouse.cc/window_manager.cc behaves exactly as it does upstream. */
    SDL_ShowCursor(SDL_DISABLE);

    int rc = fallout::falloutMain(argc, argv);

    printf("[ps2] engine exited with code %d\n", rc);
    return rc;
}
