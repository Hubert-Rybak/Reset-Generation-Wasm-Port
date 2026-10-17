/* Guest memory and a simple heap allocator for the Win32/CRT shims. */
#include "rg_mem.h"
#include "rg_host.h"

u8 *g_membase;
u32 RG_FS_BASE;

static u8 *g_buf;

void rg_mem_init(void) {
  g_buf = (u8 *)rg_host_alloc(RG_MEM_SIZE);
  g_membase = g_buf;
  RG_FS_BASE = RG_TIB_ADDR;
}

u8 *rg_mem_at(u32 va) {
  if (va < RG_GUEST_BASE || va >= RG_GUEST_BASE + RG_MEM_SIZE)
    rg_host_panic("guest address out of range");
  return GPTR(va);
}

/* ---- allocator --------------------------------------------------------
 * Block header: [u32 size][u32 flags]; payload follows, 8-byte aligned.
 * A single free list threaded through freed blocks keeps it simple; this is
 * not a hot path (the game allocates big asset buffers up front). */
typedef struct Blk {
  u32 size;   /* payload bytes */
  u32 used;   /* 1 if allocated */
  u32 next;   /* guest addr of next block header (physical order) */
  u32 prevfree;
} Blk;

#define HDR 16u

static u32 g_brk;

static u32 align8(u32 x) { return (x + 7u) & ~7u; }

u32 rg_heap_alloc(u32 size) {
  size = align8(size ? size : 1);
  /* first-fit scan over physical blocks */
  u32 a = RG_HEAP_BASE;
  while (a < g_brk) {
    Blk *b = (Blk *)GPTR(a);
    if (!b->used && b->size >= size) {
      /* split if much larger */
      if (b->size >= size + HDR + 32u) {
        u32 nb = a + HDR + size;
        Blk *n = (Blk *)GPTR(nb);
        n->size = b->size - size - HDR;
        n->used = 0;
        n->next = b->next;
        b->size = size;
        b->next = nb;
      }
      b->used = 1;
      return a + HDR;
    }
    a = b->next;
  }
  /* extend break */
  if (g_brk == 0) g_brk = RG_HEAP_BASE;
  u32 hdr = g_brk;
  u32 end = hdr + HDR + size;
  if (end > RG_HEAP_END) rg_host_panic("guest heap exhausted");
  Blk *b = (Blk *)GPTR(hdr);
  b->size = size;
  b->used = 1;
  b->next = end;
  g_brk = end;
  return hdr + HDR;
}

u32 rg_heap_size(u32 ptr) {
  if (!ptr) return 0;
  Blk *b = (Blk *)GPTR(ptr - HDR);
  return b->size;
}

void rg_heap_free(u32 ptr) {
  if (!ptr) return;
  Blk *b = (Blk *)GPTR(ptr - HDR);
  b->used = 0;
  /* coalesce with the following block if free */
  while (b->next < g_brk) {
    Blk *n = (Blk *)GPTR(b->next);
    if (n->used) break;
    b->size += HDR + n->size;
    b->next = n->next;
  }
}

u32 rg_heap_realloc(u32 ptr, u32 size) {
  if (!ptr) return rg_heap_alloc(size);
  u32 old = rg_heap_size(ptr);
  if (size <= old) return ptr;
  u32 np = rg_heap_alloc(size);
  if (np) {
    __builtin_memcpy(GPTR(np), GPTR(ptr), old);
    rg_heap_free(ptr);
  }
  return np;
}
