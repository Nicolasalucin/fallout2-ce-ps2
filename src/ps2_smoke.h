#ifndef FALLOUT_PS2_SMOKE_H_
#define FALLOUT_PS2_SMOKE_H_

/*
 * Automated in-engine smoke harness (dev builds only).
 *
 * Enabled with -DPS2_DEV_SMOKE_TEST=ON (tools/run_smoke.sh does this). When
 * compiled in, falloutMain() hands control to the harness right after engine
 * init: the intro movies and the stock main menu loop are skipped and the
 * harness drives the real frontend entry points (main menu, character
 * selector, character editor + OSK), boots a premade character into the
 * starting map and then exercises the game:
 *
 *   PART 1  core + options + save/load
 *           frontend/OSK, map boot + [PERF] map_load_ms, UI tour
 *           (pause menu, inventory, Pip-Boy, skilldex, automap, character
 *           sheet), exhaustive preferences sweep (difficulty/skill modifiers,
 *           volumes, sliders), turn-based combat with the combat difficulty
 *           probes (AI + to-hit + damage branches), save to slot 01 with the
 *           OSK-typed save comment, map wipe + reload and state verification.
 *   PART 2  heavy stress + FPS report
 *           dense town maps tour with per-map load time, avg/min FPS and the
 *           32 MB memory budget report.
 *
 * Parts are sequential by default. Dropping an empty file named
 * PS2_SMOKE_PART1 or PS2_SMOKE_PART2 in the game data root runs only that
 * part, so a single map can be re-tested without the whole suite.
 *
 * The harness logs [SMOKE]/[PERF] lines to the EE console and terminates the
 * ELF with exit(), so tools/run_smoke.sh can grep the verdict out of the
 * emulator log without any user interaction.
 */

#ifdef PS2_DEV_SMOKE_TEST

#ifdef __cplusplus
extern "C" {
#endif

/* Called by inputGetInput() on every input poll. Advances the state machine:
 * posts synthetic input (SDL key events and pad samples) and advances timers
 * only - heavy engine work (map loads, save/load, combat) is deferred to the
 * harness driver loop in ps2_smoke.cc. */
void ps2_smoke_tick(void);

#ifdef __cplusplus
}
#endif

#ifdef __cplusplus
namespace fallout {

/* Replaces the movies + main menu flow when the harness is compiled in.
 * Never returns. */
int ps2_smoke_main(void);

} // namespace fallout
#endif

#endif /* PS2_DEV_SMOKE_TEST */

#endif /* FALLOUT_PS2_SMOKE_H_ */
