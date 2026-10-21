/*
 * Platform bootstrap for the Fallout 2 PS2 port.
 *
 * Runs before the engine starts, in this order:
 *
 *   telemetry -> persistent debug.log (survives a console freeze)
 *   timing    -> EE timebase (SDL_GetTicks/SDL_Delay)
 *   input     -> libpad init (DualShock 2)
 *   storage   -> USB/BDM IOP drivers for raw-ELF boots + ELF-relative
 *                asset discovery seeded from argv[0]
 *   fs        -> device probing + case-insensitive path resolution
 *   save      -> memory card / host save root
 *   logging   -> stdout/stderr redirected to mass:/FALLOUT2/debug.log
 *                AFTER fs probing completes (avoids PCSX2 readdir hang)
 *
 * Audio (audsrv) is initialized lazily, when the engine opens the mixer.
 */

#include "ps2_time.h"
#include "ps2_input.h"
#include "ps2_fs.h"
#include "ps2_save.h"
#include "ps2_storage.h"

#include "ps2_debug.h"
#include "ps2_mem.h"

#include <stdio.h>

static void boot_log_fs_roots(void)
{
    int count = ps2_fs_root_count();
    int i;

    PS2_LOG("[ps2] boot: VFS roots = %d", count);
    for (i = 0; i < count; i++) {
        PS2_LOG("[ps2] boot: root[%d] = '%s'", i, ps2_fs_root_path(i));
    }

    /* The shipped media is uppercase, the engine asks for lowercase paths.
     * Log both spellings so a hardware report proves the FAT32 case-insensitive
     * resolver is doing its job. */
    {
        static const char* const probes[] = {
            "DATA/",
            "data/",
            "DATA/MAPS/",
            "data/maps/",
        };
        int p;

        for (p = 0; p < (int)(sizeof(probes) / sizeof(probes[0])); p++) {
            char resolved[256];

            resolved[0] = '\0';
            if (ps2_fs_resolve(probes[p], resolved, sizeof(resolved), 0) == 0) {
                PS2_LOG("[ps2] boot: resolve '%s' -> '%s'", probes[p], resolved);
            } else {
                PS2_LOG("[ps2] boot: resolve '%s' -> NOT FOUND", probes[p]);
            }
        }
    }
}

int ps2_boot(const char* argv0)
{
    ps2_time_init();
    ps2_debug_init();
    ps2_mem_init();

    PS2_LOG("[ps2] boot: argv0='%s'", argv0 != NULL ? argv0 : "");
    PS2_LOG("[ps2] boot: debug log='%s'", ps2_debug_path());

    ps2_input_init();
    ps2_storage_init(argv0);

    /* mass: exists now (ps2_storage_init() loaded usbd/usbmass_bd and
     * confirmed the device). Probe the data roots FIRST (ps2_fs_init does
     * case-insensitive readdir on mass:/ which hangs in PCSX2's usbhdfsd HLE
     * if the vfat driver was exercised by prior opens). Redirect stdout/stderr
     * AFTER fs probing completes, so every plain printf from here on lands in
     * mass:/FALLOUT2/debug.log. */
    ps2_fs_init();
    ps2_save_init();

    /* Now safe to redirect - the dangerous readdir on mass:/ is done. */
    ps2_init_debug_log();

    /* No-op once the redirect above took over; otherwise the first lines
     * above were buffered and are replayed to the per-line destination. */
    ps2_debug_reprobe();
    PS2_LOG("[ps2] boot: debug log active='%s'", ps2_debug_path());

    boot_log_fs_roots();
    ps2_irx_alignment_report();

    PS2_LOG("[ps2] boot: save root='%s'", ps2_save_root());
    PS2_LOG("[ps2] boot: heap free=%d KB used=%d KB", ps2_mem_free_kb(), ps2_mem_used_kb());
    PS2_LOG("[ps2] boot: boot ok, timebase %u ms", (unsigned)ps2_time_ms());

    printf("[ps2] boot ok, timebase %u ms\n", (unsigned)ps2_time_ms());

    return 0;
}