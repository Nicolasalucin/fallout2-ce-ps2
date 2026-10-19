/*
 * F6 automated smoke harness (dev builds only, see ps2_smoke.h).
 *
 * Architecture
 * ------------
 *   ps2_smoke_tick()          runs from inputGetInput(), i.e. on every input
 *                             poll of every UI loop (gameplay, main menu,
 *                             character selector/editor, preferences,
 *                             inventory, Pip-Boy, save/load, dialogue...).
 *                             It ONLY posts synthetic input (SDL key events
 *                             through the shim, pad samples through the
 *                             input_pad smoke hook) and advances timers.
 *   ps2_smoke_main()          the driver: runs the suite as a sequence of
 *                             acts. Blocking engine work (map loads,
 *                             save/load, combat) executes here, never from
 *                             inside an event pump.
 *   smoke_frame()             one gameplay frame, mirroring mainLoop().
 *
 * Verification style: the suite only passes when a real engine side effect is
 * observed - a GameMode transition for UI opens, skillGetGameDifficultyModifier
 * for the difficulty setting, the combat probes planted in combat.cc/
 * combat_ai.cc for the combat difficulty, SAVE.DAT contents (description typed
 * through the on-screen keyboard) for the save/load round trip, plus map
 * names/time/tile after reloading.
 */

#include "ps2_smoke.h"

#ifdef PS2_DEV_SMOKE_TEST

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libpad.h>

extern "C" {
#include "platform/ps2/ps2_heap.h"
}

#include "platform/ps2/ps2_input.h"
#include "platform/ps2/ps2_log.h"
#include "platform/ps2/ps2_osk.h"
#include "platform/ps2/ps2_perf.h"
#include "platform/ps2/ps2_save.h"
#include "platform/ps2/ps2_time.h"
#include "platform/ps2/sdl/ps2_sdl_internal.h"

#include "character_selector.h"
#include "color.h"
#include "combat.h"
#include "combat_defs.h"
#include "critter.h"
#include "db.h"
#include "fps_limiter.h"
#include "game.h"
#include "game_config.h"
#include "game_mouse.h"
#include "game_sound.h"
#include "input.h"
#include "kb.h"
#include "loadsave.h"
#include "mainmenu.h"
#include "map.h"
#include "mouse.h"
#include "object.h"
#include "obj_types.h"
#include "palette.h"
#include "platform_compat.h"
#include "preferences.h"
#include "random.h"
#include "scripts.h"
#include "sfall_global_scripts.h"
#include "settings.h"
#include "skill.h"
#include "skill_defs.h"
#include "svga.h"
#include "tile.h"
#include "window_manager.h"
#include "worldmap.h"

