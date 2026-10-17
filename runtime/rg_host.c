/* Host interface for the wasm build: a bump allocator over linear memory, a
 * tiny libc subset, and math routines (freestanding wasm has no libc). */
#include "rg_host.h"

/* ---- JS imports ------------------------------------------------------- */
__attribute__((import_module("env"), import_name("js_log")))
extern void js_log(u32 ptr, u32 len);
__attribute__((import_module("env"), import_name("js_now")))
extern double js_now(void);
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
  if (g_top == 0) g_top = ((u32)(&__heap_base) + 15u) & ~15u;
  u32 p = (g_top + 15u) & ~15u;
  grow_to(p + size);
  g_top = p + size;
  return (void *)p;
}

void rg_host_panic(const char *msg) {
  js_abort((u32)msg, rg_strlen(msg));
  __builtin_unreachable();
}

void rg_host_log(const char *msg) { js_log((u32)msg, rg_strlen(msg)); }
void rg_host_logn(const char *msg, u32 len) { js_log((u32)msg, len); }
double rg_host_now_ms(void) { return js_now(); }

/* ---- libc subset (also satisfies compiler-emitted mem* calls) --------- */
void *memcpy(void *d, const void *s, unsigned long n) {
  u8 *dd = d; const u8 *ss = s;
  for (unsigned long i = 0; i < n; i++) dd[i] = ss[i];
  return d;
}
void *memmove(void *d, const void *s, unsigned long n) {
  u8 *dd = d; const u8 *ss = s;
  if (dd < ss) { for (unsigned long i = 0; i < n; i++) dd[i] = ss[i]; }
  else { for (unsigned long i = n; i; i--) dd[i-1] = ss[i-1]; }
  return d;
}
void *memset(void *d, int v, unsigned long n) {
  u8 *dd = d;
  for (unsigned long i = 0; i < n; i++) dd[i] = (u8)v;
  return d;
}
int memcmp(const void *a, const void *b, unsigned long n) {
  const u8 *x = a, *y = b;
  for (unsigned long i = 0; i < n; i++) if (x[i] != y[i]) return (int)x[i] - (int)y[i];
  return 0;
}
void *rg_memcpy(void *d, const void *s, u32 n) { return memcpy(d, s, n); }
void *rg_memset(void *d, int v, u32 n) { return memset(d, v, n); }
int rg_memcmp(const void *a, const void *b, u32 n) { return memcmp(a, b, n); }
u32 rg_strlen(const char *s) { u32 n = 0; while (s[n]) n++; return n; }

/* ---- math ------------------------------------------------------------- */
double rg_m_floor(double x) {
  double t = (double)(i64)x;
  if (t > x) t -= 1.0;
  return t;
}
double rg_m_fmod(double a, double b) {
  if (b == 0.0) return 0.0;
  double q = a / b;
  double n = (double)(i64)q;       /* truncate toward zero */
  return a - n * b;
}
/* sin/cos via Payne-Hanek-lite range reduction and a degree-11/10 minimax. */
#define RG_PI    3.14159265358979323846
#define RG_2PI   6.28318530717958647692
#define RG_PI_2  1.57079632679489661923
static double sin_poly(double x) { /* x in [-pi/4, pi/4] */
  double x2 = x * x;
  return x * (1.0 + x2 * (-1.66666666666666307295e-1 + x2 * (8.33333333332211858878e-3
       + x2 * (-1.98412698295895385996e-4 + x2 * (2.75573136213857245213e-6
       + x2 * (-2.50507477628578072866e-8 + x2 * 1.58962301576546568060e-10))))));
}
static double cos_poly(double x) { /* x in [-pi/4, pi/4] */
  double x2 = x * x;
  return 1.0 + x2 * (-0.5 + x2 * (4.16666666666665929218e-2 + x2 * (-1.38888888888730564116e-3
       + x2 * (2.48015872888517045348e-5 + x2 * (-2.75573141792967388112e-7
       + x2 * 2.08757008419747316778e-9)))));
}
double rg_m_sin(double x) {
  /* reduce x to quadrant */
  double q = x / RG_PI_2;
  i64 k = (i64)(q + (q < 0 ? -0.5 : 0.5));
  double y = x - (double)k * RG_PI_2;
  switch ((int)(k & 3)) {
    case 0: return sin_poly(y);
    case 1: return cos_poly(y);
    case 2: return -sin_poly(y);
    default: return -cos_poly(y);
  }
}
double rg_m_cos(double x) { return rg_m_sin(x + RG_PI_2); }
