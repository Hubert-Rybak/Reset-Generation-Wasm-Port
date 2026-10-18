/* CPU support routines: dispatch, x87/SSE helpers, cpuid, traps. */
#include "rg_defs.h"
#include "rg_mem.h"
#include "rg_host.h"
#include "rg_math.h"

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
static char *put(char *p, const char *s) {
  while (*s && p < g_msg + sizeof g_msg - 1) *p++ = *s++;
  return p;
}
static char *puthex(char *p, u32 v) {
  for (int i = 7; i >= 0 && p < g_msg + sizeof g_msg - 1; i--) {
    int d = (v >> (i * 4)) & 0xf;
    *p++ = (char)(d < 10 ? '0' + d : 'a' + d - 10);
  }
  return p;
}
__attribute__((noreturn)) static void panic3(const char *a, u32 va, const char *b) {
  char *p = puthex(put(g_msg, a), va);
  if (b) p = put(put(p, " "), b);
  *p = 0;
  rg_host_panic(g_msg);
}
void rg_trap(CPU *c, u32 va, const char *what) { (void)c; panic3("RG trap @", va, what); }
void rg_unhandled(CPU *c, u32 va, const char *text) { (void)c; panic3("RG unhandled @", va, text); }
void rg_bad_target(CPU *c, u32 addr) { (void)c; panic3("RG bad indirect target ", addr, 0); }
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
void rg_fcom(CPU *c, double a, double b) {
  u32 sw = c->fpu_sw & ~(FSW_C0 | FSW_C1 | FSW_C2 | FSW_C3);
  if (a != a || b != b) sw |= FSW_C0 | FSW_C2 | FSW_C3;   /* unordered */
  else if (a < b) sw |= FSW_C0;
  else if (a == b) sw |= FSW_C3;
  c->fpu_sw = sw;
}
u32 rg_fsw(CPU *c) {
  return (c->fpu_sw & ~(7u << 11)) | ((u32)(c->fpu_top & 7) << 11);
}
void rg_ucomisd(CPU *c, double a, double b) {
  c->of = c->sf = c->af = 0;
  if (a != a || b != b) { c->zf = c->pf = c->cf = 1; }
  else if (a < b)       { c->zf = 0; c->pf = 0; c->cf = 1; }
  else if (a == b)      { c->zf = 1; c->pf = 0; c->cf = 0; }
  else                  { c->zf = 0; c->pf = 0; c->cf = 0; }
}

/* Round to an integral value per the control word's rounding field. */
static double fpu_round(CPU *c, double v) {
  switch ((c->fpu_cw >> 10) & 3) {
    case 0:  return __builtin_rint(v);    /* nearest, ties to even */
    case 1:  return __builtin_floor(v);
    case 2:  return __builtin_ceil(v);
    default: return __builtin_trunc(v);
  }
}
double rg_rint(CPU *c, double v) { return fpu_round(c, v); }

/* FIST with the invalid exception masked: NaN or an out-of-range value
 * stores the "integer indefinite" (most negative) value. */
i32 rg_f2i(CPU *c, double v) {
  double r = fpu_round(c, v);
  if (!(r >= -2147483648.0 && r < 2147483648.0)) return (i32)0x80000000u;
  return (i32)r;
}
i64 rg_f2ll(CPU *c, double v) {
  double r = fpu_round(c, v);
  if (!(r >= -9223372036854775808.0 && r < 9223372036854775808.0))
    return (i64)0x8000000000000000ull;
  return (i64)r;
}

/* FPREM/FPREM1 always finish in one step here (C2 = 0); the quotient bits
 * in C0/C1/C3 are not modelled. */
void rg_fprem(CPU *c) {
  rg_setst(c, 0, rg_m_fmod(rg_st(c, 0), rg_st(c, 1)));
  c->fpu_sw &= ~FSW_C2;
}
void rg_fprem1(CPU *c) {
  rg_setst(c, 0, rg_m_remainder(rg_st(c, 0), rg_st(c, 1)));
  c->fpu_sw &= ~FSW_C2;
}

/* FSIN/FCOS: like the hardware, an operand with |x| >= 2^63 is left in place
 * with C2 set so the caller can reduce it with FPREM1. */
static int trig_out_of_range(CPU *c, double x) {
  c->fpu_sw &= ~FSW_C2;
  if (__builtin_fabs(x) >= 0x1p63 && x - x == 0) { c->fpu_sw |= FSW_C2; return 1; }
  return 0;
}
void rg_fsin(CPU *c) {
  double x = rg_st(c, 0);
  if (!trig_out_of_range(c, x)) rg_setst(c, 0, rg_m_sin(x));
}
void rg_fcos(CPU *c) {
  double x = rg_st(c, 0);
  if (!trig_out_of_range(c, x)) rg_setst(c, 0, rg_m_cos(x));
}

/* ---- 80-bit extended <-> double -------------------------------------- */
/* m * 2^e rounded to the nearest double (ties to even), m != 0. */
static double scaled_to_double(u64 m, int e) {
  int lz = __builtin_clzll(m);
  m <<= lz; e -= lz;                    /* m in [2^63, 2^64) */
  int be = e + 63 + 1023, sh = 11;      /* biased exponent; bits to drop */
  if (be >= 2047) return __builtin_inf();
  if (be < 1) { sh += 1 - be; be = 0; } /* subnormal result */
  if (sh > 64) return 0.0;              /* below half the smallest subnormal */
  u64 q = sh == 64 ? 0 : m >> sh, rem = sh == 64 ? m : m & ((1ull << sh) - 1);
  u64 half = 1ull << (sh - 1);
  if (rem > half || (rem == half && (q & 1))) q++;
  u64 bits;
  if (be == 0) bits = q;                /* a carry into bit 52 yields the smallest normal */
  else {
    if (q >> 53) { q >>= 1; if (++be >= 2047) return __builtin_inf(); }
    bits = ((u64)be << 52) | (q & 0xfffffffffffffull);
  }
  double v;
  __builtin_memcpy(&v, &bits, 8);
  return v;
}
double rg_rdf80(u32 a) {
  u64 m = Rd64(a);
  u32 se = Rd16(a + 8);
  int e = (int)(se & 0x7fff);
  double v;
  if (e == 0x7fff) v = (m << 1) ? __builtin_nan("") : __builtin_inf();
  else if (m == 0) v = 0.0;
  else v = scaled_to_double(m, (e ? e : 1) - 16383 - 63);
  return (se & 0x8000) ? -v : v;
}
void rg_wrf80(u32 a, double v) {
  u64 b;
  __builtin_memcpy(&b, &v, 8);
  u32 sign = (u32)(b >> 63) << 15, e = (u32)(b >> 52) & 0x7ff, ex;
  u64 f = b & 0xfffffffffffffull, m;
  if (e == 0x7ff) { ex = 0x7fff; m = 0x8000000000000000ull | (f << 11); }   /* inf / nan */
  else if (e == 0 && f == 0) { ex = 0; m = 0; }
  else if (e == 0) {                       /* subnormal double: normalise */
    int lz = __builtin_clzll(f);
    m = f << lz;
    ex = (u32)(16383 + 63 - 1074 - lz);
  } else { ex = e - 1023 + 16383; m = 0x8000000000000000ull | (f << 11); }
  Wr64(a, m);
  Wr16(a + 8, sign | ex);
}
