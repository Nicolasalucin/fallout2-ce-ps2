/*
 * [PERF] telemetry for the PS2 port.
 *
 * Cheap, always-on counters that the smoke harness (F6) turns into the
 * load/latency/FPS report. They are compiled into every PS2 build; on
 * desktop builds the call sites are guarded by __PS2__ so nothing here is
 * referenced.
 *
 * Covered today:
 *   - map load wall time (ps2_perf_map_* called from map.cc)
 *   - UI open latency by name (ps2_perf_ui_begin/_end; optionally sampled
 *     automatically per frame by the input hook, see input.cc)
 *   - combat difficulty observation: confirms the difficulty branches in
 *     combat.cc / combat_ai.cc are actually read while a battle runs
 */

#ifndef PS2_PERF_H_
#define PS2_PERF_H_

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
void ps2_perf_frame_tick(unsigned int nowMs);
void ps2_perf_ui_context(const char* name);
void ps2_perf_frame_reset(void);
int ps2_perf_frame_avg_ms(void);
int ps2_perf_frame_max_ms(void);
int ps2_perf_frame_slow_count(void);
const char* ps2_perf_frame_worst_state(void);

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

#endif /* PS2_PERF_H_ */
