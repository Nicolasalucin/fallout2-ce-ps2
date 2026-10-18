#ifndef PS2_SAVE_H_
#define PS2_SAVE_H_

/*
 * PS2 save-slot redirection + PS2 browser metadata.
 *
 * The engine treats a save slot as a directory ("SAVEGAME\SLOTxx\", usually
 * with the patch-dir prefix "data\") holding SAVE.DAT plus one ".SAV" per map
 * and AUTOMAP.SAV. This module maps those paths onto PS2 storage without
 * touching engine code: fs.c and the platform compat layer run every
 * relative path through ps2_save_remap() before resolving it.
 *
 *   data\SAVEGAME                -> <root>/
 *   data\SAVEGAME\SLOT01\X.SAV   -> <root>/<slot dir>/X.SAV
 *
 * Device selection (probed once, first use):
 *
 *   mc0:/   slots FALLOUT2_S01..FALLOUT2_S10   (preferred)
 *   mc1:/   slots FALLOUT2_S01..FALLOUT2_S10   (mc0: absent/unformatted)
 *   <first data root>saves/SLOTxx              (host fallback; always used
 *                                               by dev/headless builds so
 *                                               saves stay inspectable from
 *                                               the build machine)
 *
 * Whenever a slot directory is created, a browsable PS2 save is generated
 * inside it: icon.sys (964 bytes, Shift-JIS title + standard lighting) and
 * icon.icn (static untextured mesh). Formats follow the published icon.sys
 * layout and the "PS2 Icon Format v0.5" specification, mirrored from icon
 * files shipped by real saves.
 */

#ifdef __cplusplus
extern "C" {
#endif

/* Official sizes of the generated metadata files. */
#define PS2_SAVE_ICON_SYS_SIZE 964
#define PS2_SAVE_ICON_ICN_SIZE 928

/* Number of engine save slots (SLOT01..SLOT10). */
#define PS2_SAVE_SLOT_COUNT 10

/* Probes the save device once and logs the result. Idempotent; every entry
 * point calls it lazily, ps2_boot() calls it eagerly for the log. */
void ps2_save_init(void);

/* Root path of the selected save device, e.g. "mc0:/" or
 * "host:local_assets/saves/". */
const char* ps2_save_root(void);

/* Remaps an engine save path onto the save device. Accepts relative
 * "SAVEGAME..." / "data\SAVEGAME..." spellings ("/" or "\" separators, any
 * case). Returns 0 when the path was recognized and `out` filled; non-zero
 * when the path has nothing to do with saves (the caller then falls back to
 * its normal resolution). */
int ps2_save_remap(const char* path, char* out, int out_size);

/* Called by compat_mkdir() after a successful mkdir: when `resolved_path`
 * addresses a save-slot directory, generates icon.sys/icon.icn inside it
 * (no-op for every other directory and when the metadata already exists). */
void ps2_save_slot_ensure_meta(const char* resolved_path);

/* Dev/smoke helper: deletes all files inside a save slot (1..10) and the
 * directory itself, so the next save starts from a fresh slot. Returns 0
 * when the slot is clean (also when it did not exist). */
int ps2_save_slot_wipe(int slot);

#ifdef __cplusplus
}
#endif

#endif /* PS2_SAVE_H_ */
