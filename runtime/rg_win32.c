/* Win32 / CRT import shims (stdcall).  Each reads its arguments from the guest
 * stack, performs the work against the runtime's memory/filesystem, puts the
 * result in EAX, and pops the return address plus its arguments. */
#include "rg_defs.h"
#include "rg_imports.h"
#include "rg_mem.h"
#include "rg_fs.h"
#include "rg_host.h"
#include "rg_win32.h"

#define A(i) Rd32(c->esp + 4u + 4u * (i))
#define RET(nargs, val) do { c->eax = (u32)(val); c->esp += 4u + 4u * (nargs); return; } while (0)
#define RETV(nargs)     do { c->esp += 4u + 4u * (nargs); return; } while (0)
#define FAIL(nargs, val, err) do { g_lasterr = (err); RET(nargs, val); } while (0)

enum {
  ERROR_FILE_NOT_FOUND = 2, ERROR_TOO_MANY_OPEN_FILES = 4, ERROR_INVALID_HANDLE = 6,
  ERROR_NOT_ENOUGH_MEMORY = 8, ERROR_NO_MORE_FILES = 18, ERROR_SHARING_VIOLATION = 32,
  ERROR_FILE_EXISTS = 80, ERROR_INVALID_PARAMETER = 87, ERROR_INSUFFICIENT_BUFFER = 122,
  ERROR_MOD_NOT_FOUND = 126, ERROR_PROC_NOT_FOUND = 127, ERROR_NEGATIVE_SEEK = 131,
  ERROR_ALREADY_EXISTS = 183,
};

/* Standard handles from GetStdHandle (STD_INPUT/OUTPUT/ERROR_HANDLE). */
#define H_STDIN  0x1au
#define H_STDOUT 0x1bu
#define H_STDERR 0x1cu

static u32 g_lasterr = 0;
static u32 g_tls[256];
static u32 g_tls_next = 1;
static u32 g_cmdline;     /* guest string */
static u32 g_env;         /* guest double-null env block */
static u32 g_sleep_ms;    /* Sleep() time requested since the last take */

static void zero(u32 va, u32 n) { rg_memset(GPTR(va), 0, n); }

void rg_win32_init(void) {
  static const char cl[] = "ResetGeneration";
  g_lasterr = 0;
  g_tls_next = 1;
  g_sleep_ms = 0;
  rg_memset(g_tls, 0, sizeof g_tls);
  g_cmdline = rg_heap_alloc(sizeof cl);
  rg_memcpy(GPTR(g_cmdline), cl, sizeof cl);
  g_env = rg_heap_alloc(2);
  zero(g_env, 2);
}

u32 rg_win32_take_sleep(void) {
  u32 ms = g_sleep_ms;
  g_sleep_ms = 0;
  return ms;
}

/* ---- time ------------------------------------------------------------- */
static u32 ms32(void) { return (u32)(u64)rg_host_now_ms(); }   /* wraps like the real tick count */

void imp_timeGetTime(CPU *c) { RET(0, ms32()); }
void imp_GetTickCount(CPU *c) { RET(0, ms32()); }
void imp_QueryPerformanceCounter(CPU *c) {
  Wr64(A(0), (u64)(rg_host_now_ms() * 1000.0));   /* microseconds */
  RET(1, 1);
}
void imp_GetSystemTimeAsFileTime(CPU *c) {
  /* 100 ns units since 1601-01-01 (UTC) */
  Wr64(A(0), (u64)(rg_host_wall_ms() * 10000.0) + 116444736000000000ull);
  RETV(1);
}
void imp_GetTimeZoneInformation(CPU *c) {
  zero(A(0), 172);                /* UTC, no daylight saving */
  RET(1, 0);                      /* TIME_ZONE_ID_UNKNOWN */
}
void imp_FileTimeToLocalFileTime(CPU *c) { Wr64(A(1), Rd64(A(0))); RET(2, 1); }
void imp_FileTimeToSystemTime(CPU *c) {
  u64 t = Rd64(A(0)) / 10000;     /* ms since 1601-01-01 */
  u64 day = t / 86400000;
  u32 ms = (u32)(t % 1000), s = (u32)(t / 1000 % 86400);
  /* civil date from a day count (H. Hinnant), with years starting in March */
  u64 z = day + 306;              /* days since 1600-03-01 */
  u32 era = (u32)(z / 146097), doe = (u32)(z % 146097);
  u32 yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  u32 doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  u32 mp = (5 * doy + 2) / 153, d = doy - (153 * mp + 2) / 5 + 1;
  u32 m = mp < 10 ? mp + 3 : mp - 9, y = 1600 + era * 400 + yoe + (m <= 2);
  u32 st = A(1);
  Wr16(st, y); Wr16(st + 2, m); Wr16(st + 4, (u32)((day + 1) % 7));  /* 1601-01-01 was a Monday */
  Wr16(st + 6, d); Wr16(st + 8, s / 3600); Wr16(st + 10, s / 60 % 60);
  Wr16(st + 12, s % 60); Wr16(st + 14, ms);
  RET(2, 1);
}

