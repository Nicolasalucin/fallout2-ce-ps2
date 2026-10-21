#include "ps2_perf.h"

#ifdef PS2_DEV_SMOKE_TEST

#include <stdio.h>
#include <string.h>

#include "ps2_time.h"

/* EE system timer: 147.456 MHz. Keep the conversions here so the hot path
 * only reads the raw counter and never divides. */
#define PS2_PERF_TICKS_PER_MS 147456ULL
#define PS2_PERF_TICKS_PER_US 147ULL
/* Frame deltas above one second are blocking work (map load, save), not a
 * frame: keep them out of the FPS statistics but still account their stages. */
#define PS2_PERF_MAX_FRAME_TICKS (1000ULL * PS2_PERF_TICKS_PER_MS)
#define PS2_PERF_SLOW_FRAME_TICKS (50ULL * PS2_PERF_TICKS_PER_MS)

#define PS2_PERF_CONTEXT_MAX 40

static const char* const s_stage_names[PS2_PERF_STAGE_COUNT] = {
    "map",
    "tile",
    "floor",
    "roof",
    "objpre",
    "objpost",
    "scriptreq",
    "scriptmap",
    "scriptexec",
    "interp",
    "lut",
    "osk",
    "upload",
    "draw",
    "exec",
    "flip",
    "vfs",
    "rawio",
    "inflate",
};

static const char* const s_counter_names[PS2_PERF_COUNT_COUNT] = {
    "trans",
    "darktrans",
    "translucent",
    "darktranslucent",
    "intensity",
    "blit_px",
    "scriptreq",
    "scriptexec",
    "cachehit",
    "cachemiss",
    "cacheevict",
    "dfileread",
    "dfilebytes",
    "inflate",
    "inflatebytes",
};

/* ---- Tick conversion helpers ---- */

static unsigned long long perf_ticks_to_us(uint64_t ticks)
{
    return (unsigned long long)((ticks + (PS2_PERF_TICKS_PER_US / 2)) / PS2_PERF_TICKS_PER_US);
}

static unsigned long long perf_ticks_to_ms(uint64_t ticks)
{
    return (unsigned long long)((ticks + (PS2_PERF_TICKS_PER_MS / 2)) / PS2_PERF_TICKS_PER_MS);
}

/* ---- Map load ---- */

static uint64_t s_map_begin_ticks;
static int s_map_last_ms;
static int s_map_max_ms;
static int s_map_count;
static char s_map_last_name[24];

void ps2_perf_map_load_begin(void)
{
    s_map_begin_ticks = ps2_time_ticks();
    ps2_perf_stage_begin(PS2_PERF_STAGE_MAP_LOAD);
}

void ps2_perf_map_load_end(const char* mapName)
{
    ps2_perf_stage_end(PS2_PERF_STAGE_MAP_LOAD);

    int ms = (int)perf_ticks_to_ms(ps2_time_ticks() - s_map_begin_ticks);
    s_map_last_ms = ms;
    s_map_count++;
    if (ms > s_map_max_ms) {
        s_map_max_ms = ms;
    }
    if (mapName != NULL) {
        strncpy(s_map_last_name, mapName, sizeof(s_map_last_name) - 1);
        s_map_last_name[sizeof(s_map_last_name) - 1] = '\0';
    } else {
        s_map_last_name[0] = '\0';
    }

    printf("[PERF] map-load %s %d ms (max %d ms, loads %d)\n",
        s_map_last_name[0] != '\0' ? s_map_last_name : "?",
        ms, s_map_max_ms, s_map_count);
    fflush(stdout);
}

int ps2_perf_map_load_last_ms(void) { return s_map_last_ms; }
int ps2_perf_map_load_max_ms(void) { return s_map_max_ms; }
int ps2_perf_map_load_count(void) { return s_map_count; }

/* ---- UI latency ---- */

static const char* s_ui_name;
static uint64_t s_ui_begin_ticks;
static int s_ui_depth;

void ps2_perf_ui_begin(const char* name)
{
    if (s_ui_depth == 0) {
        s_ui_name = name;
        s_ui_begin_ticks = ps2_time_ticks();
    }
    s_ui_depth++;
}

void ps2_perf_ui_end(void)
{
    if (s_ui_depth > 0) {
        s_ui_depth--;
    }
    if (s_ui_depth == 0 && s_ui_name != NULL) {
        printf("[PERF] ui-open %s %u us\n", s_ui_name,
            (unsigned)perf_ticks_to_us(ps2_time_ticks() - s_ui_begin_ticks));
        fflush(stdout);
        s_ui_name = NULL;
    }
}

