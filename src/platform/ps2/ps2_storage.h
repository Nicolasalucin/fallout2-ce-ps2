#ifndef PS2_STORAGE_H_
#define PS2_STORAGE_H_

/*
 * PS2 storage bootstrap.
 *
 * Two jobs, both fully tolerated to fail:
 *
 *  1. Recover the directory the ELF was launched from (argv[0]) so fs.c can
 *     look for master.dat / critter.dat right next to the game before falling
 *     back to the fixed device chain.
 *
 *  2. Bring up USB mass storage for raw-ELF boots (uLaunchELF "Launch Elf",
 *     etc.). The ps2sdk IOP drivers are embedded in the ELF (see
 *     CMakeLists.txt) and executed from memory, so no loose .irx files need
 *     to ship next to the game data.
 *
 * When a loader already provides I/O (OPL, uLaunchELF, ps2link, PCSX2 host
 * filesystem) the device answers before any module is touched and the IOP is
 * left alone; a module that is already resident is logged and ignored.
 */

#ifdef __cplusplus
extern "C" {
#endif

/* `argv0` may be NULL or empty; safe to call once at boot, before
 * ps2_fs_init(). */
void ps2_storage_init(const char* argv0);

/* Normalized directory of the running ELF with a trailing slash
 * ("mass:/FALLOUT2/", "host:/", "cdrom0:/"), or NULL when argv[0] carries no
 * usable device path. Valid after ps2_storage_init(). */
const char* ps2_storage_elf_dir(void);

#ifdef __cplusplus
}
#endif

#endif /* PS2_STORAGE_H_ */