/* ---- process / module ------------------------------------------------- */
void imp_GetCurrentThreadId(CPU *c) { RET(0, 1); }
void imp_GetCurrentProcessId(CPU *c) { RET(0, 1); }
void imp_GetCurrentProcess(CPU *c) { RET(0, 0xffffffffu); }
void imp_GetCommandLineA(CPU *c) { RET(0, g_cmdline); }
void imp_GetModuleHandleA(CPU *c) { RET(1, RG_GUEST_BASE); }
void imp_GetModuleFileNameA(CPU *c) {
  static const char p[] = "C:\\ResetGeneration\\ResetGeneration-Win32-Web.dll";
  u32 buf = A(1), size = A(2), i = 0;
  if (!size) RET(3, 0);
  for (; p[i] && i + 1 < size; i++) Wr8(buf + i, (u8)p[i]);
  Wr8(buf + i, 0);
  RET(3, i);
}
void imp_GetProcAddress(CPU *c) { FAIL(2, 0, ERROR_PROC_NOT_FOUND); }
void imp_LoadLibraryA(CPU *c) { FAIL(1, 0, ERROR_MOD_NOT_FOUND); }
void imp_GetVersionExA(CPU *c) {
  u32 p = A(0);                 /* OSVERSIONINFOA: Windows XP */
  Wr32(p + 4, 5);               /* major */
  Wr32(p + 8, 1);               /* minor */
  Wr32(p + 12, 2600);           /* build */
  Wr32(p + 16, 2);              /* VER_PLATFORM_WIN32_NT */
  zero(p + 20, 128);
  RET(1, 1);
}
void imp_GetStartupInfoA(CPU *c) {
  u32 p = A(0);
  zero(p, 68);
  Wr32(p, 68);
  RETV(1);
}
void imp_GetStdHandle(CPU *c) {
  switch (A(0)) {
    case 0xfffffff6u: RET(1, H_STDIN);
    case 0xfffffff5u: RET(1, H_STDOUT);
    case 0xfffffff4u: RET(1, H_STDERR);
    default: FAIL(1, 0xffffffffu, ERROR_INVALID_PARAMETER);
  }
}
void imp_SetStdHandle(CPU *c) { RET(2, 1); }
void imp_SetHandleCount(CPU *c) { RET(1, A(0)); }
void imp_GetFileType(CPU *c) {
  u32 h = A(0);
  if (rg_fs_is_handle(h)) RET(1, 1);                       /* FILE_TYPE_DISK */
  if (h >= H_STDIN && h <= H_STDERR) RET(1, 2);            /* FILE_TYPE_CHAR */
  FAIL(1, 0, ERROR_INVALID_HANDLE);                        /* FILE_TYPE_UNKNOWN */
}

/* ---- exit ------------------------------------------------------------- */
void imp_ExitProcess(CPU *c) { (void)c; rg_host_panic("ExitProcess called"); }
void imp_TerminateProcess(CPU *c) { (void)c; rg_host_panic("TerminateProcess called"); }

/* ---- error ------------------------------------------------------------ */
void imp_GetLastError(CPU *c) { RET(0, g_lasterr); }
void imp_SetLastError(CPU *c) { g_lasterr = A(0); RETV(1); }

