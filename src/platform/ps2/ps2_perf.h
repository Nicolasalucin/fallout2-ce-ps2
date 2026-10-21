/*
 * [PERF] telemetry for the PS2 port.
 *
 * Smoke-test only. Everything in this header and in ps2_perf.c is compiled
 * when PS2_DEV_SMOKE_TEST is defined; production builds get no-op macros so
 * the call sites cannot emit timers, counters or [PERF] strings, and the
 * profiling translation unit is empty.
 *
 * Covered today:
 *   - map load wall time (ps2_perf_map_* called from map.cc)
 *   - UI open latency by name (ps2_perf_ui_begin/_end; optionally sampled
 *     automatically per frame by the input hook, see input.cc)
 *   - combat difficulty observation: confirms the difficulty branches in
 *     combat.cc / combat_ai.cc are actually read while a battle runs
 *   - high-resolution stages (video present, tiles, objects, scripts, VFS)
 *     accumulated per frame, per act (ui context) and per map tour entry
 *   - event counters (blitters, script calls, cache, VFS/inflate)
 *
 * The A/B metric is cpu_active = frame - flip: frame is sampled from ticks
 * at consecutive input polls (so it includes any vsync wait), flip is the
 * time spent inside gsKit_sync_flip(). Stage values and the worst frame are
 * kept in raw timer ticks (147.456 MHz) and converted only when printed.
 */

#ifndef PS2_PERF_H_
#define PS2_PERF_H_

#include <stdint.h>

#ifdef PS2_DEV_SMOKE_TEST

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Map load ---- */

void ps2_perf_map_load_begin(void);
void ps2_perf_map_load_end(const char* mapName);
int ps2_perf_map_load_last_ms(void);
int ps2_perf_map_load_max_ms(void);
int ps2_perf_map_load_count(void);

/* ---- UI latency ---- */

/* Manual bracketing (used by the smoke harness around direct UI calls).
 * Nesting is not supported: only the outermost pair counts. */
void ps2_perf_ui_begin(const char* name);
void ps2_perf_ui_end(void);

/* Automatic sampling of the engine frame time. Called once per frame from
 * the input hook; produces per-frame min/avg/max and reports frames that
 * exceeded 50 ms as "slow frames" together with the current UI context
 * (set with ps2_perf_ui_context). */
void ps2_perf_frame_tick(uint64_t nowTicks);
void ps2_perf_ui_context(const char* name);
void ps2_perf_frame_reset(void);
int ps2_perf_frame_avg_ms(void);
int ps2_perf_frame_max_ms(void);
int ps2_perf_frame_slow_count(void);
const char* ps2_perf_frame_worst_state(void);

/* ---- High-resolution stage profiling ---- */

/* Keep this list in sync with s_stage_names[] in ps2_perf.c. */
typedef enum Ps2PerfStage {
    PS2_PERF_STAGE_MAP_LOAD = 0,
    PS2_PERF_STAGE_TILE_REFRESH,
    PS2_PERF_STAGE_TILE_FLOOR,
    PS2_PERF_STAGE_TILE_ROOF,
    PS2_PERF_STAGE_OBJ_PRE_ROOF,
    PS2_PERF_STAGE_OBJ_POST_ROOF,
    PS2_PERF_STAGE_SCRIPTS_REQ,
    PS2_PERF_STAGE_SCRIPTS_MAP,
    PS2_PERF_STAGE_SCRIPT_EXEC,
    PS2_PERF_STAGE_INTERP_UPDATE,
    PS2_PERF_STAGE_PRESENT_LUT,
    PS2_PERF_STAGE_PRESENT_OSK,
    PS2_PERF_STAGE_PRESENT_UPLOAD,
    PS2_PERF_STAGE_PRESENT_DRAW,
    PS2_PERF_STAGE_PRESENT_EXEC,
    PS2_PERF_STAGE_PRESENT_FLIP,
    PS2_PERF_STAGE_VFS_READ,
    PS2_PERF_STAGE_VFS_RAWIO,
    PS2_PERF_STAGE_VFS_INFLATE,
    PS2_PERF_STAGE_COUNT
} Ps2PerfStage;

