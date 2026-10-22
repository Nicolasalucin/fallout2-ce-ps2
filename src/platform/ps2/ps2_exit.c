/*
 * Boot-failure screen and return-to-loader path for the Fallout 2 PS2 port.
 *
 * The GS console is libdebug's raw setup (no gsKit): it works before the
 * video backend exists and after a GS initialization failure.
 */

#include "ps2_exit.h"
#include "ps2_debug.h"
#include "ps2_storage.h"

#include "ps2sdk_debug.h"
#include <delaythread.h>
#include <fcntl.h>
#include <kernel.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int exit_file_exists(const char* path)
{
    int fd;

    if (path == NULL || path[0] == '\0') {
        return 0;
    }

    fd = open(path, O_RDONLY);
    if (fd < 0) {
        return 0;
    }

    close(fd);
    return 1;
}

void ps2_fatal_show(const char* title,
    const char* line1,
    const char* line2,
    const char* line3,
    int hold_ms)
{
    init_scr();
    scr_clear();
    scr_setbgcolor(0x000000);
    scr_setfontcolor(0xFFFFFF);

    scr_printf("\n");
    scr_printf("  FALLOUT 2 for PlayStation 2\n");
    scr_printf("  -------------------------\n\n");
    scr_printf("  %s\n\n", title != NULL ? title : "BOOT FAILURE");

    if (line1 != NULL) {
        scr_printf("  %s\n", line1);
    }
    if (line2 != NULL) {
        scr_printf("  %s\n", line2);
    }
    if (line3 != NULL) {
        scr_printf("  %s\n", line3);
    }

    scr_printf("\n  The console will return to the loader in %d seconds.\n",
        hold_ms / 1000);

    PS2_LOG("[ps2] FATAL: %s | %s | %s | %s",
        title != NULL ? title : "",
        line1 != NULL ? line1 : "",
        line2 != NULL ? line2 : "",
        line3 != NULL ? line3 : "");

    if (hold_ms < 1000) {
        hold_ms = 1000;
    }
    DelayThread((unsigned int)hold_ms * 1000u);

    ps2_exit_to_loader(1);
}

void ps2_exit_to_loader(int status)
{
    static const char* const boot_paths[] = {
        "boot.elf",
        "mc0:/BOOT/BOOT.ELF",
        "mc0:/BOOT/boot.elf",
        "mass:/BOOT/BOOT.ELF",
        "mass:/BOOT/boot.elf",
        "mass:/boot.elf",
        "mc0:/boot.elf",
        "host:boot.elf",
    };
    const char* elf_dir;
    char elf_boot[256];
    int i;

    PS2_LOG("[ps2] exit: returning to loader (status %d)", status);
    ps2_debug_flush_all();

#ifdef PS2_DEV_SMOKE_TEST
    /* The smoke runner owns the process and expects a clean exit code;
     * LoadExecPS2 would boot the BIOS instead of reporting the verdict. */
    exit(status);
#else
    for (i = 0; i < (int)(sizeof(boot_paths) / sizeof(boot_paths[0])); i++) {
        if (!exit_file_exists(boot_paths[i])) {
            continue;
        }

        PS2_LOG("[ps2] exit: LoadExecPS2('%s')", boot_paths[i]);
        LoadExecPS2(boot_paths[i], 0, NULL);
    }

    elf_dir = ps2_storage_elf_dir();
    if (elf_dir != NULL
        && snprintf(elf_boot, sizeof(elf_boot), "%sboot.elf", elf_dir) < (int)sizeof(elf_boot)
        && exit_file_exists(elf_boot)) {
        PS2_LOG("[ps2] exit: LoadExecPS2('%s')", elf_boot);
        LoadExecPS2(elf_boot, 0, NULL);
    }

    /* Last resort: return to the PS2 browser. This is the same target the
     * console uses after a normal game exit. */
    PS2_LOG("[ps2] exit: falling back to rom0:OSDSYS");
    LoadExecPS2("rom0:OSDSYS", 0, NULL);

    /* LoadExecPS2 is not supposed to return, but a missing/broken loader
     * module must not leave the console spinning in this function. */
    ResetEE(0);
    Exit(status);
#endif
}
