#ifndef RG_DEFS_H
#define RG_DEFS_H
/* Core definitions shared by the generated code and the runtime. */

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long long u64;
typedef signed char i8;
typedef short i16;
typedef int i32;
typedef long long i64;

/* Guest image is loaded at its preferred base, so guest VAs map linearly. */
#define RG_GUEST_BASE 0x10000000u

/* Thread information block (fs: segment) lives just below nothing special;
 * it is a fixed guest page the runtime allocates. */
extern u32 RG_FS_BASE;

/* Flat guest memory.  g_membase points at the host buffer that holds guest VA
 * RG_GUEST_BASE, so a guest VA maps to g_membase + (va - RG_GUEST_BASE).  The
 * subtraction keeps the wasm load/store offset immediate non-negative (wasm
 * bounds-checks base+offset without wrapping, so a biased base would trap). */
extern u8 *rg_mem_at(u32 va);        /* bounds-checked slow path (debug) */
extern u8 *g_membase;                 /* host pointer to guest VA RG_GUEST_BASE */

#define GPTR(va) (g_membase + ((va) - RG_GUEST_BASE))

typedef union {
  u64 q[2];
  u32 d[4];
  i32 i[4];
  double f[2];
} XMM;

typedef struct CPU {
  u32 eax, ecx, edx, ebx, esp, ebp, esi, edi;
  /* materialised arithmetic flags (0/1) */
  u8 cf, pf, af, zf, sf, of, df;
  /* x87 */
  double st[8];
  int fpu_top;
  u32 fpu_sw;   /* status word (C0..C3 in bits 8,9,10,14) */
  u32 fpu_cw;   /* control word */
  /* SSE */
  XMM xmm[8];
} CPU;

/* ---- guest memory access (unaligned little-endian) -------------------- */
static inline u32 Rd8(u32 a)  { return *(u8 *)GPTR(a); }
static inline u32 Rd16(u32 a) { u16 v; __builtin_memcpy(&v, GPTR(a), 2); return v; }
static inline u32 Rd32(u32 a) { u32 v; __builtin_memcpy(&v, GPTR(a), 4); return v; }
static inline u64 Rd64(u32 a) { u64 v; __builtin_memcpy(&v, GPTR(a), 8); return v; }
static inline void Wr8(u32 a, u32 v)  { *(u8 *)GPTR(a) = (u8)v; }
static inline void Wr16(u32 a, u32 v) { u16 t = (u16)v; __builtin_memcpy(GPTR(a), &t, 2); }
static inline void Wr32(u32 a, u32 v) { __builtin_memcpy(GPTR(a), &v, 4); }
static inline void Wr64(u32 a, u64 v) { __builtin_memcpy(GPTR(a), &v, 8); }

static inline float  rg_rdf32(u32 a) { float v;  __builtin_memcpy(&v, GPTR(a), 4); return v; }
static inline double rg_rdf64(u32 a) { double v; __builtin_memcpy(&v, GPTR(a), 8); return v; }
static inline void   rg_wrf32(u32 a, float v)  { __builtin_memcpy(GPTR(a), &v, 4); }
static inline void   rg_wrf64(u32 a, double v) { __builtin_memcpy(GPTR(a), &v, 8); }
double rg_rdf80(u32 a);
void   rg_wrf80(u32 a, double v);

static inline u32 rg_parity(u32 r) { return (u32)((__builtin_popcount(r & 0xff) & 1) ^ 1); }

/* ---- x87 helpers ------------------------------------------------------ */
static inline double rg_st(CPU *c, int i) { return c->st[(c->fpu_top + i) & 7]; }
static inline void rg_setst(CPU *c, int i, double v) { c->st[(c->fpu_top + i) & 7] = v; }
static inline void rg_fpush(CPU *c, double v) { c->fpu_top = (c->fpu_top - 1) & 7; c->st[c->fpu_top] = v; }
static inline void rg_fpop(CPU *c) { c->fpu_top = (c->fpu_top + 1) & 7; }
static inline void rg_fxch(CPU *c, int i) { int a=(c->fpu_top)&7, b=(c->fpu_top+i)&7; double t=c->st[a]; c->st[a]=c->st[b]; c->st[b]=t; }
void rg_fcom(CPU *c, double a, double b);
u32  rg_fsw(CPU *c);
void rg_fprem1(CPU *c);
void rg_fprem(CPU *c);
i32  rg_f2i(CPU *c, double v);
i64  rg_f2ll(CPU *c, double v);
double rg_rint(CPU *c, double v);
double rg_fabs(double v);
double rg_sqrt(double v);
double rg_sin(double v);
double rg_cos(double v);
int    rg_isnan(double v);
void   rg_ucomisd(CPU *c, double a, double b);

/* ---- control-flow helpers -------------------------------------------- */
typedef struct { u32 addr; void (*fn)(CPU *); } RGEntry;
typedef struct { const char *name; u32 addr; } RGExport;
typedef struct { u32 slot; u32 index; } RGIatSlot;   /* IAT slot VA -> import # */
extern const RGEntry rg_entries[];
extern const unsigned rg_entry_count;
extern const RGExport rg_exports[];
extern void (*const rg_imp_fns[])(CPU *);            /* indexed import shims */
extern const unsigned rg_imp_count;
extern const RGIatSlot rg_iat[];
extern const unsigned rg_iat_count;

/* Import trampoline address block: a call through an unresolved IAT slot lands
 * on one of these synthetic addresses, which the dispatcher routes to the
 * matching stdcall shim. */
#define RG_IMP_TRAMP   0x101d8000u
#define RG_IMP_STRIDE  16u

void rg_fill_iat(void);
void rg_dispatch(CPU *c, u32 addr);
void rg_cpuid(CPU *c);
void rg_trap(CPU *c, u32 va, const char *what) __attribute__((noreturn));
void rg_unhandled(CPU *c, u32 va, const char *text) __attribute__((noreturn));
void rg_bad_target(CPU *c, u32 addr) __attribute__((noreturn));
void rg_div0(CPU *c) __attribute__((noreturn));

#endif
