/*
 * PS2 low-level EE memory allocator implementation.
 *
 * Uses the PS2 SDK's standard malloc/free which manages the EE main heap
 * (32 MB). No custom heap management - we rely on the SDK's allocator
 * and just add alignment helpers and debugging.
 */

#include "ps2_heap.h"

#include <malloc.h>
#include <stdlib.h>
#include <string.h>

void* ps2_mem_malloc(size_t size)
{
    if (size == 0) {
        size = 1;
    }
    void* ptr = malloc(size);
    return ptr;
}

void* ps2_mem_realloc(void* ptr, size_t size)
{
    if (size == 0) {
        ps2_mem_free(ptr);
        return NULL;
    }
    return realloc(ptr, size);
}

void ps2_mem_free(void* ptr)
{
    if (ptr) {
        free(ptr);
    }
}

void* ps2_mem_memalign(size_t alignment, size_t size)
{
    if (size == 0) {
        size = 1;
    }
    /* memalign is available in PS2 SDK libc. Alignment must be power of 2. */
    return memalign(alignment, size);
}

size_t ps2_mem_free_space(void)
{
    /* mallinfo is not available in PS2 SDK. Return a rough estimate:
     * 32 MB minus what we've tracked. The real free space is managed
     * by the SDK's heap allocator internally. */
    struct mallinfo mi = mallinfo();
    return (size_t)mi.fordblks;  /* free ordinary blocks */
}