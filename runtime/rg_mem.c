/* Guest memory and a simple heap allocator for the Win32/CRT shims. */
#include "rg_mem.h"
#include "rg_host.h"

u8 *g_membase;
u32 RG_FS_BASE;

void rg_mem_init(void) {
  g_membase = (u8 *)rg_host_alloc(RG_MEM_SIZE);
  RG_FS_BASE = RG_TIB_ADDR;
}

/* ---- allocator --------------------------------------------------------
 * Blocks are laid out back to back from RG_HEAP_BASE up to the break, each
 * with a 16-byte header; payloads are 8-byte aligned.  Allocation is first
 * fit over the block list, merging runs of free blocks as it walks them, so
 * freed neighbours coalesce no matter in which order they were released. */
typedef struct Blk {
  u32 size;   /* payload capacity in bytes */
  u32 used;   /* 1 if allocated */
  u32 next;   /* guest addr of the next block header (physical order) */
  u32 req;    /* size the caller asked for (what HeapSize reports) */
} Blk;

#define HDR 16u
#define MIN_SPLIT 32u

static u32 g_brk = RG_HEAP_BASE;

static u32 align8(u32 x) { return (x + 7u) & ~7u; }
static Blk *blk(u32 a) { return (Blk *)GPTR(a); }

/* Absorb any free blocks that follow the block at `a`. */
static void merge_following(u32 a) {
  Blk *b = blk(a);
  while (b->next < g_brk && !blk(b->next)->used) {
    Blk *n = blk(b->next);
    b->size += HDR + n->size;
    b->next = n->next;
  }
}

/* Shrink block `a` to `size` bytes, returning the tail as a free block. */
static void split(u32 a, u32 size) {
  Blk *b = blk(a);
  if (b->size < size + HDR + MIN_SPLIT) return;
  u32 na = a + HDR + size;
  Blk *n = blk(na);
  n->size = b->size - size - HDR;
  n->used = 0;
  n->next = b->next;
  b->size = size;
  b->next = na;
}

u32 rg_heap_alloc(u32 req) {
  if (req > RG_HEAP_END - RG_HEAP_BASE) return 0;
  u32 size = align8(req ? req : 1);
  for (u32 a = RG_HEAP_BASE; a < g_brk; a = blk(a)->next) {
    if (blk(a)->used) continue;
    merge_following(a);
    if (blk(a)->size >= size) {
      split(a, size);
      blk(a)->used = 1;
      blk(a)->req = req;
      return a + HDR;
    }
  }
  /* extend the break */
  u32 a = g_brk, end = a + HDR + size;
  if (end > RG_HEAP_END) return 0;
  Blk *b = blk(a);
  b->size = size;
  b->used = 1;
  b->next = end;
  b->req = req;
  g_brk = end;
  return a + HDR;
}

u32 rg_heap_size(u32 ptr) {
  return ptr ? blk(ptr - HDR)->req : 0;
}

void rg_heap_free(u32 ptr) {
  if (!ptr) return;
  u32 a = ptr - HDR;
  blk(a)->used = 0;
  merge_following(a);
  if (blk(a)->next >= g_brk) g_brk = a;   /* trailing free block: lower the break */
}

u32 rg_heap_realloc(u32 ptr, u32 req) {
  if (!ptr) return rg_heap_alloc(req);
  if (req > RG_HEAP_END - RG_HEAP_BASE) return 0;
  u32 a = ptr - HDR, want = align8(req ? req : 1), old = blk(a)->req;
  Blk *b = blk(a);
  /* shrink, or grow in place into free neighbours or at the break */
  merge_following(a);
  if (b->size >= want) { split(a, want); b->req = req; return ptr; }
  if (b->next >= g_brk && a + HDR + want <= RG_HEAP_END) {
    b->size = want;
    b->next = g_brk = a + HDR + want;
    b->req = req;
    return ptr;
  }
  u32 np = rg_heap_alloc(req);
  if (np) {
    rg_memcpy(GPTR(np), GPTR(ptr), old);
    rg_heap_free(ptr);
  }
  return np;
}

u32 rg_heap_brk(void) { return g_brk; }
