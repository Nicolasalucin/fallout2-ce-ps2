#include "ps2_perf.h"

#include <stdio.h>
#include <string.h>

#include "ps2_time.h"

/* ---- Map load ---- */

static unsigned int s_map_begin_ms;
static int s_map_last_ms;
static int s_map_max_ms;
static int s_map_count;
static char s_map_last_name[24];

void ps2_perf_map_load_begin(void)
{
    s_map_begin_ms = ps2_time_ms();
}

void ps2_perf_map_load_end(const char* mapName)
{
    int ms = (int)(ps2_time_ms() - s_map_begin_ms);
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
static unsigned int s_ui_begin_ms;
static int s_ui_depth;

void ps2_perf_ui_begin(const char* name)
{
    if (s_ui_depth == 0) {
        s_ui_name = name;
        s_ui_begin_ms = ps2_time_ms();
    }
    s_ui_depth++;
}

void ps2_perf_ui_end(void)
{
    if (s_ui_depth > 0) {
        s_ui_depth--;
    }
    if (s_ui_depth == 0 && s_ui_name != NULL) {
        printf("[PERF] ui-open %s %u ms\n", s_ui_name,
            (unsigned)(ps2_time_ms() - s_ui_begin_ms));
        fflush(stdout);
        s_ui_name = NULL;
    }
}

/* ---- Automatic frame profiling ---- */

static const char* s_frame_context = "boot";
static unsigned int s_frame_last_ms;
static unsigned long long s_frame_accum_ms;
static unsigned int s_frame_count;
static unsigned int s_frame_max_ms;
static unsigned int s_frame_slow_count;
static char s_frame_worst_name[32];

void ps2_perf_ui_context(const char* name)
{
    s_frame_context = name != NULL ? name : "?";
}

void ps2_perf_frame_reset(void)
{
    s_frame_last_ms = 0;
    s_frame_accum_ms = 0;
    s_frame_count = 0;
    s_frame_max_ms = 0;
    s_frame_slow_count = 0;
    s_frame_worst_name[0] = '\0';
}

void ps2_perf_frame_tick(unsigned int nowMs)
{
    unsigned int delta;

    if (s_frame_last_ms == 0) {
        s_frame_last_ms = nowMs;
        return;
    }

    delta = nowMs - s_frame_last_ms;
    s_frame_last_ms = nowMs;

    /* A frame gap is only meaningful below one second; longer pauses are
     * blocking engine work (map loads, save round-trips) measured by their
     * own counters. */
    if (delta == 0 || delta > 1000) {
        return;
    }

    s_frame_count++;
    s_frame_accum_ms += delta;
    if (delta > s_frame_max_ms) {
        s_frame_max_ms = delta;
        strncpy(s_frame_worst_name, s_frame_context, sizeof(s_frame_worst_name) - 1);
        s_frame_worst_name[sizeof(s_frame_worst_name) - 1] = '\0';
    }
    if (delta > 50) {
        s_frame_slow_count++;
    }
}

int ps2_perf_frame_avg_ms(void)
{
    if (s_frame_count == 0) {
        return 0;
    }
    return (int)(s_frame_accum_ms / s_frame_count);
}

int ps2_perf_frame_max_ms(void) { return (int)s_frame_max_ms; }
int ps2_perf_frame_slow_count(void) { return (int)s_frame_slow_count; }
const char* ps2_perf_frame_worst_state(void) { return s_frame_worst_name; }

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
    s_frame_last_ms = 0;
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
        (int)s_frame_max_ms,
        (int)s_frame_slow_count,
        s_frame_worst_name[0] != '\0' ? s_frame_worst_name : "?");
    printf("[PERF] maps=%d last=%d ms max=%d ms\n",
        s_map_count, s_map_last_ms, s_map_max_ms);
    printf("[PERF] cbt: tohit-reads=%d (diff %d mod %d mask 0x%X) dmg-reads=%d (diff %d mod %d mask 0x%X) ai-reads=%d (diff %d mod %d mask 0x%X)\n",
        s_cbt_count[PS2_PERF_CBT_TOHIT], s_cbt_seen[PS2_PERF_CBT_TOHIT], s_cbt_mod[PS2_PERF_CBT_TOHIT], s_cbt_diff_mask[PS2_PERF_CBT_TOHIT],
        s_cbt_count[PS2_PERF_CBT_DAMAGE], s_cbt_seen[PS2_PERF_CBT_DAMAGE], s_cbt_mod[PS2_PERF_CBT_DAMAGE], s_cbt_diff_mask[PS2_PERF_CBT_DAMAGE],
        s_cbt_count[PS2_PERF_CBT_AI], s_cbt_seen[PS2_PERF_CBT_AI], s_cbt_mod[PS2_PERF_CBT_AI], s_cbt_diff_mask[PS2_PERF_CBT_AI]);
    fflush(stdout);
}