/* ---- Stage and counter accumulators ---- */

/* Current frame */
static uint64_t s_frame_stage_ticks[PS2_PERF_STAGE_COUNT];
static uint64_t s_frame_stage_start[PS2_PERF_STAGE_COUNT];
static int s_frame_stage_depth[PS2_PERF_STAGE_COUNT];
static unsigned long long s_frame_counter[PS2_PERF_COUNT_COUNT];

/* Run-wide (never reset by the per-map frame reset) */
static uint64_t s_run_stage_ticks[PS2_PERF_STAGE_COUNT];
static unsigned long long s_run_counter[PS2_PERF_COUNT_COUNT];

/* Per act/map scope (reset by the smoke harness or a context change) */
static uint64_t s_scope_stage_ticks[PS2_PERF_STAGE_COUNT];
static unsigned long long s_scope_counter[PS2_PERF_COUNT_COUNT];

/* Worst frame snapshot (run-wide) */
static uint64_t s_worst_delta_ticks;
static uint64_t s_worst_flip_ticks;
static uint64_t s_worst_stage_ticks[PS2_PERF_STAGE_COUNT];
static unsigned long long s_worst_counter[PS2_PERF_COUNT_COUNT];
static char s_worst_context[PS2_PERF_CONTEXT_MAX];

void ps2_perf_stage_begin(int stage)
{
    if ((unsigned)stage >= PS2_PERF_STAGE_COUNT) {
        return;
    }

    if (s_frame_stage_depth[stage]++ == 0) {
        s_frame_stage_start[stage] = ps2_time_ticks();
    }
}

void ps2_perf_stage_end(int stage)
{
    if ((unsigned)stage >= PS2_PERF_STAGE_COUNT) {
        return;
    }

    if (s_frame_stage_depth[stage] <= 0) {
        return;
    }

    if (--s_frame_stage_depth[stage] == 0) {
        s_frame_stage_ticks[stage] += ps2_time_ticks() - s_frame_stage_start[stage];
    }
}

void ps2_perf_counter_add(int counter, unsigned long long value)
{
    if ((unsigned)counter >= PS2_PERF_COUNT_COUNT) {
        return;
    }

    s_frame_counter[counter] += value;
    s_run_counter[counter] += value;
    s_scope_counter[counter] += value;
}

/* Adds the stages accumulated since the previous call to the run and scope
 * totals. The frame's own stage array is left untouched by the caller when it
 * needs to snapshot the worst frame first. */
static void perf_flush_frame_stages(void)
{
    for (int i = 0; i < PS2_PERF_STAGE_COUNT; i++) {
        if (s_frame_stage_depth[i] == 0 && s_frame_stage_ticks[i] != 0) {
            s_run_stage_ticks[i] += s_frame_stage_ticks[i];
            s_scope_stage_ticks[i] += s_frame_stage_ticks[i];
            s_frame_stage_ticks[i] = 0;
        }
    }
}

static void perf_reset_frame_accumulators(void)
{
    memset(s_frame_stage_ticks, 0, sizeof(s_frame_stage_ticks));
    memset(s_frame_stage_depth, 0, sizeof(s_frame_stage_depth));
    memset(s_frame_counter, 0, sizeof(s_frame_counter));
}

/* ---- Automatic frame profiling ---- */

static char s_context[PS2_PERF_CONTEXT_MAX] = "boot";
static const char* s_frame_context = s_context;
static uint64_t s_frame_last_ticks;
static uint64_t s_frame_accum_ticks;
static unsigned int s_frame_count;
static uint64_t s_frame_max_ticks;
static unsigned int s_frame_slow_count;
static char s_frame_worst_name[32];

/* Per-map window, reset by ps2_perf_frame_reset(). */
static uint64_t s_window_frame_ticks;
static uint64_t s_window_flip_ticks;
static uint64_t s_window_cpu_ticks;
static unsigned int s_window_frames;

/* Run-wide frame accounting, never reset by the per-map window. */
static uint64_t s_run_frame_ticks;
static uint64_t s_run_flip_ticks;
static uint64_t s_run_cpu_ticks;
static unsigned int s_run_frames;
static uint64_t s_run_max_ticks;

