/* Host interface for the wasm build: a bump allocator over linear memory, the
 * JS imports, and a tiny libc subset (freestanding wasm has no libc). */
#include "rg_host.h"

/* ---- JS imports ------------------------------------------------------- */
__attribute__((import_module("env"), import_name("js_log")))
extern void js_log(u32 ptr, u32 len);
__attribute__((import_module("env"), import_name("js_now")))
extern double js_now(void);
__attribute__((import_module("env"), import_name("js_wall_ms")))
extern double js_wall_ms(void);
__attribute__((import_module("env"), import_name("js_abort")))
extern void js_abort(u32 ptr, u32 len);

/* ---- linear-memory bump allocator ------------------------------------ */
extern unsigned char __heap_base;   /* provided by wasm-ld */
static u32 g_top;                   /* next free byte */

static void grow_to(u32 want) {
  u32 have = __builtin_wasm_memory_size(0) * 65536u;
  if (want <= have) return;
  u32 need_pages = (want - have + 65535u) / 65536u;
  if (__builtin_wasm_memory_grow(0, need_pages) == (unsigned)-1)
    rg_host_panic("memory.grow failed");
}

void *rg_host_alloc(u32 size) {
  if (g_top == 0) g_top = (u32)(&__heap_base);
  u32 p = (g_top + 15u) & ~15u;
  grow_to(p + size);
  g_top = p + size;
  return (void *)p;
}

void rg_host_panic(const char *msg) {
  js_abort((u32)msg, rg_strlen(msg));
  __builtin_unreachable();
}

void rg_host_logn(const char *msg, u32 len) { js_log((u32)msg, len); }
double rg_host_now_ms(void) { return js_now(); }
double rg_host_wall_ms(void) { return js_wall_ms(); }

/* ---- libc subset (also satisfies compiler-emitted mem* calls) ---------
 * Built with -mbulk-memory, so these lower to memory.copy / memory.fill. */
void *memcpy(void *d, const void *s, unsigned long n) { __builtin_memmove(d, s, n); return d; }
void *memmove(void *d, const void *s, unsigned long n) { __builtin_memmove(d, s, n); return d; }
void *memset(void *d, int v, unsigned long n) { __builtin_memset(d, v, n); return d; }
int memcmp(const void *a, const void *b, unsigned long n) {
  const u8 *x = a, *y = b;
  for (unsigned long i = 0; i < n; i++) if (x[i] != y[i]) return (int)x[i] - (int)y[i];
  return 0;
}
void *rg_memcpy(void *d, const void *s, u32 n) { return memcpy(d, s, n); }
void *rg_memset(void *d, int v, u32 n) { return memset(d, v, n); }
u32 rg_strlen(const char *s) { u32 n = 0; while (s[n]) n++; return n; }
