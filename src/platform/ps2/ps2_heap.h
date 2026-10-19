/*
 * PS2 low-level EE memory allocator.
 *
 * Wraps the PS2 SDK's malloc/free (which uses the EE main heap) with
 * alignment and debugging helpers. All engine allocations ultimately
 * route through here.
 */

#ifndef PS2_HEAP_H_
#define PS2_HEAP_H_

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Allocate `size` bytes from the EE main heap. Returns NULL on OOM.
 * Alignment is at least 8 bytes (PS2 SDK malloc default). */
void* ps2_mem_malloc(size_t size);

/* Reallocate `ptr` to `size` bytes. Returns NULL on OOM (original unchanged). */
void* ps2_mem_realloc(void* ptr, size_t size);

/* Free memory returned by ps2_mem_malloc/realloc. */
void ps2_mem_free(void* ptr);

/* Aligned allocation for DMA/buffer requirements. */
void* ps2_mem_memalign(size_t alignment, size_t size);

/* Get total EE heap free space (approximate). */
size_t ps2_mem_free_space(void);

/* Initialize default memory manager procs (safe to call multiple times). */
void ps2_mem_init_procs(void);

/* Print memory usage statistics to stdout. */
void ps2_mem_print_stats(void);

#ifdef __cplusplus
}
#endif

#endif /* PS2_HEAP_H_ */