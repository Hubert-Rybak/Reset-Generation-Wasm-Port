#ifndef RG_MEM_H
#define RG_MEM_H
#include "rg_defs.h"

/* Guest address-space layout, all within [0x10000000, 0x20000000).  The DLL
 * image sits at its preferred base 0x10000000 and ends below RG_TIB_ADDR. */
#define RG_MEM_SIZE   0x10000000u          /* 256 MiB window */
#define RG_TIB_ADDR   0x101d0000u          /* fs: segment, 1 page */
#define RG_JNI_ENV    0x101d2000u          /* JNIEnv* + function table */
#define RG_JNI_TRAMP  0x101d4000u          /* JNI trampoline address block */
#define RG_JNI_TRAMP_N 512u
#define RG_JNI_TRAMP_STRIDE 16u
#define RG_HEAP_BASE  0x10400000u
#define RG_HEAP_END   0x1f000000u
#define RG_STACK_TOP  0x1fe00000u          /* guest stack grows down from here */
#define RG_STACK_LIMIT 0x1fc00000u         /* 2 MiB of stack */

void rg_mem_init(void);
u32  rg_heap_alloc(u32 size);              /* guest heap (HeapAlloc); 0 if full */
u32  rg_heap_realloc(u32 ptr, u32 size);
void rg_heap_free(u32 ptr);
u32  rg_heap_size(u32 ptr);                /* size originally requested */
u32  rg_heap_brk(void);                    /* current top of the heap */

#endif