/* ---- heap ------------------------------------------------------------- */
#define HEAP_ZERO_MEMORY 0x08u
#define HEAP_REALLOC_IN_PLACE_ONLY 0x10u

void imp_HeapCreate(CPU *c) { RET(3, 0x00f00d01u); }
void imp_HeapDestroy(CPU *c) { RET(1, 1); }
void imp_HeapAlloc(CPU *c) {
  u32 flags = A(1), size = A(2);
  u32 p = rg_heap_alloc(size);
  if (!p) FAIL(3, 0, ERROR_NOT_ENOUGH_MEMORY);
  if (flags & HEAP_ZERO_MEMORY) zero(p, size);
  RET(3, p);
}
void imp_HeapFree(CPU *c) { rg_heap_free(A(2)); RET(3, 1); }
void imp_HeapReAlloc(CPU *c) {
  u32 flags = A(1), p = A(2), size = A(3);
  u32 old = rg_heap_size(p);
  if ((flags & HEAP_REALLOC_IN_PLACE_ONLY) && size > old) FAIL(4, 0, ERROR_NOT_ENOUGH_MEMORY);
  u32 np = rg_heap_realloc(p, size);
  if (!np) FAIL(4, 0, ERROR_NOT_ENOUGH_MEMORY);
  if ((flags & HEAP_ZERO_MEMORY) && size > old) zero(np + old, size - old);
  RET(4, np);
}
void imp_HeapSize(CPU *c) { RET(3, rg_heap_size(A(2))); }

/* ---- virtual memory ----------------------------------------------------
 * Everything is committed read/write memory.  Fresh regions come from the
 * guest heap, aligned to the 64 KiB allocation granularity; the original
 * block pointer is kept just below the aligned address for VirtualFree. */
#define VA_MAGIC 0x56414c4cu   /* "VALL" */
#define MEM_RELEASE 0x8000u

void imp_VirtualAlloc(CPU *c) {
  u32 addr = A(0), size = A(1);
  if (addr) RET(4, addr & ~0xfffu);       /* commit inside an earlier reservation */
  u32 raw = rg_heap_alloc(size + 0x10000u + 8u);
  if (!raw) FAIL(4, 0, ERROR_NOT_ENOUGH_MEMORY);
  u32 p = (raw + 8u + 0xffffu) & ~0xffffu;
  Wr32(p - 8, VA_MAGIC);
  Wr32(p - 4, raw);
  zero(p, size);
  RET(4, p);
}
void imp_VirtualFree(CPU *c) {
  u32 addr = A(0), type = A(2);
  if ((type & MEM_RELEASE) && addr >= RG_HEAP_BASE + 8u && Rd32(addr - 8) == VA_MAGIC) {
    Wr32(addr - 8, 0);
    rg_heap_free(Rd32(addr - 4));
  }
  RET(3, 1);
}
void imp_VirtualProtect(CPU *c) { if (A(3)) Wr32(A(3), 0x04); RET(4, 1); }   /* PAGE_READWRITE */
void imp_VirtualQuery(CPU *c) {
  u32 va = A(0), info = A(1), len = A(2);
  if (len < 28) FAIL(3, 0, ERROR_INSUFFICIENT_BUFFER);
  int in_image = va >= RG_GUEST_BASE && va < RG_TIB_ADDR;
  zero(info, 28);
  Wr32(info + 0, va & ~0xfffu);                               /* BaseAddress */
  Wr32(info + 4, in_image ? RG_GUEST_BASE : va & ~0xffffu);   /* AllocationBase */
  Wr32(info + 8, 0x04);                                       /* AllocationProtect */
  Wr32(info + 12, 0x1000);                                    /* RegionSize */
  Wr32(info + 16, 0x1000);                                    /* State = MEM_COMMIT */
  Wr32(info + 20, 0x04);                                      /* Protect */
  Wr32(info + 24, in_image ? 0x1000000u : 0x20000u);          /* MEM_IMAGE / MEM_PRIVATE */
  RET(3, 28);
}
void imp_GetSystemInfo(CPU *c) {
  u32 p = A(0);
  zero(p, 36);
  Wr32(p + 4, 4096);          /* dwPageSize */
  Wr32(p + 8, 0x10000);       /* lpMinimumApplicationAddress */
  Wr32(p + 12, 0x7ffeffff);   /* lpMaximumApplicationAddress */
  Wr32(p + 16, 1);            /* dwActiveProcessorMask */
  Wr32(p + 20, 1);            /* dwNumberOfProcessors */
  Wr32(p + 24, 586);          /* dwProcessorType */
  Wr32(p + 28, 65536);        /* dwAllocationGranularity */
  Wr16(p + 32, 6);            /* wProcessorLevel */
  RETV(1);
}