/* Per-scope frame accounting. */
static uint64_t s_scope_frame_ticks;
static uint64_t s_scope_flip_ticks;
static uint64_t s_scope_cpu_ticks;
static unsigned int s_scope_frames;

void ps2_perf_ui_context(const char* name)
{
    if (name == NULL || name[0] == '\0') {
        name = "?";
    }

    if (strcmp(name, s_context) == 0) {
        s_frame_context = s_context;
        return;
    }

    /* The act is changing: close its scope and print the breakdown before
     * starting the new one. Pending stage ticks belong to the old act. */
    perf_flush_frame_stages();
    ps2_perf_scope_print(s_context);
    ps2_perf_scope_reset();

    strncpy(s_context, name, sizeof(s_context) - 1);
    s_context[sizeof(s_context) - 1] = '\0';
    s_frame_context = s_context;
}

void ps2_perf_frame_reset(void)
{
    /* Keep the stages of the frame that just ended in the run/scope totals. */
    perf_flush_frame_stages();

    s_frame_last_ticks = 0;
    s_frame_accum_ticks = 0;
    s_frame_count = 0;
    s_frame_max_ticks = 0;
    s_frame_slow_count = 0;
    s_frame_worst_name[0] = '\0';

    s_window_frame_ticks = 0;
    s_window_flip_ticks = 0;
    s_window_cpu_ticks = 0;
    s_window_frames = 0;

    perf_reset_frame_accumulators();
}

void ps2_perf_frame_tick(uint64_t nowTicks)
{
    if (nowTicks == 0) {
        return;
    }

    if (s_frame_last_ticks == 0) {
        s_frame_last_ticks = nowTicks;
        return;
    }

    uint64_t delta = nowTicks - s_frame_last_ticks;
    s_frame_last_ticks = nowTicks;

    uint64_t flip = s_frame_stage_ticks[PS2_PERF_STAGE_PRESENT_FLIP];
    uint64_t cpu = delta > flip ? delta - flip : 0;

    if (delta > 0 && delta <= PS2_PERF_MAX_FRAME_TICKS) {
        s_frame_count++;
        s_frame_accum_ticks += delta;
        if (delta > s_frame_max_ticks) {
            s_frame_max_ticks = delta;
            strncpy(s_frame_worst_name, s_frame_context, sizeof(s_frame_worst_name) - 1);
            s_frame_worst_name[sizeof(s_frame_worst_name) - 1] = '\0';
        }
        if (delta > PS2_PERF_SLOW_FRAME_TICKS) {
            s_frame_slow_count++;
        }

        s_window_frames++;
        s_window_frame_ticks += delta;
        s_window_flip_ticks += flip;
        s_window_cpu_ticks += cpu;

        s_run_frames++;
        s_run_frame_ticks += delta;
        s_run_flip_ticks += flip;
        s_run_cpu_ticks += cpu;
        if (delta > s_run_max_ticks) {
            s_run_max_ticks = delta;
        }

        s_scope_frames++;
        s_scope_frame_ticks += delta;
        s_scope_flip_ticks += flip;
        s_scope_cpu_ticks += cpu;

        /* Snapshot everything that explains an outlier while the stage and
         * counter arrays still belong to that frame. */
        if (delta > s_worst_delta_ticks) {
            s_worst_delta_ticks = delta;
            s_worst_flip_ticks = flip;
            memcpy(s_worst_stage_ticks, s_frame_stage_ticks, sizeof(s_worst_stage_ticks));
            memcpy(s_worst_counter, s_frame_counter, sizeof(s_worst_counter));
            strncpy(s_worst_context, s_frame_context, sizeof(s_worst_context) - 1);
            s_worst_context[sizeof(s_worst_context) - 1] = '\0';
        }
    }

    perf_flush_frame_stages();
    memset(s_frame_counter, 0, sizeof(s_frame_counter));
}

int ps2_perf_frame_avg_ms(void)
{
    if (s_frame_count == 0) {
        return 0;
    }
    return (int)perf_ticks_to_ms(s_frame_accum_ticks / s_frame_count);
}

int ps2_perf_frame_max_ms(void) { return (int)perf_ticks_to_ms(s_frame_max_ticks); }
int ps2_perf_frame_slow_count(void) { return (int)s_frame_slow_count; }
const char* ps2_perf_frame_worst_state(void) { return s_frame_worst_name; }

/* ---- Scope and worst-frame reporting ---- */

