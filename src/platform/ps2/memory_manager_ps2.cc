/*
 * PS2-specific memory manager with 32 MB hard budget enforcement.
 *
 * The PS2 EE has 32 MB main RAM. This module wraps the engine's allocator
 * to track total usage, enforce the budget, and provide graceful degradation
 * instead of hard exit() on OOM.
 *
 * Budget breakdown (target):
 *   Engine heap (malloc/free)     : ~16 MB
 *   ART cache (FRM/ACM/tiles)     :  4 MB (configurable, was 8 MB)
 *   Sound effects cache           :  256 KB (configurable, was 448 KB)
 *   Map/Script/AI working buffers : ~6 MB
 *   FS directory cache            :  192 KB
 *   Stacks, framebuffers, IOP comm: ~3 MB
 *   Safety margin / fragmentation : ~2 MB
 *   --------------------------------
 *   TOTAL                         : 32 MB
 */

#include "memory_manager.h"
#include "memory_defs.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ps2_heap.h"

namespace fallout {

/* Type from the original memory_manager.cc */
typedef void(MemoryManagerPrintErrorProc)(const char* string);

/* ---- PS2 budget configuration ---- */

#define PS2_MEM_TOTAL_BUDGET       (32 * 1024 * 1024)   /* 32 MB */
#define PS2_MEM_ENGINE_HEAP_MAX    (16 * 1024 * 1024)   /* 16 MB */
#define PS2_MEM_ART_CACHE_MAX      (4 * 1024 * 1024)    /* 4 MB default */
#define PS2_MEM_SOUND_CACHE_MAX    (256 * 1024)         /* 256 KB default */
#define PS2_MEM_WARN_THRESHOLD     (PS2_MEM_TOTAL_BUDGET * 9 / 10)  /* 90% */

static size_t s_ps2_total_allocated = 0;
static size_t s_ps2_peak_allocated = 0;
static int s_ps2_oom_count = 0;

/* ---- Global proc pointers (mirror the upstream memory_manager.cc) ---- */

static MemoryManagerPrintErrorProc* gMemoryManagerPrintErrorProc = nullptr;
static MallocProc* gMemoryManagerMallocProc = nullptr;
static ReallocProc* gMemoryManagerReallocProc = nullptr;
static FreeProc* gMemoryManagerFreeProc = nullptr;

/* ---- Default implementations using PS2 EE heap ---- */

static void memoryManagerDefaultPrintErrorImpl(const char* string)
{
    printf("%s", string);
}

static void* memoryManagerDefaultMallocImpl(size_t size)
{
    if (size == 0) size = 1;
    void* ptr = ps2_mem_malloc(size);
    return ptr;
}

static void* memoryManagerDefaultReallocImpl(void* ptr, size_t size)
{
    if (size == 0) {
        ps2_mem_free(ptr);
        return nullptr;
    }
    return ps2_mem_realloc(ptr, size);
}

static void memoryManagerDefaultFreeImpl(void* ptr)
{
    if (ptr) ps2_mem_free(ptr);
}

/* ---- Initialize default procs at startup ---- */

void ps2_mem_init_procs(void)
{
    if (gMemoryManagerMallocProc == nullptr) {
        gMemoryManagerPrintErrorProc = memoryManagerDefaultPrintErrorImpl;
        gMemoryManagerMallocProc = memoryManagerDefaultMallocImpl;
        gMemoryManagerReallocProc = memoryManagerDefaultReallocImpl;
        gMemoryManagerFreeProc = memoryManagerDefaultFreeImpl;
    }
}

extern "C" void fallout_ps2_mem_init_procs_impl(void)
{
    ps2_mem_init_procs();
}

/* ---- memoryManagerSetProcs: called by gameMemoryInit() ---- */

void memoryManagerSetProcs(MallocProc* mallocProc, ReallocProc* reallocProc, FreeProc* freeProc)
{
    gMemoryManagerMallocProc = mallocProc ? mallocProc : memoryManagerDefaultMallocImpl;
    gMemoryManagerReallocProc = reallocProc ? reallocProc : memoryManagerDefaultReallocImpl;
    gMemoryManagerFreeProc = freeProc ? freeProc : memoryManagerDefaultFreeImpl;
}

/* ---- Error reporting ---- */

static int memoryManagerPrintError(const char* format, ...)
{
    static char err[256];
    int length = 0;

    if (gMemoryManagerPrintErrorProc != nullptr) {
        va_list args;
        va_start(args, format);
        length = vsnprintf(err, sizeof(err), format, args);
        va_end(args);

        gMemoryManagerPrintErrorProc(err);
    }

    return length;
}

[[noreturn]] static void memoryManagerFatalAllocationError(const char* func, size_t size, const char* file, int line)
{
    memoryManagerPrintError("%s: Error allocating block of size %zu (%zx), %s %d\n", func, size, size, file, line);
    /* Graceful: don't exit(1), but we can't return. In practice this should
     * not be hit because the safe allocators check budget first. */
    for (;;) { }
}

/* ---- Safe allocators (used by engine code) ---- */

void* internal_malloc_safe(size_t size, const char* file, int line)
{
    ps2_mem_init_procs();

    /* Budget check before allocating */
    if (s_ps2_total_allocated + size > PS2_MEM_TOTAL_BUDGET) {
        printf("[ps2] mem: OOM denied %zu bytes (used %zu/%zu MB) at %s:%d\n",
               size, s_ps2_total_allocated / (1024*1024), PS2_MEM_TOTAL_BUDGET / (1024*1024), file, line);
        s_ps2_oom_count++;
        return nullptr;
    }

    void* ptr = gMemoryManagerMallocProc(size);
    if (ptr == nullptr) {
        s_ps2_oom_count++;
        return nullptr;
    }

    s_ps2_total_allocated += size;
    if (s_ps2_total_allocated > s_ps2_peak_allocated) {
        s_ps2_peak_allocated = s_ps2_total_allocated;
    }

    return ptr;
}

void* internal_realloc_safe(void* ptr, size_t size, const char* file, int line)
{
    ps2_mem_init_procs();

    if (size == 0) {
        internal_free_safe(ptr, file, line);
        return nullptr;
    }

    if (s_ps2_total_allocated + size > PS2_MEM_TOTAL_BUDGET) {
        printf("[ps2] mem: OOM denied realloc %zu bytes (used %zu/%zu MB) at %s:%d\n",
               size, s_ps2_total_allocated / (1024*1024), PS2_MEM_TOTAL_BUDGET / (1024*1024), file, line);
        s_ps2_oom_count++;
        return nullptr;
    }

    void* new_ptr = gMemoryManagerReallocProc(ptr, size);
    if (new_ptr == nullptr) {
        s_ps2_oom_count++;
        return nullptr;
    }

    /* Rough tracking - we don't know exact old size */
    s_ps2_total_allocated += size;
    if (s_ps2_total_allocated > s_ps2_peak_allocated) {
        s_ps2_peak_allocated = s_ps2_total_allocated;
    }

    return new_ptr;
}

void internal_free_safe(void* ptr, const char* file, int line)
{
    if (ptr == nullptr) {
        printf("[ps2] mem: free(nullptr) at %s:%d\n", file, line);
        return;  /* Graceful: don't exit(1) */
    }
    gMemoryManagerFreeProc(ptr);
    /* Rough estimate */
    if (s_ps2_total_allocated > 1024) {
        s_ps2_total_allocated -= 1024;
    } else {
        s_ps2_total_allocated = 0;
    }
}

void* internal_calloc_safe(int count, int size, const char* file, int line)
{
    ps2_mem_init_procs();

    size_t total = (size_t)count * (size_t)size;
    if (s_ps2_total_allocated + total > PS2_MEM_TOTAL_BUDGET) {
        printf("[ps2] mem: OOM denied calloc %zu bytes at %s:%d\n",
               total, file, line);
        s_ps2_oom_count++;
        return nullptr;
    }

    void* ptr = gMemoryManagerMallocProc(total);
    if (ptr == nullptr) {
        s_ps2_oom_count++;
        return nullptr;
    }

    memset(ptr, 0, total);
    s_ps2_total_allocated += total;
    if (s_ps2_total_allocated > s_ps2_peak_allocated) {
        s_ps2_peak_allocated = s_ps2_total_allocated;
    }

    return ptr;
}

char* strdup_safe(const char* string, const char* file, int line)
{
    ps2_mem_init_procs();

    size_t size = strlen(string) + 1;
    if (s_ps2_total_allocated + size > PS2_MEM_TOTAL_BUDGET) {
        printf("[ps2] mem: OOM denied strdup %zu bytes at %s:%d\n",
               size, file, line);
        s_ps2_oom_count++;
        return nullptr;
    }

    void* ptr = gMemoryManagerMallocProc(size);
    if (ptr == nullptr) {
        s_ps2_oom_count++;
        return nullptr;
    }

    strcpy((char*)ptr, string);
    s_ps2_total_allocated += size;
    if (s_ps2_total_allocated > s_ps2_peak_allocated) {
        s_ps2_peak_allocated = s_ps2_total_allocated;
    }

    return (char*)ptr;
}

/* ---- Diagnostics ---- */

void ps2_mem_print_stats_impl(void)
{
    printf("[ps2] mem: used=%zu KB peak=%zu KB budget=%zu KB oom_events=%d\n",
           s_ps2_total_allocated / 1024,
           s_ps2_peak_allocated / 1024,
           PS2_MEM_TOTAL_BUDGET / 1024,
           s_ps2_oom_count);
    fflush(stdout);
}

extern "C" void fallout_ps2_mem_print_stats_impl(void)
{
    ps2_mem_print_stats_impl();
}

int ps2_mem_get_usage_kb(void)
{
    return (int)(s_ps2_total_allocated / 1024);
}

int ps2_mem_get_peak_kb(void)
{
    return (int)(s_ps2_peak_allocated / 1024);
}

int ps2_mem_get_oom_count(void)
{
    return s_ps2_oom_count;
}

/* Cache reservation helpers */

int ps2_mem_reserve_cache(const char* name, size_t bytes)
{
    if (s_ps2_total_allocated + bytes > PS2_MEM_TOTAL_BUDGET) {
        printf("[ps2] mem: cache reserve failed for %s (%zu KB), budget full\n",
               name, bytes / 1024);
        return -1;
    }
    s_ps2_total_allocated += bytes;
    printf("[ps2] mem: reserved %zu KB for %s (total %zu KB)\n",
           bytes / 1024, name, s_ps2_total_allocated / 1024);
    return 0;
}

void ps2_mem_release_cache(size_t bytes)
{
    if (s_ps2_total_allocated > bytes) {
        s_ps2_total_allocated -= bytes;
    } else {
        s_ps2_total_allocated = 0;
    }
}

/* ---- Budget constants (used by ps2_heap.h declaration) ---- */

const size_t PS2_MEM_TOTAL_BUDGET_KB = PS2_MEM_TOTAL_BUDGET / 1024;

} /* namespace fallout */