/* ---- synchronisation (single-threaded) -------------------------------- */
void imp_InitializeCriticalSection(CPU *c) { RETV(1); }
void imp_DeleteCriticalSection(CPU *c) { RETV(1); }
void imp_EnterCriticalSection(CPU *c) { RETV(1); }
void imp_LeaveCriticalSection(CPU *c) { RETV(1); }
void imp_CreateMutexA(CPU *c) { RET(3, 0x00111001u); }
void imp_ReleaseMutex(CPU *c) { RET(1, 1); }
void imp_CreateSemaphoreA(CPU *c) { RET(4, 0x00111002u); }
void imp_ReleaseSemaphore(CPU *c) { if (A(2)) Wr32(A(2), 0); RET(3, 1); }
void imp_WaitForSingleObject(CPU *c) { RET(2, 0); }        /* WAIT_OBJECT_0 */
void imp_CloseHandle(CPU *c) {
  u32 h = A(0);
  if (rg_fs_is_handle(h)) rg_fs_close(h);
  RET(1, 1);
}
/* The game paces its 30 fps frame loop with Sleep(); the embedder reads the
 * requested time back (rg_sleep_ms) and schedules the next tick after it. */
void imp_Sleep(CPU *c) {
  u32 ms = A(0);
  g_sleep_ms = ms > 1000u - g_sleep_ms ? 1000u : g_sleep_ms + ms;
  RETV(1);
}
void imp_InterlockedExchange(CPU *c) {
  u32 pa = A(0), v = A(1), old = Rd32(pa);
  Wr32(pa, v);
  RET(2, old);
}

/* ---- TLS -------------------------------------------------------------- */
void imp_TlsAlloc(CPU *c) { RET(0, g_tls_next < 256 ? g_tls_next++ : 0xffffffffu); }
void imp_TlsFree(CPU *c) { RET(1, 1); }
void imp_TlsGetValue(CPU *c) { u32 i = A(0); g_lasterr = 0; RET(1, i < 256 ? g_tls[i] : 0); }
void imp_TlsSetValue(CPU *c) { u32 i = A(0); if (i < 256) g_tls[i] = A(1); RET(2, i < 256); }

/* ---- files ------------------------------------------------------------ */
enum { CREATE_NEW = 1, CREATE_ALWAYS = 2, OPEN_EXISTING = 3, OPEN_ALWAYS = 4, TRUNCATE_EXISTING = 5 };

static void path_from(u32 gptr, char *out, int cap) {
  int i = 0;
  for (; i < cap - 1; i++) { u8 ch = (u8)Rd8(gptr + i); if (!ch) break; out[i] = (char)ch; }
  out[i] = 0;
}