static void perf_print_stage_us(const char* kind, const char* tag,
    const uint64_t* ticks, unsigned int frames)
{
    printf("[PERF] %s %s stages_us", kind, tag);
    for (int i = 0; i < PS2_PERF_STAGE_COUNT; i++) {
        unsigned long long us;
        if (frames > 0) {
            us = perf_ticks_to_us(ticks[i] / frames);
        } else {
            us = perf_ticks_to_us(ticks[i]);
        }
        printf(" %s=%llu", s_stage_names[i], us);
    }
    printf("\n");
}

static void perf_print_counters(const char* kind, const char* tag,
    const unsigned long long* counters)
{
    printf("[PERF] %s %s counts", kind, tag);
    for (int i = 0; i < PS2_PERF_COUNT_COUNT; i++) {
        printf(" %s=%llu", s_counter_names[i], counters[i]);
    }
    printf("\n");
}

void ps2_perf_scope_reset(void)
{
    memset(s_scope_stage_ticks, 0, sizeof(s_scope_stage_ticks));
    memset(s_scope_counter, 0, sizeof(s_scope_counter));
    s_scope_frame_ticks = 0;
    s_scope_flip_ticks = 0;
    s_scope_cpu_ticks = 0;
    s_scope_frames = 0;
}

void ps2_perf_scope_flush(void)
{
    perf_flush_frame_stages();
}

void ps2_perf_scope_print(const char* tag)
{
    if (tag == NULL || tag[0] == '\0') {
        tag = "?";
    }

    if (s_scope_frames == 0) {
        /* Scopes without sampled frames are boot/selftest bursts: their
         * stage totals are not directly comparable to the per-frame A/B
         * table. Map loads are reported explicitly after their gameplay
         * window, so they always have frames here. */
        return;
    }

    unsigned long long frame_us = s_scope_frames > 0
        ? perf_ticks_to_us(s_scope_frame_ticks / s_scope_frames)
        : 0;
    unsigned long long flip_us = s_scope_frames > 0
        ? perf_ticks_to_us(s_scope_flip_ticks / s_scope_frames)
        : 0;
    unsigned long long cpu_us = s_scope_frames > 0
        ? perf_ticks_to_us(s_scope_cpu_ticks / s_scope_frames)
        : 0;

    printf("[PERF] scope %s frames=%u frame_us=%llu flip_us=%llu cpu_us=%llu\n",
        tag, s_scope_frames, frame_us, flip_us, cpu_us);
    perf_print_stage_us("scope", tag, s_scope_stage_ticks, s_scope_frames);
    perf_print_counters("scope", tag, s_scope_counter);
    fflush(stdout);
}

static void perf_print_worst(void)
{
    if (s_worst_delta_ticks == 0) {
        return;
    }

    unsigned long long flip_us = perf_ticks_to_us(s_worst_flip_ticks);
    unsigned long long cpu_us = perf_ticks_to_us(
        s_worst_delta_ticks > s_worst_flip_ticks ? s_worst_delta_ticks - s_worst_flip_ticks : 0);

    printf("[PERF] worst frame_us=%llu ctx=%s flip_us=%llu cpu_us=%llu\n",
        perf_ticks_to_us(s_worst_delta_ticks), s_worst_context, flip_us, cpu_us);
    perf_print_stage_us("worst", s_worst_context, s_worst_stage_ticks, 1);
    perf_print_counters("worst", s_worst_context, s_worst_counter);
}

/* ---- Combat difficulty observation ---- */

#define PS2_PERF_CBT_KINDS 3

static int s_cbt_count[PS2_PERF_CBT_KINDS];
static int s_cbt_seen[PS2_PERF_CBT_KINDS] = { -1, -1, -1 };
static int s_cbt_mod[PS2_PERF_CBT_KINDS] = { 0, 0, 0 };
static unsigned int s_cbt_diff_mask[PS2_PERF_CBT_KINDS];

void ps2_perf_cbt_difficulty(int kind, int difficulty, int modifier)
{
    if (kind < 0 || kind >= PS2_PERF_CBT_KINDS) {
        return;
    }
    s_cbt_count[kind]++;
    s_cbt_seen[kind] = difficulty;
    s_cbt_mod[kind] = modifier;
    if (difficulty >= 0 && difficulty < 32) {
        s_cbt_diff_mask[kind] |= 1u << difficulty;
    }
}

int ps2_perf_cbt_count(int kind)
{
    return (kind >= 0 && kind < PS2_PERF_CBT_KINDS) ? s_cbt_count[kind] : 0;
}

