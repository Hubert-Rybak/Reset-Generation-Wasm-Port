/* CPU support routines: dispatch, x87/SSE helpers, cpuid, traps. */
#include "rg_defs.h"
#include "rg_mem.h"
#include "rg_host.h"

/* ---- indirect dispatch ------------------------------------------------ */
void rg_jni_call(CPU *c, u32 index);   /* in rg_jni.c */

void rg_fill_iat(void) {
  for (unsigned i = 0; i < rg_iat_count; i++)
    Wr32(rg_iat[i].slot, RG_IMP_TRAMP + rg_iat[i].index * RG_IMP_STRIDE);
}

void rg_dispatch(CPU *c, u32 addr) {
  if (addr >= RG_JNI_TRAMP && addr < RG_JNI_TRAMP + RG_JNI_TRAMP_N * RG_JNI_TRAMP_STRIDE) {
    rg_jni_call(c, (addr - RG_JNI_TRAMP) / RG_JNI_TRAMP_STRIDE);
    return;
  }
  if (addr >= RG_IMP_TRAMP && addr < RG_IMP_TRAMP + rg_imp_count * RG_IMP_STRIDE) {
    rg_imp_fns[(addr - RG_IMP_TRAMP) / RG_IMP_STRIDE](c);
    return;
  }
  u32 lo = 0, hi = rg_entry_count;
  while (lo < hi) {
    u32 mid = (lo + hi) >> 1;
    u32 a = rg_entries[mid].addr;
    if (a == addr) { rg_entries[mid].fn(c); return; }
    if (a < addr) lo = mid + 1; else hi = mid;
  }
  rg_bad_target(c, addr);
}

/* ---- traps ------------------------------------------------------------ */
static char g_msg[256];
static void fmthex(char *p, u32 v) {
  for (int i = 7; i >= 0; i--) { int d = (v >> (i*4)) & 0xf; *p++ = d < 10 ? '0'+d : 'a'+d-10; }
}
void rg_trap(CPU *c, u32 va, const char *what) {
  (void)c; const char *q = "RG trap @"; char *p = g_msg;
  while (*q) *p++ = *q++; fmthex(p, va); p += 8; *p++ = ' ';
  while (*what) *p++ = *what++; *p = 0;
  rg_host_panic(g_msg);
}
void rg_unhandled(CPU *c, u32 va, const char *text) {
  (void)c; const char *q = "RG unhandled @"; char *p = g_msg;
  while (*q) *p++ = *q++; fmthex(p, va); p += 8; *p++ = ' ';
  while (*text && p < g_msg+250) *p++ = *text++; *p = 0;
  rg_host_panic(g_msg);
}
void rg_bad_target(CPU *c, u32 addr) {
  (void)c; const char *q = "RG bad indirect target "; char *p = g_msg;
  while (*q) *p++ = *q++; fmthex(p, addr); p += 8; *p = 0;
  rg_host_panic(g_msg);
}
void rg_div0(CPU *c) { (void)c; rg_host_panic("RG divide by zero"); }

/* ---- cpuid ------------------------------------------------------------ */
void rg_cpuid(CPU *c) {
  u32 leaf = c->eax;
  if (leaf == 0) {
    c->eax = 1;                       /* max standard leaf */
    c->ebx = 0x756e6547;              /* "Genu" */
    c->edx = 0x49656e69;              /* "ineI" */
    c->ecx = 0x6c65746e;              /* "ntel" */
  } else {
    c->eax = 0x000006d8;              /* family 6 */
    c->ebx = 0;
    c->ecx = 0;
    /* FPU|TSC|CMOV|MMX|FXSR|SSE|SSE2 */
    c->edx = (1u<<0)|(1u<<4)|(1u<<15)|(1u<<23)|(1u<<24)|(1u<<25)|(1u<<26);
  }
}

/* ---- x87 helpers ------------------------------------------------------ */
int rg_isnan(double v) { return v != v; }
double rg_fabs(double v) { return v < 0 ? -v : v; }
double rg_sqrt(double v) { return __builtin_sqrt(v); }
double rg_sin(double v) { return rg_m_sin(v); }
double rg_cos(double v) { return rg_m_cos(v); }

