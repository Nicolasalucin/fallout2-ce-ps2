#ifndef PS2_MEM_H_
#define PS2_MEM_H_

/*
 * EE heap instrumentation for the Fallout PS2 port.
 *
 * Reports the system allocator's view (newlib mallinfo + the sbrk heap
 * high-water mark) so the smoke suite can watch the 32 MB RDRAM budget
 * while it hops maps. See ps2_mem.c for details.
 */

#ifdef __cplusplus
extern "C" {
#endif

/* Snapshots the sbrk baseline (call once, early). */
void ps2_mem_init(void);

/* Prints one heap snapshot as a "[ps2][smoke] mem:" line. */
void ps2_mem_report(const char* tag);

/* mallinfo() views, in KB. */
int ps2_mem_used_kb(void);
int ps2_mem_free_kb(void);

/* Heap break growth since ps2_mem_init(), in KB (never shrinks: high-water
 * mark of the newlib heap). */
int ps2_mem_sbrk_kb(void);

#ifdef __cplusplus
}
#endif

#endif /* PS2_MEM_H_ */