int ps2_perf_cbt_difficulty_seen(int kind)
{
    return (kind >= 0 && kind < PS2_PERF_CBT_KINDS) ? s_cbt_seen[kind] : -1;
}

int ps2_perf_cbt_mod(int kind)
{
    return (kind >= 0 && kind < PS2_PERF_CBT_KINDS) ? s_cbt_mod[kind] : 0;
}

unsigned int ps2_perf_cbt_diff_mask(int kind)
{
    return (kind >= 0 && kind < PS2_PERF_CBT_KINDS) ? s_cbt_diff_mask[kind] : 0;
}

/* ---- Reporting ---- */

void ps2_perf_reset(void)
{
    s_map_last_ms = 0;
    s_map_max_ms = 0;
    s_map_count = 0;
    s_frame_last_ticks = 0;

    s_run_frames = 0;
    s_run_frame_ticks = 0;
    s_run_flip_ticks = 0;
    s_run_cpu_ticks = 0;
    s_run_max_ticks = 0;
    memset(s_run_stage_ticks, 0, sizeof(s_run_stage_ticks));
    memset(s_run_counter, 0, sizeof(s_run_counter));
    memset(s_worst_stage_ticks, 0, sizeof(s_worst_stage_ticks));
    memset(s_worst_counter, 0, sizeof(s_worst_counter));
    s_worst_delta_ticks = 0;
    s_worst_flip_ticks = 0;
    s_worst_context[0] = '\0';

    ps2_perf_scope_reset();
    ps2_perf_frame_reset();

    for (int i = 0; i < PS2_PERF_CBT_KINDS; i++) {
        s_cbt_count[i] = 0;
        s_cbt_seen[i] = -1;
        s_cbt_mod[i] = 0;
        s_cbt_diff_mask[i] = 0;
    }
}

void ps2_perf_print(void)
{
    int avg = ps2_perf_frame_avg_ms();

    printf("[PERF] frames=%u avg=%d ms (~%d fps) max=%d ms slow(>50ms)=%d worst-state=%s\n",
        s_frame_count,
        avg,
        avg > 0 ? 1000 / avg : 0,
        (int)perf_ticks_to_ms(s_frame_max_ticks),
        (int)s_frame_slow_count,
        s_frame_worst_name[0] != '\0' ? s_frame_worst_name : "?");
    printf("[PERF] maps=%d last=%d ms max=%d ms\n",
        s_map_count, s_map_last_ms, s_map_max_ms);

    if (s_run_frames != 0) {
        printf("[PERF] run frames=%u frame_us=%llu flip_us=%llu cpu_us=%llu max_us=%llu\n",
            s_run_frames,
            perf_ticks_to_us(s_run_frame_ticks / s_run_frames),
            perf_ticks_to_us(s_run_flip_ticks / s_run_frames),
            perf_ticks_to_us(s_run_cpu_ticks / s_run_frames),
            perf_ticks_to_us(s_run_max_ticks));
    } else {
        printf("[PERF] run frames=0 frame_us=0 flip_us=0 cpu_us=0 max_us=0\n");
    }

    perf_print_stage_us("run", "all", s_run_stage_ticks, s_run_frames);
    perf_print_counters("run", "all", s_run_counter);
    perf_print_worst();

    printf("[PERF] cbt: tohit-reads=%d (diff %d mod %d mask 0x%X) dmg-reads=%d (diff %d mod %d mask 0x%X) ai-reads=%d (diff %d mod %d mask 0x%X)\n",
        s_cbt_count[PS2_PERF_CBT_TOHIT], s_cbt_seen[PS2_PERF_CBT_TOHIT], s_cbt_mod[PS2_PERF_CBT_TOHIT], s_cbt_diff_mask[PS2_PERF_CBT_TOHIT],
        s_cbt_count[PS2_PERF_CBT_DAMAGE], s_cbt_seen[PS2_PERF_CBT_DAMAGE], s_cbt_mod[PS2_PERF_CBT_DAMAGE], s_cbt_diff_mask[PS2_PERF_CBT_DAMAGE],
        s_cbt_count[PS2_PERF_CBT_AI], s_cbt_seen[PS2_PERF_CBT_AI], s_cbt_mod[PS2_PERF_CBT_AI], s_cbt_diff_mask[PS2_PERF_CBT_AI]);
    fflush(stdout);
}

#endif /* PS2_DEV_SMOKE_TEST */