void rg_fcom(CPU *c, double a, double b) {
  u32 sw = c->fpu_sw & ~((1u<<8)|(1u<<10)|(1u<<14));  /* clear C0,C2,C3 */
  if (a != a || b != b) sw |= (1u<<8)|(1u<<10)|(1u<<14);  /* unordered */
  else if (a < b) sw |= (1u<<8);                           /* C0 */
  else if (a == b) sw |= (1u<<14);                         /* C3 */
  /* a > b leaves all clear */
  c->fpu_sw = sw;
}
u32 rg_fsw(CPU *c) {
  return (c->fpu_sw & 0x47ffu) | ((u32)(c->fpu_top & 7) << 11);
}
void rg_ucomisd(CPU *c, double a, double b) {
  c->of = c->sf = c->af = 0;
  if (a != a || b != b) { c->zf = c->pf = c->cf = 1; }
  else if (a < b)       { c->zf = 0; c->pf = 0; c->cf = 1; }
  else if (a == b)      { c->zf = 1; c->pf = 0; c->cf = 0; }
  else                  { c->zf = 0; c->pf = 0; c->cf = 0; }
}

static double round_mode(CPU *c, double v) {
  u32 rc = (c->fpu_cw >> 10) & 3;
  switch (rc) {
    case 0: {  /* nearest, ties to even */
      double f = rg_m_floor(v), d = v - f;
      if (d < 0.5) return f;
      if (d > 0.5) return f + 1.0;
      double e = f * 0.5; return (e == rg_m_floor(e)) ? f : f + 1.0;
    }
    case 1: return rg_m_floor(v);                       /* down */
    case 2: { double f = rg_m_floor(v); return (f == v) ? f : f + 1.0; }  /* up */
    default: return (double)(i64)v;                     /* truncate */
  }
}
i32 rg_f2i(CPU *c, double v) { return (i32)round_mode(c, v); }
i64 rg_f2ll(CPU *c, double v) { return (i64)round_mode(c, v); }
double rg_rint(CPU *c, double v) { return round_mode(c, v); }

void rg_fprem1(CPU *c) {
  double a = rg_st(c, 0), b = rg_st(c, 1);
  rg_setst(c, 0, rg_m_fmod(a, b));
  c->fpu_sw &= ~(1u<<10);  /* C2 = 0: reduction complete */
}
void rg_fprem(CPU *c) { rg_fprem1(c); }

/* 80-bit extended <-> double. */
double rg_rdf80(u32 a) {
  u64 mant = Rd64(a);
  u32 se = Rd16(a + 8);
  int sign = (se >> 15) & 1;
  int exp = se & 0x7fff;
  if (exp == 0 && mant == 0) return sign ? -0.0 : 0.0;
  double m = (double)mant * (1.0 / 18446744073709551616.0);  /* mant / 2^64 */
  /* value = m * 2^(exp-16382) ; integer bit already in mant (bit 63) */
  double val = m;
  int e = exp - 16382;
  while (e > 0) { val *= 2.0; e--; }
  while (e < 0) { val *= 0.5; e++; }
  return sign ? -val : val;
}
void rg_wrf80(u32 a, double v) {
  int sign = 0;
  if (v < 0 || (v == 0 && 1.0/v < 0)) { sign = 1; v = -v; }
  u64 mant = 0; u32 se = 0;
  if (v != 0.0 && v == v) {
    int e = 16383;
    while (v >= 2.0) { v *= 0.5; e++; }
    while (v < 1.0)  { v *= 2.0; e--; }
    mant = (u64)(v * 9223372036854775808.0) << 1;  /* v*2^63, shift to bit63 */
    mant |= 0x8000000000000000ull;
    se = (u32)(e & 0x7fff);
  } else if (v != v) { mant = 0xc000000000000000ull; se = 0x7fff; }
  se |= (u32)sign << 15;
  Wr64(a, mant);
  Wr16(a + 8, (u16)se);
}