/* Event counters. Additive, never divided; cheap enough for per-call use. */
typedef enum Ps2PerfCounter {
    PS2_PERF_COUNT_TRANS = 0,
    PS2_PERF_COUNT_DARK_TRANS,
    PS2_PERF_COUNT_TRANS_LUCENT,
    PS2_PERF_COUNT_DARK_TRANS_LUCENT,
    PS2_PERF_COUNT_INTENSITY_MASK,
    PS2_PERF_COUNT_BLIT_PIXELS,
    PS2_PERF_COUNT_SCRIPT_REQUESTS,
    PS2_PERF_COUNT_SCRIPT_EXECS,
    PS2_PERF_COUNT_CACHE_HITS,
    PS2_PERF_COUNT_CACHE_MISSES,
    PS2_PERF_COUNT_CACHE_EVICTIONS,
    PS2_PERF_COUNT_DFILE_READS,
    PS2_PERF_COUNT_DFILE_BYTES,
    PS2_PERF_COUNT_INFLATE_CALLS,
    PS2_PERF_COUNT_INFLATE_BYTES,
    PS2_PERF_COUNT_COUNT
} Ps2PerfCounter;

/* Single-threaded EE instrumentation: begin/end pairs may nest per stage,
 * they only accumulate when the outermost pair closes. */
void ps2_perf_stage_begin(int stage);
void ps2_perf_stage_end(int stage);
void ps2_perf_counter_add(int counter, unsigned long long value);

/* Reset the per-act/per-map accumulator (not the run-wide totals). */
void ps2_perf_scope_reset(void);

/* Move any stage ticks from the frame in progress into the scope totals. */
void ps2_perf_scope_flush(void);

/* Print the accumulator for `tag`: frame/flip/cpu_active averages, stage
 * averages per frame and event counts. No output when the scope is empty. */
void ps2_perf_scope_print(const char* tag);

/* ---- Combat difficulty observation ---- */

#define PS2_PERF_CBT_TOHIT 0
#define PS2_PERF_CBT_DAMAGE 1
#define PS2_PERF_CBT_AI 2

void ps2_perf_cbt_difficulty(int kind, int difficulty, int modifier);
int ps2_perf_cbt_count(int kind);
int ps2_perf_cbt_difficulty_seen(int kind);
int ps2_perf_cbt_mod(int kind);

/* Bitmask of the difficulty values seen by `kind` (bit N = difficulty N). */
unsigned int ps2_perf_cbt_diff_mask(int kind);

/* ---- Reporting ---- */

void ps2_perf_reset(void);
void ps2_perf_print(void);

#ifdef __cplusplus
}
#endif

#else /* !PS2_DEV_SMOKE_TEST */

/* Production: instrumentation is a source-level no-op. Keep the call sites
 * readable without emitting any code, timer read or string constant. */
#define ps2_perf_map_load_begin() ((void)0)
#define ps2_perf_map_load_end(mapName) ((void)0)
#define ps2_perf_map_load_last_ms() (0)
#define ps2_perf_map_load_max_ms() (0)
#define ps2_perf_map_load_count() (0)

#define ps2_perf_ui_begin(name) ((void)0)
#define ps2_perf_ui_end() ((void)0)
#define ps2_perf_frame_tick(nowTicks) ((void)0)
#define ps2_perf_ui_context(name) ((void)0)
#define ps2_perf_frame_reset() ((void)0)
#define ps2_perf_frame_avg_ms() (0)
#define ps2_perf_frame_max_ms() (0)
#define ps2_perf_frame_slow_count() (0)
#define ps2_perf_frame_worst_state() ((const char*)0)

#define ps2_perf_stage_begin(stage) ((void)0)
#define ps2_perf_stage_end(stage) ((void)0)
#define ps2_perf_counter_add(counter, value) ((void)0)
#define ps2_perf_scope_reset() ((void)0)
#define ps2_perf_scope_flush() ((void)0)
#define ps2_perf_scope_print(tag) ((void)0)

#define ps2_perf_cbt_difficulty(kind, difficulty, modifier) ((void)0)
#define ps2_perf_cbt_count(kind) (0)
#define ps2_perf_cbt_difficulty_seen(kind) (0)
#define ps2_perf_cbt_mod(kind) (0)
#define ps2_perf_cbt_diff_mask(kind) (0)

#define ps2_perf_reset() ((void)0)
#define ps2_perf_print() ((void)0)

#endif /* PS2_DEV_SMOKE_TEST */

#endif /* PS2_PERF_H_ */