namespace fallout {

/* Diagnostics exported by platform/ps2/memory_manager_ps2.cc without a
 * public header (same pattern as ps2_mem_print_stats in ps2_heap.h). */
int ps2_mem_get_usage_kb(void);
int ps2_mem_get_peak_kb(void);
int ps2_mem_get_oom_count(void);

/* pipboy.cc dev hook, PS2_DEV_SMOKE_TEST only: makes the Pip-Boy UI
 * reachable in a temple-start game (no vault suit movie seen). */
void ps2_smoke_pipboy_set_available(void);

/* combat_ai.cc dev hook, PS2_DEV_SMOKE_TEST only: pins the AI packet's
 * called-shot frequency so the combat-difficulty branch it guards is
 * reached deterministically during the scripted battle. */
int ps2_smoke_ai_set_called_freq(Object* critter, int freq);

/* Defined in game_sound.cc but missing from game_sound.h. */
int ps2_smoke_sound_effects_get_volume(void);

/* ---- Logging / failure bookkeeping ---- */

#define SMOKE_LOG(...)                            \
    do {                                          \
        printf("[SMOKE] " __VA_ARGS__);           \
        printf("\n");                             \
        fflush(stdout);                           \
    } while (0)

#define SMOKE_PERF(...)                           \
    do {                                          \
        printf("[PERF] " __VA_ARGS__);            \
        printf("\n");                             \
        fflush(stdout);                           \
    } while (0)

static int s_fail_count;
static char s_first_failure[160];

static void smoke_fail(const char* fmt, ...)
{
    char buf[160];
    va_list args;

    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    if (s_fail_count == 0) {
        strncpy(s_first_failure, buf, sizeof(s_first_failure) - 1);
        s_first_failure[sizeof(s_first_failure) - 1] = '\0';
    }
    s_fail_count++;
    SMOKE_LOG("FAIL: %s", buf);
}

#define SMOKE_ACT_BEGIN() const int smokeFails0 = s_fail_count
#define SMOKE_ACT_OK() (s_fail_count == smokeFails0)

/* ---- Limits ---- */

#define SMOKE_STEP_TIMEOUT_MS 25000u /* default per-step stall timeout */
#define SMOKE_SLOW_STEP_TIMEOUT_MS 60000u /* save/load (memory card I/O) */
#define SMOKE_RUN_TIMEOUT_MS 480000u /* hard watchdog for the whole run */
#define SMOKE_UI_DWELL_MS 1400u
#define SMOKE_MEM_PERIOD_MS 15000u

#define SMOKE_TEMPLE_MAP "artemple.map"
#define SMOKE_WIPE_MAP "arvillag.map"

static const char* const s_part2_maps[] = {
    "denbus1.map",
    "klamall.map",
    "newr1.map",
    "ncrent.map",
    "sftanker.map",
};

/* ---- Harness state ---- */

static bool s_active;
static bool s_run_part1 = true;
static bool s_run_part2 = true;

/* PS2_SMOKE_FAST marker: run only the frontend -> boot -> save/load spine,
 * skipping the UI tour, preferences sweep and combat acts. Used to bisect
 * crashes that depend on state built up by the earlier acts. */
static bool s_fast_mode = false;
static unsigned int s_run_start;

static const char* s_step_name = "boot";
static unsigned int s_step_start;
static unsigned int s_step_deadline;
static bool s_step_active;
static bool s_step_expired;

/* UI open latency table */
#define SMOKE_UI_MAX 16
static struct {
    const char* name;
    int ms;
} s_ui_times[SMOKE_UI_MAX];
static int s_ui_time_count;

/* FPS accumulators (whole run) */
static unsigned int s_fps_last;
static unsigned int s_fps_frames;
static unsigned long long s_fps_sum_ms;
static unsigned int s_fps_max_ms;
static char s_fps_worst[32];

/* OSK observation */
static bool s_osk_was_open;
static int s_osk_open_count;
static char s_osk_name_close[64];

static unsigned int s_mem_next_report;

/* Combat difficulty phases */
#define SMOKE_CBT_PHASES 3
/* A phase ends when it produced to-hit probes (SMOKE_CBT_PHASE_READS) and at
 * least SMOKE_CBT_PHASE_MIN_MS elapsed, or at the phase's max at the latest:
 * the engine's paced AI turns run through delay_ms() without polling input,
 * so a wall-clock-only window can miss the probes entirely. The last phase
 * gets a longer budget because the battle is wound down right after it. */
#define SMOKE_CBT_PHASE_MIN_MS 2500u
#define SMOKE_CBT_PHASE_MAX_MS 12000u
#define SMOKE_CBT_PHASE_LAST_MAX_MS 45000u
#define SMOKE_CBT_PHASE_READS 2
/* SPACE taps ending the player's turn (one every 3 s, ~72 s of coverage). */
#define SMOKE_CBT_SPACE_STEPS 24
static bool s_cbt_active;
static int s_cbt_phase;
static unsigned int s_cbt_phase_at;
static int s_cbt_reads_before;
static struct {
    int reads;
    int diff;
    int mod;
} s_cbt_report[SMOKE_CBT_PHASES];

/* Tour safety net (part 2): bail out of unscripted combat/dialogue */
static bool s_tour_safety;
static unsigned int s_tour_safety_at;

/* ---- Synthetic input script ---- */

enum {
    SMOKE_IN_KEY = 1, /* tap an SDL scancode (down now, up +60 ms) */
    SMOKE_IN_PAD = 2, /* tap PAD_* bits (press now, release +90 ms) */
};

typedef struct SmokeStep {
    unsigned int at; /* ms after script arm */
    int kind;
    unsigned int arg;
} SmokeStep;

#define SMOKE_SCRIPT_MAX 32
static SmokeStep s_script[SMOKE_SCRIPT_MAX];
static int s_script_len;
static int s_script_pos;
static unsigned int s_script_base;

#define SMOKE_RELEASE_MAX 8
static struct {
    int scancode;
    unsigned int at;
} s_releases[SMOKE_RELEASE_MAX];
static int s_release_len;

static bool s_pad_down;
static unsigned int s_pad_release_at;

static void smoke_key_tap_now(unsigned int now, int scancode)
{
    ps2_sdl_post_key(scancode, 1);
    if (s_release_len < SMOKE_RELEASE_MAX) {
        s_releases[s_release_len].scancode = scancode;
        s_releases[s_release_len].at = now + 60;
        s_release_len++;
    }
}

static void smoke_pad_tap_now(unsigned int now, unsigned int buttons)
{
    if (ps2_input_smoke_release_pending()) {
        SMOKE_LOG("input: pad tap skipped (release still pending)");
        return;
    }
    ps2_input_smoke_press(buttons);
    s_pad_down = true;
    s_pad_release_at = now + 90;
}

static void smoke_script_arm(const SmokeStep* steps, int count)
{
    if (count > SMOKE_SCRIPT_MAX) {
        count = SMOKE_SCRIPT_MAX;
    }
    memcpy(s_script, steps, sizeof(SmokeStep) * count);
    s_script_len = count;
    s_script_pos = 0;
    s_script_base = ps2_time_ms();
}

static void smoke_script_clear(void)
{
    s_script_len = 0;
    s_script_pos = 0;
}

static void smoke_input_pump(unsigned int now)
{
    int i;

    /* Deferred key releases. */
    for (i = 0; i < s_release_len;) {
        if ((int)(now - s_releases[i].at) >= 0) {
            ps2_sdl_post_key(s_releases[i].scancode, 0);
            s_releases[i] = s_releases[--s_release_len];
        } else {
            i++;
        }
    }

    /* Deferred pad release. */
    if (s_pad_down && (int)(now - s_pad_release_at) >= 0) {
        ps2_input_smoke_release();
        s_pad_down = false;
    }

    /* Scripted steps that are due. */
    while (s_script_pos < s_script_len && (int)(now - (s_script_base + s_script[s_script_pos].at)) >= 0) {
        SmokeStep* st = &s_script[s_script_pos++];
        switch (st->kind) {
        case SMOKE_IN_KEY:
            smoke_key_tap_now(now, (int)st->arg);
            break;
        case SMOKE_IN_PAD:
            smoke_pad_tap_now(now, st->arg);
            break;
        default:
            break;
        }
    }
}

/* ---- Watches ---- */

typedef struct SmokeUiWatch {
    const char* name;
    int mode;
    unsigned int t0;
    int open_ms;
    bool armed;
    bool saw;
} SmokeUiWatch;

static SmokeUiWatch s_ui;

static void smoke_ui_watch_begin(const char* name, int mode)
{
    s_ui.name = name;
    s_ui.mode = mode;
    s_ui.t0 = ps2_time_ms();
    s_ui.open_ms = -1;
    s_ui.armed = true;
    s_ui.saw = false;
}

static void smoke_ui_report(const char* name, int ms)
{
    int i;

    for (i = 0; i < s_ui_time_count; i++) {
        if (compat_stricmp(s_ui_times[i].name, name) == 0) {
            s_ui_times[i].ms = ms;
            return;
        }
    }
    if (s_ui_time_count < SMOKE_UI_MAX) {
        s_ui_times[s_ui_time_count].name = name;
        s_ui_times[s_ui_time_count].ms = ms;
        s_ui_time_count++;
    }
}

static bool smoke_ui_watch_finish(const char* name)
{
    s_ui.armed = false;
    if (!s_ui.saw) {
        smoke_fail("ui %s: never entered its game mode", name);
        return false;
    }
    smoke_ui_report(name, s_ui.open_ms);
    SMOKE_LOG("ui %s: open=%d ms (target < 150 ms)", name, s_ui.open_ms);
    if (s_ui.open_ms > 3000) {
        smoke_fail("ui %s: open latency %d ms", name, s_ui.open_ms);
    } else if (s_ui.open_ms > 150) {
        SMOKE_LOG("WARN: ui %s open %d ms above the 150 ms target", name, s_ui.open_ms);
    }
    return true;
}

static void smoke_ui_watch_tick(unsigned int now)
{
    if (!s_ui.armed || s_ui.saw) {
        return;
    }
    if (s_ui.mode >= 0 && GameMode::isInGameMode(s_ui.mode)) {
        s_ui.saw = true;
        s_ui.open_ms = (int)(now - s_ui.t0);
    }
}

static void smoke_osk_tick(void)
{
    bool open = ps2_osk_is_open() != 0;

    if (open && !s_osk_was_open) {
        s_osk_open_count++;
        SMOKE_LOG("osk: opened (#%d)", s_osk_open_count);
    } else if (!open && s_osk_was_open) {
        if (gDude != nullptr) {
            const char* name = critterGetName(gDude);
            if (name != nullptr) {
                strncpy(s_osk_name_close, name, sizeof(s_osk_name_close) - 1);
                s_osk_name_close[sizeof(s_osk_name_close) - 1] = '\0';
            }
        }
        SMOKE_LOG("osk: closed (dude-name='%s')", s_osk_name_close);
    }

    s_osk_was_open = open;
}

static void smoke_fps_tick(unsigned int now)
{
    unsigned int delta;

    if (s_fps_last == 0) {
        s_fps_last = now;
        return;
    }

    delta = now - s_fps_last;
    s_fps_last = now;

    if (delta == 0 || delta > 1000) {
        return;
    }

    s_fps_frames++;
    s_fps_sum_ms += delta;
    if (delta > s_fps_max_ms) {
        s_fps_max_ms = delta;
        strncpy(s_fps_worst, s_step_active ? s_step_name : "boot", sizeof(s_fps_worst) - 1);
        s_fps_worst[sizeof(s_fps_worst) - 1] = '\0';
    }
}

static int smoke_fps_avg_ms(void)
{
    if (s_fps_frames == 0) {
        return 0;
    }
    return (int)(s_fps_sum_ms / s_fps_frames);
}

/* ---- Steps and watchdog ---- */

static void smoke_step_begin2(const char* name, unsigned int timeout_ms)
{
    s_step_name = name;
    s_step_start = ps2_time_ms();
    s_step_deadline = s_step_start + timeout_ms;
    s_step_active = true;
    s_step_expired = false;
    ps2_perf_ui_context(name);
    SMOKE_LOG("---- %s ----", name);
    ps2_mem_print_stats();
}

static void smoke_step_begin(const char* name)
{
    smoke_step_begin2(name, SMOKE_STEP_TIMEOUT_MS);
}

static void smoke_step_end(void)
{
    s_step_active = false;
    smoke_script_clear();
}

static void smoke_final_verdict(void);

static void smoke_watchdog_tick(unsigned int now)
{
    if (!s_active) {
        return;
    }

    if ((int)(now - s_run_start) > (int)SMOKE_RUN_TIMEOUT_MS) {
        smoke_fail("suite watchdog: %u ms elapsed", (unsigned)(now - s_run_start));
        smoke_final_verdict();
    }

    if (s_step_active && !s_step_expired && (int)(now - s_step_deadline) >= 0) {
        SmokeStep esc[] = {
            { 200, SMOKE_IN_KEY, SDL_SCANCODE_ESCAPE },
            { 600, SMOKE_IN_KEY, SDL_SCANCODE_ESCAPE },
            { 1000, SMOKE_IN_KEY, SDL_SCANCODE_ESCAPE },
            { 1400, SMOKE_IN_KEY, SDL_SCANCODE_ESCAPE },
        };
        s_step_expired = true;
        smoke_fail("step '%s' timed out", s_step_name);
        smoke_script_arm(esc, 4);
        return;
    }

    if (s_step_expired && (int)(now - s_step_deadline) >= (int)SMOKE_STEP_TIMEOUT_MS + 15000) {
        SMOKE_LOG("stalled step '%s' is unrecoverable, aborting", s_step_name);
        smoke_final_verdict();
    }

    if ((int)(now - s_mem_next_report) >= 0) {
        s_mem_next_report = now + SMOKE_MEM_PERIOD_MS;
        ps2_mem_print_stats();
    }
}

static void smoke_combat_tick(unsigned int now)
{
    static const int diffs[SMOKE_CBT_PHASES] = {
        COMBAT_DIFFICULTY_EASY,
        COMBAT_DIFFICULTY_NORMAL,
        COMBAT_DIFFICULTY_HARD,
    };

    int elapsed;
    int reads;

    if (!s_cbt_active || s_cbt_phase > SMOKE_CBT_PHASES) {
        return;
    }

    elapsed = (int)(now - s_cbt_phase_at);

    if (s_cbt_phase == SMOKE_CBT_PHASES) {
        /* All phases observed; the battle can be wound down. */
        SMOKE_LOG("combat: forcing the battle to end");
        _game_user_wants_to_quit = 1;
        s_cbt_phase++;
        return;
    }

    reads = ps2_perf_cbt_count(PS2_PERF_CBT_TOHIT) - s_cbt_reads_before;

    if (elapsed < (int)SMOKE_CBT_PHASE_MIN_MS
        || (reads < SMOKE_CBT_PHASE_READS
            && elapsed < (int)(s_cbt_phase == SMOKE_CBT_PHASES - 1
                    ? SMOKE_CBT_PHASE_LAST_MAX_MS
                    : SMOKE_CBT_PHASE_MAX_MS))) {
        return;
    }

    s_cbt_report[s_cbt_phase].reads = reads;
    s_cbt_report[s_cbt_phase].diff = ps2_perf_cbt_difficulty_seen(PS2_PERF_CBT_TOHIT);
    s_cbt_report[s_cbt_phase].mod = ps2_perf_cbt_mod(PS2_PERF_CBT_TOHIT);
    s_cbt_reads_before = ps2_perf_cbt_count(PS2_PERF_CBT_TOHIT);

    s_cbt_phase++;
    s_cbt_phase_at = now;

    if (s_cbt_phase < SMOKE_CBT_PHASES) {
        settings.preferences.combat_difficulty = diffs[s_cbt_phase];
        SMOKE_LOG("combat: phase %d, difficulty %d (reads %d, %d ms)",
            s_cbt_phase, diffs[s_cbt_phase], reads, elapsed);
    }
}

static void smoke_tour_safety_tick(unsigned int now)
{
    if (!s_tour_safety || (int)(now - s_tour_safety_at) < 0) {
        return;
    }

    s_tour_safety_at = now + 1500;

    if (ps2_platform_in_combat() != 0) {
        SMOKE_LOG("tour: combat started by map scripts, ending it");
        _game_user_wants_to_quit = 1;
    }

    if (GameMode::isInGameMode(GameMode::kDialog)) {
        SMOKE_LOG("tour: dialogue opened by map scripts, dismissing");
        smoke_key_tap_now(now, SDL_SCANCODE_ESCAPE);
    }
}

/* ---- Frames and waits ---- */

static void smoke_frame(void)
{
    sharedFpsLimiter.mark();

    int keyCode = inputGetInput();

    sfall_gl_scr_process_main();
    gameHandleKey(keyCode, false);
    scriptsHandleRequests();
    mapHandleTransition();

    if (gDude != nullptr && (gDude->data.critter.combat.results & (DAM_DEAD | DAM_KNOCKED_OUT)) != 0) {
        smoke_fail("dude is dead or knocked out");
    }

    renderPresent();
    sharedFpsLimiter.throttle();
}

static bool smoke_frame_ok(void)
{
    smoke_frame();
    return !s_step_expired;
}

static bool smoke_wait_ms(unsigned int ms)
{
    unsigned int start = ps2_time_ms();

    while ((int)(ps2_time_ms() - start) < (int)ms) {
        if (!smoke_frame_ok()) {
            return false;
        }
    }
    return true;
}

/* Post a key tap and run frames until the watched UI opened (the tick latches
 * the mode) and closed (the close script runs inside the modal loop). The
 * close script is re-armed on every attempt: a tap that opens a different
 * modal (e.g. an availability dialog) still has an escape route. */
static bool smoke_ui_tap(const char* name, int scancode, int mode)
{
    int attempt;

    smoke_ui_watch_begin(name, mode);

    for (attempt = 0; attempt < 3 && !s_ui.saw; attempt++) {
        SmokeStep closeStep[] = {
            { SMOKE_UI_DWELL_MS, SMOKE_IN_KEY, SDL_SCANCODE_ESCAPE },
        };

        smoke_script_arm(closeStep, 1);
        smoke_key_tap_now(ps2_time_ms(), scancode);

        if (!smoke_frame_ok()) {
            smoke_script_clear();
            return false;
        }
    }

    smoke_script_clear();
    return smoke_ui_watch_finish(name);
}

/* ---- Part selection ---- */

static bool smoke_marker_exists(const char* name)
{
    File* stream = fileOpen(name, "rb");

    if (stream == nullptr) {
        return false;
    }

    fileClose(stream);
    return true;
}

static void smoke_select_parts(void)
{
    bool m1 = smoke_marker_exists("PS2_SMOKE_PART1");
    bool m2 = smoke_marker_exists("PS2_SMOKE_PART2");
    bool fast = smoke_marker_exists("PS2_SMOKE_FAST");

    if (m1 || m2) {
        s_run_part1 = m1;
        s_run_part2 = m2;
    } else {
        s_run_part1 = true;
        s_run_part2 = true;
    }

    if (fast) {
        s_fast_mode = true;
        s_run_part1 = true;
        s_run_part2 = false;
    }

    SMOKE_LOG("suite: part1=%d part2=%d fast=%d (markers: part1=%d part2=%d fast=%d)",
        s_run_part1, s_run_part2, s_fast_mode, m1, m2, fast);

    /* PS2_SMOKE_VERBOSE turns on the port's per-open/per-remap logs
     * (ps2_log_set_verbose, off by default) for crash triage. Fast runs
     * enable it automatically: they exist to debug exactly that. */
    if (fast || smoke_marker_exists("PS2_SMOKE_VERBOSE")) {
        ps2_log_set_verbose(1);
        SMOKE_LOG("suite: verbose port logging enabled");
    }
}

/* ---- Act 1: frontend (main menu, preferences, character selector + OSK) ---- */

static bool smoke_act_frontend(void)
{
    SMOKE_ACT_BEGIN();

    if (mainMenuWindowInit() != 0) {
        smoke_fail("frontend: mainMenuWindowInit failed");
        return false;
    }
    SMOKE_LOG("frontend: main menu reached");
    ps2_perf_ui_context("main-menu");

    /* Options screen through the real menu entry point. */
    {
        SmokeStep steps[] = {
            { 700, SMOKE_IN_KEY, SDL_SCANCODE_O },
        };

        keyboardReset();
        mainMenuWindowUnhide(1);
        mouseShowCursor();
        smoke_script_arm(steps, 1);
        int rc = mainMenuWindowHandleEvents();
        mouseHideCursor();
        smoke_script_clear();

        if (rc != MAIN_MENU_OPTIONS) {
            smoke_fail("frontend: menu rc=%d, expected OPTIONS", rc);
            return false;
        }

        mainMenuWindowHide(true);

        SmokeStep closeStep[] = {
            { 1800, SMOKE_IN_KEY, SDL_SCANCODE_ESCAPE },
        };
        smoke_ui_watch_begin("prefs", GameMode::kPreferences);
        smoke_script_arm(closeStep, 1);
        int prc = doPreferences(true);
        smoke_script_clear();
        smoke_ui_watch_finish("prefs");
        if (prc != 0) {
            smoke_fail("frontend: doPreferences rc=%d", prc);
        }
    }

    /* New game -> character selector -> creation editor (name prompt drives
     * the OSK) -> abort editor -> take the premade. */
    {
        SmokeStep steps[] = {
            { 700, SMOKE_IN_KEY, SDL_SCANCODE_N },
        };

        keyboardReset();
        mainMenuWindowUnhide(1);
        mouseShowCursor();
        smoke_script_arm(steps, 1);
        int rc = mainMenuWindowHandleEvents();
        mouseHideCursor();
        smoke_script_clear();

        if (rc != MAIN_MENU_NEW_GAME) {
            smoke_fail("frontend: menu rc=%d, expected NEW_GAME", rc);
            return false;
        }

        mainMenuWindowHide(true);
        mainMenuWindowFree();

        SmokeStep selectorSteps[] = {
            { 1200, SMOKE_IN_KEY, SDL_SCANCODE_C }, /* creation editor */
            { 2600, SMOKE_IN_KEY, SDL_SCANCODE_N }, /* name prompt -> OSK */
            { 3800, SMOKE_IN_PAD, PAD_CROSS }, /* OSK: type a character */
            { 4600, SMOKE_IN_PAD, PAD_START }, /* OSK: submit (Return) */
            { 5800, SMOKE_IN_KEY, SDL_SCANCODE_ESCAPE }, /* abort editor */
            { 7000, SMOKE_IN_KEY, SDL_SCANCODE_T }, /* take premade */
        };
        int osk_before = s_osk_open_count;
        int src;

        smoke_script_arm(selectorSteps, 6);
        src = characterSelectorOpen();
        smoke_script_clear();

        if (src != 2) {
            /* The editor/OSK path is timing sensitive in the emulator; give
             * the premade a second, simpler chance so the rest of the suite
             * still runs (the failure is already recorded). */
            smoke_fail("frontend: character selector rc=%d, expected 2", src);
            SmokeStep fallback[] = {
                { 1200, SMOKE_IN_KEY, SDL_SCANCODE_T },
            };
            smoke_script_arm(fallback, 1);
            src = characterSelectorOpen();
            smoke_script_clear();
        }

        if (src != 2) {
            smoke_fail("frontend: character selector failed twice (rc=%d)", src);
            return false;
        }

        if (s_osk_open_count <= osk_before) {
            smoke_fail("frontend: OSK never opened for the editor name prompt");
        } else {
            SMOKE_LOG("frontend: OSK opens=%d, typed name='%s'",
                s_osk_open_count - osk_before, s_osk_name_close);
        }

        SMOKE_LOG("frontend: premade '%s' taken", critterGetName(gDude));
    }

    return SMOKE_ACT_OK();
}

/* ---- Act 2: boot the game (mirrors main.cc _main_load_new) ---- */

static bool smoke_act_boot_game(void)
{
    SMOKE_ACT_BEGIN();

    smoke_step_begin2("boot", SMOKE_SLOW_STEP_TIMEOUT_MS);

    /* Mirrors the NEW_GAME branch of main.cc's menu loop. */
    randomSeedPrerandom(-1);

    _game_user_wants_to_quit = 0;
    gDude->flags &= ~OBJECT_FLAT;
    objectShow(gDude, nullptr);
    mouseHideCursor();

    int win = windowCreate(0, 0, screenGetWidth(), screenGetHeight(), _colorTable[0], WINDOW_MODAL | WINDOW_MOVE_ON_TOP);
    windowRefresh(win);

    colorPaletteLoad("color.pal");
    paletteFadeTo(_cmap);
    _map_init();
    gameMouseSetCursor(MOUSE_CURSOR_NONE);
    mouseShowCursor();

    char mapName[] = SMOKE_TEMPLE_MAP;
    int rc = mapLoadByName(mapName);
    if (rc != 0) {
        smoke_fail("boot: mapLoadByName(%s) rc=%d", SMOKE_TEMPLE_MAP, rc);
    }

    {
        int loadMs = ps2_perf_map_load_last_ms();
        SMOKE_PERF("map-load %s %d ms (target < 1500-2500 ms)", gMapHeader.name, loadMs);
        if (loadMs > 8000) {
            smoke_fail("boot: map load took %d ms", loadMs);
        } else if (loadMs > 2500) {
            SMOKE_LOG("WARN: boot map load %d ms above the 2500 ms target", loadMs);
        }
    }

    wmMapMusicStart();
    paletteFadeTo(gPaletteWhite);
    windowDestroy(win);
    colorPaletteLoad("color.pal");
    paletteFadeTo(_cmap);

    sfall_gl_scr_exec_start_proc();

    SMOKE_LOG("boot: %s loaded (%s, %d KB used)", gMapHeader.name, critterGetName(gDude), ps2_mem_get_usage_kb());

    return SMOKE_ACT_OK();
}

/* ---- Act 3: map / engine state verification ---- */

static bool smoke_act_map_verify(void)
{
    SMOKE_ACT_BEGIN();

    if (!smoke_wait_ms(1200)) {
        return false;
    }

    if (gDude == nullptr) {
        smoke_fail("map: no dude after boot");
        return false;
    }

    if (compat_stricmp(gMapHeader.name, "ARTEMPLE.MAP") != 0) {
        smoke_fail("map: expected ARTEMPLE.MAP, got '%s'", gMapHeader.name);
    }

    if (gDude->tile < 0) {
        smoke_fail("map: invalid dude tile %d", gDude->tile);
    }

    unsigned int t = gameTimeGetTime();
    if (t == 0) {
        smoke_fail("map: game time is still 0");
    }

    if (ps2_platform_in_combat() != 0) {
        smoke_fail("map: unexpected combat right after boot");
    }

    SMOKE_LOG("map: %s tile=%d elevation=%d time=%u dude='%s'",
        gMapHeader.name, gDude->tile, gDude->elevation, t, critterGetName(gDude));

    return SMOKE_ACT_OK();
}

/* ---- Act 4: in-game UI tour ---- */

static bool smoke_act_ui_tour(void)
{
    SMOKE_ACT_BEGIN();
    smoke_step_begin2("ui-tour", 45000);

    /* Pause menu (in-game options bar), closes with ESC. */
    if (!smoke_ui_tap("pause-menu", SDL_SCANCODE_ESCAPE, GameMode::kOptions)) {
        return false;
    }
    if (!smoke_ui_tap("inventory", SDL_SCANCODE_I, GameMode::kInventory)) {
        return false;
    }

    /* The temple-start game has no vault suit movie seen, so make the Pip-Boy
     * reachable through the dev hook before opening it. */
    ps2_smoke_pipboy_set_available();
    if (!smoke_ui_tap("pipboy", SDL_SCANCODE_P, GameMode::kPipboy)) {
        return false;
    }
    if (!smoke_ui_tap("skilldex", SDL_SCANCODE_S, GameMode::kSkilldex)) {
        return false;
    }
    if (!smoke_ui_tap("automap", SDL_SCANCODE_TAB, GameMode::kAutomap)) {
        return false;
    }
    if (!smoke_ui_tap("charsheet", SDL_SCANCODE_C, GameMode::kEditor)) {
        return false;
    }

    return SMOKE_ACT_OK();
}

/* ---- Act 5: exhaustive preferences sweep ---- */

/* true when `token` (e.g. "master_volume=11111") appears in the on-disk
 * fallout2.cfg, proving settingsSave() really wrote the sweep. */
static bool smoke_config_contains(const char* token)
{
    static char buf[65536];
    File* stream = fileOpen("fallout2.cfg", "rb");

    if (stream == nullptr) {
        return false;
    }

    size_t len = fileRead(buf, 1, sizeof(buf) - 1, stream);
    fileClose(stream);
    buf[len] = '\0';

    return strstr(buf, token) != nullptr;
}

static bool smoke_act_options_sweep(void)
{
    SMOKE_ACT_BEGIN();
    PreferencesSettings saved = settings.preferences;

    /* Game difficulty: verified through the real skill modifier path. */
    {
        static const int diffs[3] = {
            GAME_DIFFICULTY_EASY,
            GAME_DIFFICULTY_NORMAL,
            GAME_DIFFICULTY_HARD,
        };
        static const int expected[3] = { 20, 0, -10 };
        int i;

        for (i = 0; i < 3; i++) {
            settings.preferences.game_difficulty = diffs[i];

            int mod = skillGetGameDifficultyModifier(SKILL_LOCKPICK);
            if (mod != expected[i]) {
                smoke_fail("options: game_difficulty=%d lockpick mod %d, expected %d", diffs[i], mod, expected[i]);
            }

            if (skillGetGameDifficultyModifier(SKILL_SMALL_GUNS) != 0) {
                smoke_fail("options: game_difficulty=%d affected a combat skill", diffs[i]);
            }

            SMOKE_LOG("options: game_difficulty=%d -> lockpick mod %d", diffs[i], mod);
        }
    }

    /* Combat difficulty readback (real effect is verified in combat). */
    {
        int v;

        for (v = 0; v <= 2; v++) {
            settings.preferences.combat_difficulty = v;
            if (settings.preferences.combat_difficulty != v) {
                smoke_fail("options: combat_difficulty readback %d != %d", settings.preferences.combat_difficulty, v);
            }
        }
    }

    /* Violence level. */
    {
        int v;

        for (v = 0; v <= VIOLENCE_LEVEL_MAXIMUM_BLOOD; v++) {
            settings.preferences.violence_level = v;
            if (settings.preferences.violence_level != v) {
                smoke_fail("options: violence_level readback failed for %d", v);
            }
        }
    }

    /* Target highlight. */
    {
        int v;

        for (v = TARGET_HIGHLIGHT_OFF; v <= TARGET_HIGHLIGHT_TARGETING_ONLY; v++) {
            settings.preferences.target_highlight = v;
            if (settings.preferences.target_highlight != v) {
                smoke_fail("options: target_highlight readback failed for %d", v);
            }
        }
    }

    /* Sliders that live purely in settings. */
    {
        int v;

        for (v = 0; v <= 5; v++) {
            settings.preferences.combat_speed = v;
            if (settings.preferences.combat_speed != v) {
                smoke_fail("options: combat_speed readback failed for %d", v);
            }
        }
    }

    settings.preferences.text_base_delay = 5.0;
    if (settings.preferences.text_base_delay != 5.0) {
        smoke_fail("options: text_base_delay readback failed");
    }
    settings.preferences.text_line_delay = 2.5;
    settings.preferences.mouse_sensitivity = 2.0;
    if (settings.preferences.mouse_sensitivity != 2.0) {
        smoke_fail("options: mouse_sensitivity readback failed");
    }

    /* Booleans: toggle each one and read it back. */
    {
        struct {
            const char* name;
            bool* field;
        } bools[] = {
            { "item_highlight", &settings.preferences.item_highlight },
            { "combat_looks", &settings.preferences.combat_looks },
            { "combat_messages", &settings.preferences.combat_messages },
            { "combat_taunts", &settings.preferences.combat_taunts },
            { "language_filter", &settings.preferences.language_filter },
            { "running", &settings.preferences.running },
            { "subtitles", &settings.preferences.subtitles },
            { "player_speedup", &settings.preferences.player_speedup },
            { "running_burning_guy", &settings.preferences.running_burning_guy },
        };
        int i;

        for (i = 0; i < (int)(sizeof(bools) / sizeof(bools[0])); i++) {
            *bools[i].field = true;
            if (*bools[i].field != true) {
                smoke_fail("options: %s readback failed (true)", bools[i].name);
            }
            *bools[i].field = false;
            if (*bools[i].field != false) {
                smoke_fail("options: %s readback failed (false)", bools[i].name);
            }
        }
    }

    /* Brightness: the engine adjuster must move the stored value. */
    {
        double b0 = settings.preferences.brightness;
        brightnessIncrease();
        double b1 = settings.preferences.brightness;
        brightnessIncrease();
        double b2 = settings.preferences.brightness;
        brightnessDecrease();
        double b3 = settings.preferences.brightness;
        brightnessDecrease();
        double b4 = settings.preferences.brightness;

        SMOKE_LOG("options: brightness %.3f -> %.3f -> %.3f -> %.3f -> %.3f", b0, b1, b2, b3, b4);

        if (!(b1 >= b0 && b2 >= b1 && b3 <= b2 && b4 <= b3)) {
            smoke_fail("options: brightness adjuster not monotonic");
        }
        if (b1 == b0 && b2 == b1) {
            smoke_fail("options: brightness adjuster did not move the value");
        }
    }

    /* Volumes: the engine setters drive the audio layer through their own
     * statics (settings.sound.* is only written by the preferences UI commit),
     * so verify through the engine getters, then exercise the config
     * persistence path the preferences UI uses. */
    SoundSettings savedSound = settings.sound;
    {
        int r1 = gameSoundSetMasterVolume(11111);
        int r2 = soundEffectsSetVolume(13333);
        backgroundSoundSetVolume(15555);
        speechSetVolume(17777);

        int g1 = gameSoundGetMasterVolume();
        int g2 = ps2_smoke_sound_effects_get_volume();
        int g3 = backgroundSoundGetVolume();
        int g4 = speechGetVolume();

        SMOKE_LOG("options: volumes master=%d sfx=%d music=%d speech=%d (rc %d/%d)",
            g1, g2, g3, g4, r1, r2);

        if (r1 != 0 || g1 != 11111) {
            smoke_fail("options: master volume not applied (rc %d, got %d)", r1, g1);
        }
        if (r2 != 0 || g2 != 13333) {
            smoke_fail("options: SFX volume not applied (rc %d, got %d)", r2, g2);
        }
        if (g3 != 15555) {
            smoke_fail("options: music volume not applied (got %d)", g3);
        }
        if (g4 != 17777) {
            smoke_fail("options: speech volume not applied (got %d)", g4);
        }

        settings.sound.master_volume = 11111;
        settings.sound.sndfx_volume = 13333;
        settings.sound.music_volume = 15555;
        settings.sound.speech_volume = 17777;
    }

    /* Persist the sweep, proving fallout2.cfg receives the new values. */
    if (!settingsSave()) {
        smoke_fail("options: settingsSave() failed");
    } else if (!smoke_config_contains("master_volume=11111")) {
        smoke_fail("options: master_volume=11111 not found in fallout2.cfg");
    } else {
        SMOKE_LOG("options: sweep persisted to fallout2.cfg");
    }

    /* Restore everything the sweep touched, then persist the stock values. */
    settings.preferences = saved;
    settings.sound = savedSound;

    gameSoundSetMasterVolume(settings.sound.master_volume);
    soundEffectsSetVolume(settings.sound.sndfx_volume);
    backgroundSoundSetVolume(settings.sound.music_volume);
    speechSetVolume(settings.sound.speech_volume);

    if (!settingsSave()) {
        smoke_fail("options: second settingsSave() failed");
    }

    return SMOKE_ACT_OK();
}

/* ---- Act 6: turn-based combat with difficulty probes ---- */

static Object* smoke_find_fight_candidate(void)
{
    Object* best = nullptr;
    int bestDistance = 1 << 30;
    Object* obj;
    int dudeX = gDude->tile % 200;
    int dudeY = gDude->tile / 200;

    for (obj = objectFindFirst(); obj != nullptr; obj = objectFindNext()) {
        if (obj == gDude) {
            continue;
        }
        if ((obj->pid >> 24) != OBJ_TYPE_CRITTER) {
            continue;
        }
        if (!critterIsActive(obj)) {
            continue;
        }

        int x = obj->tile % 200;
        int y = obj->tile / 200;
        int d = abs(x - dudeX) + abs(y - dudeY);
        if (d < bestDistance) {
            bestDistance = d;
            best = obj;
        }
    }

    return best;
}

static bool smoke_act_combat(void)
{
    SMOKE_ACT_BEGIN();
    /* One difficulty phase ~= one fully played enemy turn (the engine paces
     * it through delay_ms() without polling input, so a turn takes 10-20 s
     * of wall time under PCSX2 with no smoke ticks in between). */
    smoke_step_begin2("combat-ai", 120000);

    Object* target = smoke_find_fight_candidate();

    if (target == nullptr) {
        SMOKE_LOG("combat: no critter in %s, loading %s instead", gMapHeader.name, SMOKE_WIPE_MAP);
        char mapName[] = SMOKE_WIPE_MAP;
        if (mapLoadByName(mapName) != 0) {
            smoke_fail("combat: cannot load a map with critters");
            return false;
        }
        if (!smoke_wait_ms(1500)) {
            return false;
        }
        target = smoke_find_fight_candidate();
    }

    if (target == nullptr) {
        smoke_fail("combat: no fightable critter found");
        return false;
    }

    char targetName[64];
    strncpy(targetName, critterGetName(target), sizeof(targetName) - 1);
    targetName[sizeof(targetName) - 1] = '\0';

    int savedTeam = target->data.critter.combat.team;
    int savedTile = target->tile;
    int savedElevation = target->elevation;

    if (target->data.critter.combat.team == gDude->data.critter.combat.team) {
        target->data.critter.combat.team = (gDude->data.critter.combat.team + 1) & 0xF;
    }

    /* Put the target on the dude's tile so the AI has a melee target and
     * always acts on its turn. */
    objectSetLocation(target, gDude->tile, gDude->elevation, nullptr);
    tileWindowRefresh();

    SMOKE_LOG("combat: %s (team %d) vs dude, starting battle", targetName, target->data.critter.combat.team);

    /* Phase machine: EASY -> NORMAL -> HARD, one ended player turn per phase,
     * the battle is forced to end afterwards. SPACE ends the player turn. */
    settings.preferences.combat_difficulty = COMBAT_DIFFICULTY_EASY;

    /* The AI difficulty branch (called-shot intelligence threshold) sits
     * behind a random gate; pin it so every AI attack reaches it. */
    int savedCalledFreq = ps2_smoke_ai_set_called_freq(target, 1);

    smoke_ui_watch_begin("combat", GameMode::kCombat);
    s_cbt_active = true;
    s_cbt_phase = 0;
    s_cbt_phase_at = ps2_time_ms();
    s_cbt_reads_before = ps2_perf_cbt_count(PS2_PERF_CBT_TOHIT);
    s_cbt_report[0].reads = 0;
    s_cbt_report[1].reads = 0;
    s_cbt_report[2].reads = 0;

    /* The battle only advances into the AI's paced turn (where the probes
     * fire) when the dude's turn is ended, and every difficulty phase needs
     * at least one full AI turn. Tap SPACE on a steady cadence so the battle
     * never parks waiting for player input. */
    SmokeStep steps[SMOKE_CBT_SPACE_STEPS];
    for (int i = 0; i < SMOKE_CBT_SPACE_STEPS; i++) {
        steps[i].at = 1500 + i * 3000;
        steps[i].kind = SMOKE_IN_KEY;
        steps[i].arg = SDL_SCANCODE_SPACE;
    }
    smoke_script_arm(steps, SMOKE_CBT_SPACE_STEPS);

    CombatStartData csd = {};
    csd.attacker = gDude;
    csd.defender = target;
    _combat(&csd);

    s_cbt_active = false;
    smoke_script_clear();
    smoke_ui_watch_finish("combat");

    /* Escape the battle if it is somehow still running. */
    if (!smoke_wait_ms(600)) {
        return false;
    }
    if (ps2_platform_in_combat() != 0) {
        _game_user_wants_to_quit = 1;
        smoke_wait_ms(900);
    }
    if (ps2_platform_in_combat() != 0) {
        smoke_fail("combat: battle did not end");
    }

    /* Restore the target so the rest of the suite is undisturbed. */
    target->data.critter.combat.team = savedTeam;
    objectSetLocation(target, savedTile, savedElevation, nullptr);
    if (savedCalledFreq >= 0) {
        ps2_smoke_ai_set_called_freq(target, savedCalledFreq);
    }

    /* The difficulty probes must have fired, and the observed modifier must
     * match the difficulty that was active during the phase. */
    {
        static const int expectedMod[SMOKE_CBT_PHASES] = { -20, 0, 20 };
        static const int expectedAiMod[SMOKE_CBT_PHASES] = { 7, 5, 3 };
        static const int expectedDmgMod[SMOKE_CBT_PHASES] = { 75, 100, 125 };
        int totalReads = ps2_perf_cbt_count(PS2_PERF_CBT_TOHIT);
        int phasesWithReads = 0;
        int i;

        for (i = 0; i < SMOKE_CBT_PHASES; i++) {
            SMOKE_LOG("combat: phase %d reads=%d diff=%d mod=%d (expected diff=%d mod=%d)",
                i, s_cbt_report[i].reads, s_cbt_report[i].diff, s_cbt_report[i].mod,
                i, expectedMod[i]);

            if (s_cbt_report[i].reads <= 0) {
                continue;
            }
            phasesWithReads++;

            if (s_cbt_report[i].diff != i) {
                smoke_fail("combat: phase %d saw difficulty %d", i, s_cbt_report[i].diff);
            }
            if (s_cbt_report[i].mod != expectedMod[i]) {
                smoke_fail("combat: phase %d to-hit modifier %d, expected %d", i, s_cbt_report[i].mod, expectedMod[i]);
            }
        }

        if (totalReads == 0) {
            smoke_fail("combat: the to-hit difficulty probe never fired");
        }
        if (phasesWithReads < SMOKE_CBT_PHASES) {
            smoke_fail("combat: only %d of %d difficulty phases produced to-hit reads",
                phasesWithReads, SMOKE_CBT_PHASES);
        }

        /* AI called-shot threshold and damage scaling: every difficulty must
         * have been observed, and the last sample must carry the modifier of
         * the difficulty it was taken under. */
        {
            const unsigned int allDiffs = (1u << SMOKE_CBT_PHASES) - 1;
            unsigned int aiMask = ps2_perf_cbt_diff_mask(PS2_PERF_CBT_AI);
            unsigned int dmgMask = ps2_perf_cbt_diff_mask(PS2_PERF_CBT_DAMAGE);
            int aiDiff = ps2_perf_cbt_difficulty_seen(PS2_PERF_CBT_AI);
            int dmgDiff = ps2_perf_cbt_difficulty_seen(PS2_PERF_CBT_DAMAGE);

            SMOKE_LOG("combat: probes tohit=%d (mod %d) damage=%d (diff %d mod %d mask 0x%X) ai=%d (diff %d mod %d mask 0x%X)",
                totalReads, ps2_perf_cbt_mod(PS2_PERF_CBT_TOHIT),
                ps2_perf_cbt_count(PS2_PERF_CBT_DAMAGE), dmgDiff, ps2_perf_cbt_mod(PS2_PERF_CBT_DAMAGE), dmgMask,
                ps2_perf_cbt_count(PS2_PERF_CBT_AI), aiDiff, ps2_perf_cbt_mod(PS2_PERF_CBT_AI), aiMask);

            if (ps2_perf_cbt_count(PS2_PERF_CBT_AI) == 0) {
                smoke_fail("combat: the AI difficulty probe never fired");
            } else {
                if (aiMask != allDiffs) {
                    smoke_fail("combat: AI probe saw difficulties 0x%X, expected all of 0x%X", aiMask, allDiffs);
                }
                if (aiDiff < 0 || aiDiff >= SMOKE_CBT_PHASES
                    || ps2_perf_cbt_mod(PS2_PERF_CBT_AI) != expectedAiMod[aiDiff]) {
                    smoke_fail("combat: AI probe modifier %d does not match difficulty %d",
                        ps2_perf_cbt_mod(PS2_PERF_CBT_AI), aiDiff);
                }
            }

            if (ps2_perf_cbt_count(PS2_PERF_CBT_DAMAGE) == 0) {
                smoke_fail("combat: the damage difficulty probe never fired");
            } else {
                if (dmgMask == 0) {
                    smoke_fail("combat: damage probe recorded no difficulty");
                }
                if (dmgDiff < 0 || dmgDiff >= SMOKE_CBT_PHASES
                    || ps2_perf_cbt_mod(PS2_PERF_CBT_DAMAGE) != expectedDmgMod[dmgDiff]) {
                    smoke_fail("combat: damage probe modifier %d does not match difficulty %d",
                        ps2_perf_cbt_mod(PS2_PERF_CBT_DAMAGE), dmgDiff);
                }
            }
        }
    }

    /* Leave the difficulty at NORMAL for the rest of the suite. */
    settings.preferences.combat_difficulty = COMBAT_DIFFICULTY_NORMAL;

    return SMOKE_ACT_OK();
}

/* ---- Act 7: save / wipe / load round trip ---- */

static int smoke_read_save_description(char* out, int outSize)
{
    File* stream = fileOpen("SAVEGAME\\SLOT01\\SAVE.DAT", "rb");
    char header[61];
    char description[31];

    if (stream == nullptr) {
        return -1;
    }

    if (fileRead(header, 1, sizeof(header), stream) != sizeof(header)) {
        fileClose(stream);
        return -1;
    }

    memset(description, 0, sizeof(description));
    if (fileRead(description, 1, 30, stream) != 30) {
        fileClose(stream);
        return -1;
    }

    fileClose(stream);

    if (outSize > 0) {
        strncpy(out, description, outSize - 1);
        out[outSize - 1] = '\0';
    }

    return (int)strlen(description);
}

static bool smoke_act_save_load(void)
{
    SMOKE_ACT_BEGIN();
    smoke_step_begin2("save-load", SMOKE_SLOW_STEP_TIMEOUT_MS);

    char descBefore[31] = "";
    char descAfter[31] = "";
    int sizeBefore = smoke_read_save_description(descBefore, sizeof(descBefore));
    int oskBefore = s_osk_open_count;

    SMOKE_LOG("save: slot 01 before: size=%d description='%s'", sizeBefore, descBefore);

    /* Save into SLOT01 through the real save UI: ENTER (slot) -> overwrite
     * YES -> save comment (OSK types a character and submits with Return). */
    {
        SmokeStep steps[] = {
            { 1500, SMOKE_IN_KEY, SDL_SCANCODE_RETURN },
            { 2400, SMOKE_IN_KEY, SDL_SCANCODE_Y },
            { 3600, SMOKE_IN_PAD, PAD_CROSS },
            { 4400, SMOKE_IN_PAD, PAD_START },
            { 6000, SMOKE_IN_KEY, SDL_SCANCODE_RETURN },
            { 9000, SMOKE_IN_KEY, SDL_SCANCODE_ESCAPE },
        };

        lsgSetSlotCursor(0);
        smoke_step_begin2("save", SMOKE_SLOW_STEP_TIMEOUT_MS);
        smoke_ui_watch_begin("save-ui", GameMode::kSaveGame);
        smoke_script_arm(steps, 6);
        int rc = lsgSaveGame(LOAD_SAVE_MODE_NORMAL);
        smoke_script_clear();
        smoke_ui_watch_finish("save-ui");

        if (rc != 1) {
            smoke_fail("save: lsgSaveGame rc=%d (expected 1)", rc);
            return false;
        }
    }

    if (s_osk_open_count <= oskBefore) {
        smoke_fail("save: OSK did not open for the save comment");
    } else {
        SMOKE_LOG("save: OSK typed the save comment (opens %d -> %d)", oskBefore, s_osk_open_count);
    }

    {
        int sizeAfter = smoke_read_save_description(descAfter, sizeof(descAfter));
        if (sizeAfter <= 0) {
            smoke_fail("save: SAVE.DAT is missing in slot 01");
        } else if (strcmp(descAfter, descBefore) == 0) {
            smoke_fail("save: the OSK-typed comment was not persisted ('%s')", descAfter);
        }
        SMOKE_LOG("save: slot 01 after: size=%d description='%s'", sizeAfter, descAfter);
    }

    unsigned int savedTime = gameTimeGetTime();

    /* Wipe the live state by loading a different map, then tamper the clock. */
    {
        char mapName[] = SMOKE_WIPE_MAP;
        int rc = mapLoadByName(mapName);
        if (rc != 0) {
            smoke_fail("load: wipe mapLoadByName(%s) rc=%d", SMOKE_WIPE_MAP, rc);
            return false;
        }
        objectSetLocation(gDude, gMapHeader.enteringTile, gMapHeader.enteringElevation, nullptr);
        tileWindowRefresh();
        if (!smoke_wait_ms(900)) {
            return false;
        }
        SMOKE_LOG("load: wiped to %s (tile %d)", gMapHeader.name, gDude->tile);
    }

    unsigned int tamperedTime = gameTimeGetTime() + 36000;
    gameTimeSetTime(tamperedTime);

    /* Reload SLOT01 through the real load UI. ENTER picks the occupied slot
     * and starts the load; the late ESC only exists to unwind a stuck
     * window (the step timeout would otherwise fail the act first). */
    {
        SmokeStep steps[] = {
            { 2000, SMOKE_IN_KEY, SDL_SCANCODE_RETURN },
            { 15000, SMOKE_IN_KEY, SDL_SCANCODE_ESCAPE },
        };

        smoke_step_begin2("load", SMOKE_SLOW_STEP_TIMEOUT_MS);
        smoke_ui_watch_begin("load-ui", GameMode::kLoadGame);
        smoke_script_arm(steps, 2);
        int rc = lsgLoadGame(LOAD_SAVE_MODE_NORMAL);
        smoke_script_clear();
        smoke_ui_watch_finish("load-ui");

        if (rc == 0) {
            smoke_fail("load: cancelled (rc=0)");
            return false;
        }
    }

    if (!smoke_wait_ms(1200)) {
        return false;
    }

    {
        unsigned int loadedTime = gameTimeGetTime();

        if (gDude == nullptr) {
            smoke_fail("load: no dude after loading");
            return false;
        }

        /* The engine keeps the saved map's file name (ARTEMPLE.SAV) as the
         * map header name after loading it from the save. */
        if (compat_stricmp(gMapHeader.name, "ARTEMPLE.SAV") != 0
            && compat_stricmp(gMapHeader.name, "ARTEMPLE.MAP") != 0) {
            smoke_fail("load: map is '%s', expected ARTEMPLE(SAV|MAP)", gMapHeader.name);
        }

        if (!(loadedTime < tamperedTime)) {
            smoke_fail("load: game time not restored (%u >= tampered %u)", loadedTime, tamperedTime);
        }

        if ((gDude->data.critter.combat.results & DAM_DEAD) != 0) {
            smoke_fail("load: dude is dead after loading");
        }

        SMOKE_LOG("load: ok map=%s tile=%d time=%u (saved %u, tampered %u)",
            gMapHeader.name, gDude->tile, loadedTime, savedTime, tamperedTime);
    }

    /* Options live in the config, not in the save: the sweep values must have
     * survived the round trip. */
    if (settings.preferences.game_difficulty != GAME_DIFFICULTY_NORMAL) {
        smoke_fail("load: preferences changed across the load (game_difficulty %d)",
            settings.preferences.game_difficulty);
    }

    return SMOKE_ACT_OK();
}

/* ---- Act 8 (part 2): dense map tour + memory / FPS report ---- */

static bool smoke_act_tour(void)
{
    SMOKE_ACT_BEGIN();
    int mapCount = (int)(sizeof(s_part2_maps) / sizeof(s_part2_maps[0]));
    int i;

    s_tour_safety = true;
    s_tour_safety_at = ps2_time_ms();

    for (i = 0; i < mapCount; i++) {
        char mapName[32];
        strncpy(mapName, s_part2_maps[i], sizeof(mapName) - 1);
        mapName[sizeof(mapName) - 1] = '\0';

        smoke_step_begin2(mapName, SMOKE_SLOW_STEP_TIMEOUT_MS);

        ps2_perf_frame_reset();

        int before = ps2_perf_map_load_last_ms();
        int rc = mapLoadByName(mapName);
        int loadMs = ps2_perf_map_load_last_ms();
        (void)before;

        if (rc != 0) {
            smoke_fail("tour: mapLoadByName(%s) rc=%d", mapName, rc);
            continue;
        }

        objectSetLocation(gDude, gMapHeader.enteringTile, gMapHeader.enteringElevation, nullptr);
        tileWindowRefresh();

        if (!smoke_wait_ms(6000)) {
            break;
        }

        {
            int avgMs = ps2_perf_frame_avg_ms();
            int maxMs = ps2_perf_frame_max_ms();
            int avgFps = avgMs > 0 ? 1000 / avgMs : 0;
            int minFps = maxMs > 0 ? 1000 / maxMs : 0;
            int usedKb = ps2_mem_get_usage_kb();
            int peakKb = ps2_mem_get_peak_kb();

            SMOKE_PERF("tour %s: load=%d ms avg=%d ms (~%d fps) min=%d ms (~%d fps) slow>50ms=%d mem=%d KB peak=%d KB oom=%d (target >=45 fps real HW)",
                mapName, loadMs, avgMs, avgFps, maxMs, minFps,
                ps2_perf_frame_slow_count(), usedKb, peakKb, ps2_mem_get_oom_count());

            /* Hard-fail only on pathological numbers: PCSX2's software GS is
             * far slower than real hardware, so the FPS targets above are
             * reported for the human, not enforced here. */
            if (avgMs > 100) {
                smoke_fail("tour %s: average frame %d ms (~%d fps)", mapName, avgMs, avgFps);
            } else if (avgMs > 40) {
                SMOKE_LOG("WARN: tour %s average frame %d ms is below the >=45 fps target", mapName, avgMs);
            }
            if (maxMs > 1500) {
                smoke_fail("tour %s: worst frame %d ms", mapName, maxMs);
            } else if (maxMs > 500) {
                SMOKE_LOG("WARN: tour %s worst frame %d ms", mapName, maxMs);
            }
            if (peakKb > 30720) {
                smoke_fail("tour %s: peak memory %d KB is too close to the 32 MB budget", mapName, peakKb);
            }
            if (ps2_mem_get_oom_count() != 0) {
                smoke_fail("tour %s: %d allocation failures", mapName, ps2_mem_get_oom_count());
            }
        }

        ps2_mem_print_stats();
        SMOKE_LOG("tour: %s done (%s)", mapName, gMapHeader.name);
    }

    s_tour_safety = false;

    return SMOKE_ACT_OK();
}

/* ---- Verdict ---- */

static void smoke_report(void)
{
    int i;
    int avgMs = smoke_fps_avg_ms();

    for (i = 0; i < s_ui_time_count; i++) {
        SMOKE_PERF("ui-open %s %d ms", s_ui_times[i].name, s_ui_times[i].ms);
    }

    SMOKE_PERF("frames=%u avg=%d ms (~%d fps) max=%u ms worst=%s",
        s_fps_frames, avgMs, avgMs > 0 ? 1000 / avgMs : 0,
        s_fps_max_ms, s_fps_worst[0] != '\0' ? s_fps_worst : "?");

    ps2_perf_print();
}

static void smoke_final_verdict(void)
{
    s_active = false;
    smoke_script_clear();

    ps2_mem_print_stats();
    smoke_report();

    if (s_fail_count == 0) {
        SMOKE_LOG("SUITE_FINISHED_SUCCESS");
    } else {
        SMOKE_LOG("SUITE_FAILED_%s (%d failure%s)", s_first_failure, s_fail_count, s_fail_count == 1 ? "" : "s");
    }

    fflush(stdout);
    exit(s_fail_count == 0 ? 0 : 1);
}

/* ---- Driver ---- */

int ps2_smoke_main(void)
{
    if (s_active) {
        return 0;
    }

    s_active = true;
    s_run_start = ps2_time_ms();
    s_mem_next_report = s_run_start + SMOKE_MEM_PERIOD_MS;

    SMOKE_LOG("suite: start (%s)", ps2_mem_get_usage_kb() > 0 ? "ps2 dev smoke" : "ps2 dev smoke");
    smoke_select_parts();

    if (s_run_part1) {
        SMOKE_LOG("=== PART 1: core + options + save/load%s ===", s_fast_mode ? " (fast)" : "");

        bool ok = smoke_act_frontend();
        if (ok) ok = smoke_act_boot_game();
        if (ok && !s_fast_mode) ok = smoke_act_map_verify();
        if (ok && !s_fast_mode) ok = smoke_act_ui_tour();
        if (ok && !s_fast_mode) ok = smoke_act_options_sweep();
        if (ok && !s_fast_mode) ok = smoke_act_combat();
        if (ok) ok = smoke_act_save_load();
        if (ok) {
            SMOKE_LOG("PART_1_PASSED");
        }

        smoke_step_end();
        ps2_mem_print_stats();
        smoke_report();
    }

    if (s_run_part2) {
        SMOKE_LOG("=== PART 2: heavy stress + fps ===");

        /* Part 2 can run alone (PS2_SMOKE_PART2 marker without PART1): it
         * needs a booted game, so drive the frontend first. */
        bool tourOk = true;
        if (!s_run_part1) {
            tourOk = smoke_act_frontend();
            if (tourOk) {
                tourOk = smoke_act_boot_game();
            }
        }

        if (tourOk && smoke_act_tour()) {
            SMOKE_LOG("PART_2_PASSED");
        }

        smoke_step_end();
        ps2_mem_print_stats();
    }

    smoke_final_verdict();

    return 0;
}

/* ---- Tick ---- */

static void smoke_tick_impl(void)
{
    unsigned int now;

    if (!s_active) {
        return;
    }

    now = ps2_time_ms();

    smoke_input_pump(now);
    smoke_ui_watch_tick(now);
    smoke_osk_tick();
    smoke_fps_tick(now);
    smoke_combat_tick(now);
    smoke_tour_safety_tick(now);
    smoke_watchdog_tick(now);

    ps2_perf_ui_context(s_step_active ? s_step_name : "boot");
}

} // namespace fallout

extern "C" void ps2_smoke_tick(void)
{
    fallout::smoke_tick_impl();
}

#else

/* Keep the translation unit non-empty in non-smoke builds. */
typedef int ps2_smoke_unused;

#endif /* PS2_DEV_SMOKE_TEST */
