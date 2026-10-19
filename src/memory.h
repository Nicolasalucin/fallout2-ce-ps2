#ifndef MEMORY_H
#define MEMORY_H

#include "memory_defs.h"

namespace fallout {

char* internal_strdup(const char* string);
void* internal_malloc(size_t size);
void* internal_realloc(void* ptr, size_t size);
void internal_free(void* ptr);
void mem_check();

// PS2 port: exact engine allocation statistics taken from the block allocator
// that backs every internal_* allocation. The PS2 memory budget and the smoke
// harness reports use these instead of their own per-call accounting, which
// cannot know the real size of a block when it is freed.
size_t mem_get_current_bytes();
size_t mem_get_peak_bytes();
int mem_get_block_count();

} // namespace fallout

#endif /* MEMORY_H */
