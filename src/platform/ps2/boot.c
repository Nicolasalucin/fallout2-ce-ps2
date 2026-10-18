/*
 * Platform bootstrap for the Fallout 2 PS2 port.
 *
 * Runs before the engine starts, in this order:
 *
 *   timing  -> EE timebase (SDL_GetTicks/SDL_Delay)
 *   input   -> libpad init (DualShock 2)
 *   storage -> USB/BDM IOP drivers for raw-ELF boots + ELF-relative
 *              asset discovery seeded from argv[0]
 *   fs      -> device probing + case-insensitive path resolution
 *   save    -> memory card / host save root
 *
 * Audio (audsrv) is initialized lazily, when the engine opens the mixer.
 */

#include "ps2_time.h"
#include "ps2_input.h"
#include "ps2_fs.h"
#include "ps2_save.h"
#include "ps2_storage.h"

#include <stdio.h>

int ps2_boot(const char* argv0)
{
    ps2_time_init();
    ps2_input_init();
    ps2_storage_init(argv0);
    ps2_fs_init();
    ps2_save_init();

    printf("[ps2] boot ok, timebase %u ms\n", (unsigned)ps2_time_ms());

    return 0;
}
