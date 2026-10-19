#ifndef PS2_FS_H_
#define PS2_FS_H_

/*
 * PS2 filesystem layer.
 *
 * The engine asks for game data with relative names ("master.dat",
 * "sound/music/01hub.acm", data files in backslash notation, ...). This layer
 * maps those names onto the PS2 storage devices with this probe order:
 *
 *   <ELF dir>/            (relative to the running ELF, from argv[0])
 *   <ELF dir>/local_assets/
 *   mass:/FALLOUT2/       (USB stick / MX4SIO)
 *   mass:/                (USB stick, data at the root)
 *   mass0:/FALLOUT2/      (numbered mass device)
 *   cdrom0:/              (CD/DVD)
 *   cdrom0:/FALLOUT2/
 *   hdd0:/FALLOUT2/       (internal HDD, PFS partition)
 *   host:local_assets/    (PCSX2 host filesystem / ps2client dev layout)
 *   host:../local_assets/ (ELF in build_ps2/ under the repo)
 *   host:/                (PCSX2 host root)
 *
 * A root is only accepted when both master.dat and critter.dat resolve there.
 * Lookups are case-insensitive (Fallout 2's media is lowercase, Fallout 1's
 * was uppercase): every component is matched with readdir() against the real
 * directory entries, which works on FAT32, ISO9660, host and PFS alike.
 * Large ".dat" files get a block-sized stdio buffer so random reads inside
 * master.dat/critter.dat do not degrade into tiny driver transactions.
 */

#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Probes the storage devices once and logs the result. Idempotent; every
 * entry point calls it lazily, but ps2_boot() calls it eagerly for the log. */
void ps2_fs_init(void);

/* Number of usable data roots found (0 when nothing is mounted). */
int ps2_fs_root_count(void);

/* Root path for diagnostics, e.g. "host:local_assets/". */
const char* ps2_fs_root_path(int index);

/* Kinds of the last component of a resolved path, taken from the readdir()
 * entry type (d_type). PS2_FS_KIND_UNKNOWN means the driver did not report
 * one. */
#define PS2_FS_KIND_UNKNOWN 0
#define PS2_FS_KIND_FILE 1
#define PS2_FS_KIND_DIR 2

/* Resolves `path` (relative engine path or absolute iomanX path) to a real,
 * correctly cased device path. `writable` allows resolving a not-yet-existing
 * last component and restricts relative lookups to writable roots if any
 * exist. Returns 0 on success (result copied to `out`). */
int ps2_fs_resolve(const char* path, char* out, int out_size, int writable);

/* Same as ps2_fs_resolve(), additionally reporting the kind of the last
 * component in *out_kind (NULL to ignore). */
int ps2_fs_resolve_kind(const char* path, char* out, int out_size, int writable, int* out_kind);

/* Resolves and opens `path` with `mode` (fopen semantics). Returns NULL when
 * the file cannot be found in any root. */
FILE* ps2_fs_fopen(const char* path, const char* mode);

/* Existence check without opening the file (compat_access(F_OK)). Resolves
 * `path` the same way ps2_fs_fopen() does but stops there. */
/**
 * True when the device hosting [resolved] implements delete-type fileio RPCs.
 *
 * False for the PCSX2 "host:" device, where every remove() replies "Unknown
 * device 'host'". A failing fileio RPC is what upsets the emulator's host
 * filesystem HLE and derails the IOP RPC state, so callers use this to answer
 * a doomed erase locally instead of issuing the RPC. The engine treats a
 * failed erase as non-fatal, and that is already the outcome today.
 */
int ps2_fs_delete_supported(const char* resolved);

/* Erases a resolved path (a canonical device path from ps2_fs_resolve()).
 * Devices with delete support (memory card, HDD) remove the file for real;
 * devices without it (the PCSX2 "host:" filesystem) record the path as
 * logically deleted instead, so later read resolutions answer "not found".
 * Returns 0 on success. */
int ps2_fs_remove(const char* resolved);

/* True when `resolved` has a logical-delete record (see ps2_fs_remove). */
int ps2_fs_tombstoned(const char* resolved);

/* Drops the logical-delete record of `resolved`, if any: writing the file or
 * creating the directory again revives the path. Returns 1 when a record was
 * dropped. */
int ps2_fs_clear_tombstone(const char* resolved);

int ps2_fs_exists(const char* path);

/* Directory probe used when the engine registers a directory-backed xbase
 * (src/xfile.cc) and to keep dbaseOpen() from opening a directory as a DAT
 * file (src/dfile.cc).
 *
 * The answer comes from the readdir() entry type already collected while
 * resolving the path, so no opendir() is issued on the target: on PCSX2 a
 * dopen() of a regular file fails and upsets the host filesystem HLE. */
int ps2_fs_directory_exists(const char* path);

/* In-place case-insensitive canonicalization of a device path such as
 * "host:local_assets/master.dat". When `allow_missing_last` is non-zero the
 * final component may be absent (for creating files). Returns 0 on success. */
int ps2_fs_canonicalize(char* path, int allow_missing_last);

/* Drops every cached directory listing (the save-slot wipe removes whole
 * directories). */
void ps2_fs_invalidate_dir_cache(void);

/* Drops the cached listing of the directory holding `resolved` (a canonical
 * device path). compat_mkdir/compat_remove/compat_rename call this after
 * changing a directory outside the fopen() path. */
void ps2_fs_invalidate_parent_of(const char* resolved);

/* Ensures all parent directories of a writable MC path exist.
 * Creates missing components along the path. */
void ps2_fs_ensure_parent_dirs(const char* resolved);

#ifdef PS2_DEV_SMOKE_TEST
/* Dev profiling counters: how much directory resolution work the engine
 * caused. Reported by the F6 smoke suite so VFS/DAT changes can be told
 * apart from rendering work. */
extern unsigned int ps2_fs_stat_fopen_calls;
extern unsigned int ps2_fs_stat_canonicalize_calls;
extern unsigned int ps2_fs_stat_dir_cache_hits;
extern unsigned int ps2_fs_stat_dir_scans;

void ps2_fs_stat_reset(void);
#endif

#ifdef __cplusplus
}
#endif

#endif /* PS2_FS_H_ */
