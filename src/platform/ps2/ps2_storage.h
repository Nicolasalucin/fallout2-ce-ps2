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

/* Executes one embedded IOP module. SifExecModuleBuffer() DMAs the module
 * image from `data`; the EE DMAC ignores the low four address bits, so a
 * pointer that is not 16-byte aligned silently shifts the image and the IOP
 * rejects it (silent audio/USB death on real hardware). If `data` is
 * misaligned it is copied to a static 64-byte aligned bounce buffer first.
 * Returns SifExecModuleBuffer's value (>= 0 on success). */
int ps2_irx_exec(const char* name, const unsigned char* data, unsigned int size);

/* Logs address/size/alignment for every IRX embedded in the ELF. Used by
 * ps2_boot() so a hardware debug.log proves whether the build is correct. */
void ps2_irx_alignment_report(void);

/* Normalized directory of the running ELF with a trailing slash
 * ("mass:/FALLOUT2/", "host:/", "cdrom0:/"), or NULL when argv[0] carries no
 * usable device path. Valid after ps2_storage_init(). */
const char* ps2_storage_elf_dir(void);

/* Returns 1 when `path` (a device root or directory) can be stat'ed or
 * opened. Uses stat()/dopen() first so it never triggers the usbhdfsd
 * readdir path that hangs under PCSX2's HLE. */
int ps2_storage_device_present(const char* path);

#ifdef __cplusplus
}
#endif

#endif /* PS2_STORAGE_H_ */
