#ifndef RG_MEM_H
#define RG_MEM_H
#include "rg_defs.h"

/* Guest address-space layout, all within [0x10000000, 0x20000000). */
#define RG_MEM_SIZE   0x10000000u          /* 256 MiB window */
#define RG_IMAGE_END  0x101d0000u          /* image rounded up */
#define RG_TIB_ADDR   0x101d0000u          /* fs: segment, 1 page */
#define RG_JNI_ENV    0x101d2000u          /* JNIEnv* + function table */
#define RG_JNI_TRAMP  0x101d4000u          /* JNI trampoline address block */
#define RG_JNI_TRAMP_N 512u
#define RG_JNI_TRAMP_STRIDE 16u
#define RG_HEAP_BASE  0x10400000u
#define RG_HEAP_END   0x1f000000u
#define RG_STACK_TOP  0x1fe00000u
#define RG_STACK_MIN  0x1fc00000u

extern u8 *g_membase;      /* host addr of guest VA RG_GUEST_BASE (rg_defs.h) */
extern u32 RG_FS_BASE;

void rg_mem_init(void);
u32  rg_heap_alloc(u32 size);              /* bump/free allocator (HeapAlloc) */
u32  rg_heap_realloc(u32 ptr, u32 size);
void rg_heap_free(u32 ptr);
u32  rg_heap_size(u32 ptr);

#endif
