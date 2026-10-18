/* Double-precision math for the x87 helpers.  Freestanding wasm has no libm,
 * so the few transcendental and remainder operations the FPU emulation needs
 * are implemented here (fmod after musl, sin/cos kernels and the
 * Cody-Waite pi/2 constants after fdlibm). */
#include "rg_math.h"

typedef union { double f; u64 i; } dbits;

/* Exact x - trunc(x/y)*y. */
double rg_m_fmod(double x, double y) {
  dbits ux = { x }, uy = { y };
  int ex = (int)(ux.i >> 52 & 0x7ff), ey = (int)(uy.i >> 52 & 0x7ff);
  u64 sx = ux.i >> 63, uxi = ux.i, i;

  if (uy.i << 1 == 0 || y != y || ex == 0x7ff) return (x * y) / (x * y);
  if (uxi << 1 <= uy.i << 1) {
    if (uxi << 1 == uy.i << 1) return 0 * x;
    return x;
  }
  /* normalise both significands */
  if (!ex) { for (i = uxi << 12; i >> 63 == 0; ex--, i <<= 1) {} uxi <<= -ex + 1; }
  else { uxi &= -1ULL >> 12; uxi |= 1ULL << 52; }
  if (!ey) { for (i = uy.i << 12; i >> 63 == 0; ey--, i <<= 1) {} uy.i <<= -ey + 1; }
  else { uy.i &= -1ULL >> 12; uy.i |= 1ULL << 52; }

  for (; ex > ey; ex--) {
    i = uxi - uy.i;
    if (i >> 63 == 0) { if (i == 0) return 0 * x; uxi = i; }
    uxi <<= 1;
  }
  i = uxi - uy.i;
  if (i >> 63 == 0) { if (i == 0) return 0 * x; uxi = i; }
  for (; uxi >> 52 == 0; uxi <<= 1, ex--) {}

  if (ex > 0) { uxi -= 1ULL << 52; uxi |= (u64)ex << 52; }
  else uxi >>= -ex + 1;
  uxi |= sx << 63;
  ux.i = uxi;
  return ux.f;
}

/* IEEE remainder: x - n*y with n = x/y rounded to nearest, ties to even. */
double rg_m_remainder(double x, double p) {
  dbits ux = { x };
  u64 sx = ux.i >> 63;
  if (p != p || x != x) return x * p;
  if (p == 0 || x - x != 0) return (x * p) / (x * p);
  p = __builtin_fabs(p);
  if (p <= 0x1p1022) x = rg_m_fmod(x, p + p);      /* now |x| < 2p */
  if (__builtin_fabs(x) == p) return 0 * x;
  x = __builtin_fabs(x);
  if (p < 0x1p-1021) {
    if (x + x > p) { x -= p; if (x + x >= p) x -= p; }
  } else {
    double half = 0.5 * p;
    if (x > half) { x -= p; if (x >= half) x -= p; }
  }
  ux.f = x;
  ux.i ^= sx << 63;
  return ux.f;
}

/* ---- sin / cos ------------------------------------------------------- */
static const double
  invpio2 = 6.36619772367581382433e-01,
  pio2_1  = 1.57079632673412561417e+00,   /* first 33 bits of pi/2 */
  pio2_1t = 6.07710050650619224932e-11,   /* pi/2 - pio2_1 */
  pio2_2  = 6.07710050630396597660e-11,   /* second 33 bits of pi/2 */
  pio2_2t = 2.02226624879595063154e-21,   /* pi/2 - (pio2_1+pio2_2) */
  pio2_3  = 2.02226624871116645580e-21,   /* third 33 bits of pi/2 */
  pio2_3t = 8.47842766036889956997e-32;   /* pi/2 - (pio2_1+pio2_2+pio2_3) */

static const double
  S1 = -1.66666666666666324348e-01, S2 =  8.33333333332248946124e-03,
  S3 = -1.98412698298579493134e-04, S4 =  2.75573137070700676789e-06,
  S5 = -2.50507602534068634195e-08, S6 =  1.58969099521155010221e-10,
  C1 =  4.16666666666666019037e-02, C2 = -1.38888888888741095749e-03,
  C3 =  2.48015872894767294178e-05, C4 = -2.75573143513906633035e-07,
  C5 =  2.08757232129817482790e-09, C6 = -1.13596475577881948265e-11;

/* sin(x+y) for |x| <= pi/4, y the tail of x */
static double k_sin(double x, double y) {
  double z = x * x, w = z * z;
  double r = S2 + z * (S3 + z * S4) + z * w * (S5 + z * S6);
  double v = z * x;
  return x - ((z * (0.5 * y - v * r) - y) - v * S1);
}

/* cos(x+y) for |x| <= pi/4 */
static double k_cos(double x, double y) {
  double z = x * x, w = z * z;
  double r = z * (C1 + z * (C2 + z * C3)) + w * w * (C4 + z * (C5 + z * C6));
  double hz = 0.5 * z;
  w = 1.0 - hz;
  return w + (((1.0 - w) - hz) + (z * r - x * y));
}

/* x = n*pi/2 + y[0] + y[1] with |y| <= pi/4; returns n mod 4.  Three-stage
 * Cody-Waite reduction: accurate for |x| up to about 2^20 * pi/2, still
 * well-defined (if imprecise) beyond that. */
static int rem_pio2(double x, double *y) {
  if (__builtin_fabs(x) <= 0.78539816339744830962) { y[0] = x; y[1] = 0; return 0; }
  double fn = __builtin_rint(x * invpio2);
  double t, w, r = x - fn * pio2_1;        /* exact while |fn| < 2^20 */
  /* fdlibm's 2nd and 3rd refinement steps, applied unconditionally */
  t = r; w = fn * pio2_2; r = t - w; w = fn * pio2_2t - ((t - r) - w);
  t = r; w = fn * pio2_3; r = t - w; w = fn * pio2_3t - ((t - r) - w);
  y[0] = r - w;
  y[1] = (r - y[0]) - w;
  return (int)((i64)fn & 3);
}

double rg_m_sin(double x) {
  double y[2];
  if (x - x != 0) return x - x;   /* inf or nan -> nan */
  switch (rem_pio2(x, y)) {
    case 0: return k_sin(y[0], y[1]);
    case 1: return k_cos(y[0], y[1]);
    case 2: return -k_sin(y[0], y[1]);
    default: return -k_cos(y[0], y[1]);
  }
}

double rg_m_cos(double x) {
  double y[2];
  if (x - x != 0) return x - x;
  switch (rem_pio2(x, y)) {
    case 0: return k_cos(y[0], y[1]);
    case 1: return -k_sin(y[0], y[1]);
    case 2: return -k_cos(y[0], y[1]);
    default: return k_sin(y[0], y[1]);
  }
}