void imp_CreateFileA(CPU *c) {
  char path[260];
  path_from(A(0), path, sizeof path);
  u32 disp = A(4);
  int fi = rg_fs_find(path), existed = fi >= 0;
  switch (disp) {
    case CREATE_NEW:
      if (existed) FAIL(7, RG_FS_BAD, ERROR_FILE_EXISTS);
      fi = rg_fs_create(path);
      break;
    case CREATE_ALWAYS:
      fi = rg_fs_create(path);
      break;
    case OPEN_EXISTING:
      if (!existed) FAIL(7, RG_FS_BAD, ERROR_FILE_NOT_FOUND);
      break;
    case OPEN_ALWAYS:
      if (!existed) fi = rg_fs_create(path);
      break;
    case TRUNCATE_EXISTING:
      if (!existed) FAIL(7, RG_FS_BAD, ERROR_FILE_NOT_FOUND);
      fi = rg_fs_create(path);
      break;
    default:
      FAIL(7, RG_FS_BAD, ERROR_INVALID_PARAMETER);
  }
  if (fi < 0) FAIL(7, RG_FS_BAD, ERROR_TOO_MANY_OPEN_FILES);
  u32 h = rg_fs_open(fi);
  if (h == RG_FS_BAD) FAIL(7, RG_FS_BAD, ERROR_TOO_MANY_OPEN_FILES);
  g_lasterr = existed && (disp == CREATE_ALWAYS || disp == OPEN_ALWAYS) ? ERROR_ALREADY_EXISTS : 0;
  RET(7, h);
}
void imp_ReadFile(CPU *c) {
  u32 h = A(0), buf = A(1), n = A(2), pread = A(3);
  u32 got = 0;
  if (rg_fs_is_handle(h)) got = rg_fs_read(h, buf, n);
  else if (h != H_STDIN) FAIL(5, 0, ERROR_INVALID_HANDLE);
  if (pread) Wr32(pread, got);
  RET(5, 1);
}
void imp_WriteFile(CPU *c) {
  u32 h = A(0), buf = A(1), n = A(2), pwrote = A(3);
  if (rg_fs_is_handle(h)) n = rg_fs_write(h, buf, n);
  else if (h == H_STDOUT || h == H_STDERR) rg_host_logn((const char *)GPTR(buf), n);
  else FAIL(5, 0, ERROR_INVALID_HANDLE);
  if (pwrote) Wr32(pwrote, n);
  RET(5, 1);
}
void imp_SetFilePointer(CPU *c) {
  u32 h = A(0), phigh = A(2), method = A(3);
  i64 dist = phigh ? (i64)(((u64)Rd32(phigh) << 32) | A(1)) : (i64)(i32)A(1);
  if (!rg_fs_is_handle(h)) FAIL(4, 0xffffffffu, ERROR_INVALID_HANDLE);
  u32 np;
  if (!rg_fs_seek(h, dist, method, &np)) FAIL(4, 0xffffffffu, ERROR_NEGATIVE_SEEK);
  if (phigh) Wr32(phigh, 0);
  g_lasterr = 0;
  RET(4, np);
}
void imp_SetEndOfFile(CPU *c) {
  u32 h = A(0);
  if (!rg_fs_is_handle(h)) FAIL(1, 0, ERROR_INVALID_HANDLE);
  rg_fs_set_eof(h);
  RET(1, 1);
}
void imp_FlushFileBuffers(CPU *c) { RET(1, 1); }
void imp_DeleteFileA(CPU *c) {
  char path[260];
  path_from(A(0), path, sizeof path);
  if (rg_fs_delete(path)) RET(1, 1);
  FAIL(1, 0, rg_fs_find(path) >= 0 ? ERROR_SHARING_VIOLATION : ERROR_FILE_NOT_FOUND);
}
void imp_CreateDirectoryA(CPU *c) { RET(2, 1); }
void imp_RemoveDirectoryA(CPU *c) { RET(1, 1); }

/* WIN32_FIND_DATAA: attributes @0, size @28/@32, cFileName[260] @44,
 * cAlternateFileName[14] @304. */
static void fill_find_data(u32 fd, int fi) {
  zero(fd, 320);
  Wr32(fd, 0x20);                              /* FILE_ATTRIBUTE_ARCHIVE */
  Wr32(fd + 32, rg_fs_len(fi));
  const char *nm = rg_fs_name(fi);
  for (u32 i = 0; nm[i] && i < 259; i++) Wr8(fd + 44 + i, (u8)nm[i]);
}
void imp_FindFirstFileA(CPU *c) {
  char pat[260];
  path_from(A(0), pat, sizeof pat);
  int fi;
  u32 fh = rg_fs_find_first(pat, &fi);
  if (fh == RG_FS_BAD) FAIL(2, RG_FS_BAD, ERROR_FILE_NOT_FOUND);
  fill_find_data(A(1), fi);
  RET(2, fh);
}
void imp_FindNextFileA(CPU *c) {
  u32 fh = A(0);
  if (!rg_fs_is_find_handle(fh)) FAIL(2, 0, ERROR_INVALID_HANDLE);
  int fi = rg_fs_find_next(fh);
  if (fi < 0) FAIL(2, 0, ERROR_NO_MORE_FILES);
  fill_find_data(A(1), fi);
  RET(2, 1);
}

/* ---- locale / codepage (code page 1252 treated as Latin-1) ------------ */
void imp_GetACP(CPU *c) { RET(0, 1252); }
void imp_GetOEMCP(CPU *c) { RET(0, 437); }
void imp_GetCPInfo(CPU *c) {
  u32 p = A(1);
  zero(p, 18);
  Wr32(p, 1);                 /* MaxCharSize: single-byte code page */
  Wr8(p + 4, '?');            /* DefaultChar */
  RET(2, 1);
}

/* CT_CTYPE1 classification for the C locale (ASCII), as GetStringType does. */
static u32 ctype1(u32 ch) {
  enum { UPPER = 1, LOWER = 2, DIGIT = 4, SPACE = 8, PUNCT = 0x10, CNTRL = 0x20,
         BLANK = 0x40, XDIGIT = 0x80, ALPHA = 0x100 };
  if (ch >= 'A' && ch <= 'Z') return UPPER | ALPHA | (ch <= 'F' ? XDIGIT : 0);
  if (ch >= 'a' && ch <= 'z') return LOWER | ALPHA | (ch <= 'f' ? XDIGIT : 0);
  if (ch >= '0' && ch <= '9') return DIGIT | XDIGIT;
  if (ch == ' ') return SPACE | BLANK;
  if (ch == '\t') return SPACE | CNTRL | BLANK;
  if (ch >= '\n' && ch <= '\r') return SPACE | CNTRL;
  if (ch < 0x20 || ch == 0x7f) return CNTRL;
  if (ch < 0x7f) return PUNCT;
  return 0;
}
/* Character count of a string argument; a negative count means the string is
 * NUL-terminated and the terminator is included. */
static u32 count_or_len(u32 src, i32 n, int wide) {
  if (n >= 0) return (u32)n;
  u32 len = 0;
  while (wide ? Rd16(src + len * 2) : Rd8(src + len)) len++;
  return len + 1;
}
void imp_GetStringTypeA(CPU *c) {       /* (Locale, InfoType, Src, cchSrc, CharType) */
  u32 src = A(2), out = A(4), n = count_or_len(src, (i32)A(3), 0);
  for (u32 i = 0; i < n; i++) Wr16(out + i * 2, ctype1(Rd8(src + i)));
  RET(5, 1);
}
void imp_GetStringTypeW(CPU *c) {       /* (InfoType, Src, cchSrc, CharType) */
  u32 src = A(1), out = A(3), n = count_or_len(src, (i32)A(2), 1);
  for (u32 i = 0; i < n; i++) Wr16(out + i * 2, ctype1(Rd16(src + i * 2)));
  RET(4, 1);
}

#define LCMAP_LOWERCASE 0x100u
#define LCMAP_UPPERCASE 0x200u
static u32 lcmap(u32 flags, u32 src, i32 slen, u32 dst, u32 dlen, int wide) {
  u32 n = count_or_len(src, slen, wide);
  if (dlen == 0) return n;
  if (dlen < n) { g_lasterr = ERROR_INSUFFICIENT_BUFFER; return 0; }
  for (u32 i = 0; i < n; i++) {
    u32 ch = wide ? Rd16(src + i * 2) : Rd8(src + i);
    if ((flags & LCMAP_UPPERCASE) && ch >= 'a' && ch <= 'z') ch -= 32;
    if ((flags & LCMAP_LOWERCASE) && ch >= 'A' && ch <= 'Z') ch += 32;
    if (wide) Wr16(dst + i * 2, ch); else Wr8(dst + i, ch);
  }
  return n;
}
void imp_LCMapStringA(CPU *c) { RET(6, lcmap(A(1), A(2), (i32)A(3), A(4), A(5), 0)); }
void imp_LCMapStringW(CPU *c) { RET(6, lcmap(A(1), A(2), (i32)A(3), A(4), A(5), 1)); }

#define NORM_IGNORECASE 1u
static u32 cmpstr(u32 flags, u32 a, i32 al, u32 b, i32 bl, int wide) {
  u32 na = count_or_len(a, al, wide), nb = count_or_len(b, bl, wide);
  if (al < 0) na--;                     /* the terminator is not compared */
  if (bl < 0) nb--;
  for (u32 i = 0;; i++) {
    if (i == na || i == nb) return na == nb ? 2 : (na < nb ? 1 : 3);
    u32 ca = wide ? Rd16(a + i * 2) : Rd8(a + i);
    u32 cb = wide ? Rd16(b + i * 2) : Rd8(b + i);
    if (flags & NORM_IGNORECASE) {
      if (ca >= 'A' && ca <= 'Z') ca += 32;
      if (cb >= 'A' && cb <= 'Z') cb += 32;
    }
    if (ca != cb) return ca < cb ? 1 : 3;   /* CSTR_LESS_THAN / CSTR_GREATER_THAN */
  }
}
void imp_CompareStringA(CPU *c) { RET(6, cmpstr(A(1), A(2), (i32)A(3), A(4), (i32)A(5), 0)); }
void imp_CompareStringW(CPU *c) { RET(6, cmpstr(A(1), A(2), (i32)A(3), A(4), (i32)A(5), 1)); }

void imp_MultiByteToWideChar(CPU *c) {
  u32 src = A(2), dst = A(4), dlen = A(5), n = count_or_len(src, (i32)A(3), 0);
  if (dlen == 0) RET(6, n);
  if (dlen < n) FAIL(6, 0, ERROR_INSUFFICIENT_BUFFER);
  for (u32 i = 0; i < n; i++) Wr16(dst + i * 2, Rd8(src + i));
  RET(6, n);
}
void imp_WideCharToMultiByte(CPU *c) {
  u32 src = A(2), dst = A(4), dlen = A(5), n = count_or_len(src, (i32)A(3), 1);
  if (dlen == 0) RET(8, n);
  if (dlen < n) FAIL(8, 0, ERROR_INSUFFICIENT_BUFFER);
  for (u32 i = 0; i < n; i++) {
    u32 ch = Rd16(src + i * 2);
    Wr8(dst + i, ch < 0x100 ? ch : '?');
  }
  RET(8, n);
}
void imp_GetLocaleInfoA(CPU *c) {
  u32 out = A(2), len = A(3);
  if (len) Wr8(out, 0);
  RET(4, 1);                            /* empty string */
}

/* ---- environment ------------------------------------------------------ */
void imp_GetEnvironmentStrings(CPU *c) { RET(0, g_env); }
void imp_GetEnvironmentStringsW(CPU *c) { RET(0, g_env); }
void imp_FreeEnvironmentStringsA(CPU *c) { RET(1, 1); }
void imp_FreeEnvironmentStringsW(CPU *c) { RET(1, 1); }
void imp_SetEnvironmentVariableA(CPU *c) { RET(2, 1); }

/* ---- pointer validation (assume valid) -------------------------------- */
void imp_IsBadReadPtr(CPU *c) { RET(2, 0); }
void imp_IsBadWritePtr(CPU *c) { RET(2, 0); }
void imp_IsBadCodePtr(CPU *c) { RET(1, 0); }

/* ---- exceptions ------------------------------------------------------
 * Nothing raises hardware exceptions here, and C++ throw / SEH unwinding is
 * not supported; the game only reaches these on fatal errors. */
void imp_RaiseException(CPU *c) { (void)c; rg_host_panic("RaiseException (C++ throw) is not supported"); }
void imp_RtlUnwind(CPU *c) { (void)c; rg_host_panic("RtlUnwind is not supported"); }
void imp_UnhandledExceptionFilter(CPU *c) { RET(1, 1); }      /* EXCEPTION_EXECUTE_HANDLER */
void imp_SetUnhandledExceptionFilter(CPU *c) { RET(1, 0); }
