/* Win32 / CRT import shims (stdcall).  Each reads its arguments from the guest
 * stack, performs the work against the runtime's memory/filesystem, puts the
 * result in EAX, and pops the return address plus its arguments. */
#include "rg_defs.h"
#include "gen/rg_imports.h"
#include "rg_mem.h"
#include "rg_fs.h"
#include "rg_host.h"

#define A(i) Rd32(c->esp + 4u + 4u * (i))
#define RET(nargs, val) do { c->eax = (u32)(val); c->esp += 4u + 4u * (nargs); return; } while (0)
#define RETV(nargs)     do { c->esp += 4u + 4u * (nargs); return; } while (0)

static u32 g_lasterr = 0;
static u32 g_tls[256];
static u32 g_tls_next = 1;
static u32 g_cmdline;     /* guest string */
static u32 g_env;         /* guest double-null env block */

void rg_win32_init(void) {
  g_lasterr = 0;
  g_tls_next = 1;
  for (int i = 0; i < 256; i++) g_tls[i] = 0;
  const char *cl = "ResetGeneration";
  g_cmdline = rg_heap_alloc(32);
  for (int i = 0; cl[i]; i++) Wr8(g_cmdline + i, (u8)cl[i]);
  Wr8(g_cmdline + 15, 0);
  g_env = rg_heap_alloc(8);
  Wr8(g_env, 0); Wr8(g_env + 1, 0);
}

/* ---- time ------------------------------------------------------------- */
void imp_timeGetTime(CPU *c) { RET(0, (u32)rg_host_now_ms()); }
void imp_GetTickCount(CPU *c) { RET(0, (u32)rg_host_now_ms()); }
void imp_QueryPerformanceCounter(CPU *c) {
  u64 t = (u64)(rg_host_now_ms() * 1000.0);
  Wr64(A(0), t);
  RET(1, 1);
}
void imp_GetSystemTimeAsFileTime(CPU *c) {
  /* 100ns units since 1601; approximate from ms since 1970. */
  u64 ft = (u64)(rg_host_now_ms() * 10000.0) + 116444736000000000ull;
  Wr32(A(0), (u32)ft); Wr32(A(0) + 4, (u32)(ft >> 32));
  RETV(1);
}
void imp_GetTimeZoneInformation(CPU *c) { RET(1, 0); }
void imp_FileTimeToLocalFileTime(CPU *c) {
  Wr32(A(1), Rd32(A(0))); Wr32(A(1) + 4, Rd32(A(0) + 4)); RET(2, 1);
}
void imp_FileTimeToSystemTime(CPU *c) {
  for (int i = 0; i < 16; i++) Wr8(A(1) + i, 0);
  Wr16(A(1), 2009); Wr16(A(1) + 2, 1); Wr16(A(1) + 6, 1);
  RET(2, 1);
}

/* ---- process / module ------------------------------------------------- */
void imp_GetCurrentThreadId(CPU *c) { RET(0, 1); }
void imp_GetCurrentProcessId(CPU *c) { RET(0, 1); }
void imp_GetCurrentProcess(CPU *c) { RET(0, 0xffffffffu); }
void imp_GetCommandLineA(CPU *c) { RET(0, g_cmdline); }
void imp_GetModuleHandleA(CPU *c) { RET(1, RG_GUEST_BASE); }
void imp_GetModuleFileNameA(CPU *c) {
  const char *p = "C:\\RG\\ResetGeneration.exe";
  u32 buf = A(1), size = A(2), i = 0;
  for (; p[i] && i + 1 < size; i++) Wr8(buf + i, (u8)p[i]);
  Wr8(buf + i, 0);
  RET(3, i);
}
void imp_GetProcAddress(CPU *c) { RET(2, 0); }
void imp_LoadLibraryA(CPU *c) { RET(1, 0x00d11d11u); }
void imp_GetVersionExA(CPU *c) {
  u32 p = A(0);                 /* OSVERSIONINFO; dwOSVersionInfoSize at [0] */
  Wr32(p + 4, 5);               /* major */
  Wr32(p + 8, 1);               /* minor */
  Wr32(p + 12, 2600);           /* build */
  Wr32(p + 16, 2);              /* VER_PLATFORM_WIN32_NT */
  for (int i = 0; i < 128; i++) Wr8(p + 20 + i, 0);
  RET(1, 1);
}
void imp_GetStartupInfoA(CPU *c) {
  u32 p = A(0);
  for (int i = 0; i < 68; i++) Wr8(p + i, 0);
  Wr32(p, 68);
  RETV(1);
}
void imp_GetStdHandle(CPU *c) {
  i32 n = (i32)A(0);
  RET(1, (u32)(0x10 + (-n)));    /* -10/-11/-12 -> 26/27/28 */
}
void imp_SetStdHandle(CPU *c) { RET(2, 1); }
void imp_SetHandleCount(CPU *c) { RET(1, A(0)); }
void imp_GetFileType(CPU *c) {
  u32 h = A(0);
  RET(1, rg_fs_is_handle(h) ? 1u : 2u);  /* FILE_TYPE_DISK / _CHAR */
}

/* ---- exit ------------------------------------------------------------- */
void imp_ExitProcess(CPU *c) { (void)c; rg_host_panic("ExitProcess called"); }
void imp_TerminateProcess(CPU *c) { (void)c; rg_host_panic("TerminateProcess called"); }

/* ---- error ------------------------------------------------------------ */
void imp_GetLastError(CPU *c) { RET(0, g_lasterr); }
void imp_SetLastError(CPU *c) { g_lasterr = A(0); RETV(1); }

/* ---- heap ------------------------------------------------------------- */
void imp_HeapCreate(CPU *c) { RET(3, 0x00f00d01u); }
void imp_HeapDestroy(CPU *c) { RET(1, 1); }
void imp_HeapAlloc(CPU *c) {
  u32 flags = A(1), size = A(2);
  u32 p = rg_heap_alloc(size);
  if (p && (flags & 8)) for (u32 i = 0; i < size; i++) Wr8(p + i, 0);
  RET(3, p);
}
void imp_HeapFree(CPU *c) { rg_heap_free(A(2)); RET(3, 1); }
void imp_HeapReAlloc(CPU *c) {
  u32 flags = A(1), p = A(2), size = A(3);
  u32 old = rg_heap_size(p);
  u32 np = rg_heap_realloc(p, size);
  if (np && (flags & 8) && size > old)
    for (u32 i = old; i < size; i++) Wr8(np + i, 0);
  RET(4, np);
}
void imp_HeapSize(CPU *c) { RET(3, rg_heap_size(A(2))); }

/* ---- virtual memory --------------------------------------------------- */
void imp_VirtualAlloc(CPU *c) {
  u32 size = A(1);
  u32 p = rg_heap_alloc(size);
  for (u32 i = 0; i < size; i++) Wr8(p + i, 0);
  RET(4, p);
}
void imp_VirtualFree(CPU *c) { rg_heap_free(A(0)); RET(3, 1); }
void imp_VirtualProtect(CPU *c) { if (A(3)) Wr32(A(3), 0x40); RET(4, 1); }
void imp_VirtualQuery(CPU *c) {
  u32 info = A(1), len = A(2);
  for (u32 i = 0; i < len; i++) Wr8(info + i, 0);
  RET(3, len);
}
void imp_GetSystemInfo(CPU *c) {
  u32 p = A(0);
  for (int i = 0; i < 36; i++) Wr8(p + i, 0);
  Wr32(p + 4, 4096);      /* dwPageSize */
  Wr32(p + 28, 65536);    /* allocation granularity */
  Wr32(p + 20, 1);        /* number of processors */
  RETV(1);
}

/* ---- synchronisation (single-threaded no-ops) ------------------------- */
void imp_InitializeCriticalSection(CPU *c) { RETV(1); }
void imp_DeleteCriticalSection(CPU *c) { RETV(1); }
void imp_EnterCriticalSection(CPU *c) { RETV(1); }
void imp_LeaveCriticalSection(CPU *c) { RETV(1); }
void imp_CreateMutexA(CPU *c) { RET(3, 0x00111001u); }
void imp_ReleaseMutex(CPU *c) { RET(1, 1); }
void imp_CreateSemaphoreA(CPU *c) { RET(4, 0x00111002u); }
void imp_ReleaseSemaphore(CPU *c) { if (A(2)) Wr32(A(2), 0); RET(3, 1); }
void imp_WaitForSingleObject(CPU *c) { RET(2, 0); }
void imp_CloseHandle(CPU *c) { u32 h = A(0); if (rg_fs_is_handle(h)) rg_fs_close(h); RET(1, 1); }
void imp_Sleep(CPU *c) { RETV(1); }
void imp_InterlockedExchange(CPU *c) {
  u32 pa = A(0), v = A(1), old = Rd32(pa);
  Wr32(pa, v);
  RET(2, old);
}

/* ---- TLS -------------------------------------------------------------- */
void imp_TlsAlloc(CPU *c) { RET(0, g_tls_next < 256 ? g_tls_next++ : 0xffffffffu); }
void imp_TlsFree(CPU *c) { RET(1, 1); }
void imp_TlsGetValue(CPU *c) { u32 i = A(0); RET(1, i < 256 ? g_tls[i] : 0); }
void imp_TlsSetValue(CPU *c) { u32 i = A(0); if (i < 256) g_tls[i] = A(1); RET(2, 1); }

/* ---- files ------------------------------------------------------------ */
static void path_from(u32 gptr, char *out, int cap) {
  int i = 0;
  for (; i < cap - 1; i++) { u8 ch = Rd8(gptr + i); if (!ch) break; out[i] = (char)ch; }
  out[i] = 0;
}
void imp_CreateFileA(CPU *c) {
  char path[260];
  path_from(A(0), path, sizeof path);
  u32 access = A(1);
  u32 disp = A(4);              /* CREATE_ALWAYS=2, CREATE_NEW=1, OPEN_ALWAYS=4 */
  int create = (disp == 1 || disp == 2 || disp == 4) || (access & 0x40000000u);
  int trunc = (disp == 2);
  u32 h = rg_fs_open(path, create, trunc);
  if (h == 0xffffffffu) g_lasterr = 2;  /* ERROR_FILE_NOT_FOUND */
  RET(7, h);
}
void imp_ReadFile(CPU *c) {
  u32 h = A(0), buf = A(1), n = A(2), pread = A(3);
  u32 got = rg_fs_is_handle(h) ? rg_fs_read(h, buf, n) : 0;
  if (pread) Wr32(pread, got);
  RET(5, 1);
}
void imp_WriteFile(CPU *c) {
  u32 h = A(0), buf = A(1), n = A(2), pwrote = A(3);
  u32 put = rg_fs_is_handle(h) ? rg_fs_write(h, buf, n) : n;
  if (pwrote) Wr32(pwrote, put);
  RET(5, 1);
}
void imp_SetFilePointer(CPU *c) {
  u32 h = A(0); i32 dist = (i32)A(1); u32 method = A(3);
  if (!rg_fs_is_handle(h)) RET(4, 0xffffffffu);
  u32 np; rg_fs_seek(h, dist, method, &np);
  RET(4, np);
}
void imp_SetEndOfFile(CPU *c) { RET(1, 1); }
void imp_FlushFileBuffers(CPU *c) { RET(1, 1); }
void imp_DeleteFileA(CPU *c) { RET(1, 1); }
void imp_CreateDirectoryA(CPU *c) { RET(2, 1); }
void imp_RemoveDirectoryA(CPU *c) { RET(1, 1); }

void imp_FindFirstFileA(CPU *c) {
  char pat[260];
  path_from(A(0), pat, sizeof pat);
  int ck = rg_fs_find_first(pat);
  if (ck < 0) { g_lasterr = 18; RET(2, 0xffffffffu); }
  /* WIN32_FIND_DATA: write cFileName at offset 44. */
  u32 fd = A(1);
  for (int i = 0; i < 44; i++) Wr8(fd + i, 0);
  const char *nm = rg_fs_name(ck);
  int i = 0; for (; nm[i]; i++) Wr8(fd + 44 + i, (u8)nm[i]);
  Wr8(fd + 44 + i, 0);
  RET(2, 0x46100000u + (u32)ck);   /* find handle encodes last index */
}
void imp_FindNextFileA(CPU *c) {
  u32 h = A(0);
  int last = (int)(h - 0x46100000u);
  int ck = rg_fs_find_next(last);
  if (ck < 0) { g_lasterr = 18; RET(2, 0); }
  u32 fd = A(1);
  for (int i = 0; i < 44; i++) Wr8(fd + i, 0);
  const char *nm = rg_fs_name(ck);
  int i = 0; for (; nm[i]; i++) Wr8(fd + 44 + i, (u8)nm[i]);
  Wr8(fd + 44 + i, 0);
  /* note: find handle is read-only here; caller passes same handle back and
     we rely on the global cursor advanced by rg_fs_find_next */
  RET(2, 1);
}

/* ---- locale / codepage (minimal ASCII) -------------------------------- */
void imp_GetACP(CPU *c) { RET(0, 1252); }
void imp_GetOEMCP(CPU *c) { RET(0, 437); }
void imp_GetCPInfo(CPU *c) {
  u32 p = A(1);
  Wr32(p, 1);                 /* MaxCharSize */
  Wr8(p + 4, 0); Wr8(p + 5, 0);
  for (int i = 0; i < 12; i++) Wr8(p + 6 + i, 0);
  RET(2, 1);
}
void imp_GetStringTypeA(CPU *c) {
  u32 src = A(2), cnt = A(3), out = A(4);
  for (u32 i = 0; i < cnt; i++) {
    u8 ch = Rd8(src + i); u16 t = 0;
    if (ch >= '0' && ch <= '9') t = 0x0084;      /* digit */
    else if (ch >= 'A' && ch <= 'Z') t = 0x0101; /* upper|alpha */
    else if (ch >= 'a' && ch <= 'z') t = 0x0102; /* lower|alpha */
    else if (ch == ' ' || ch == '\t') t = 0x0048;/* space|blank */
    Wr16(out + i * 2, t);
  }
  RET(5, 1);
}
void imp_GetStringTypeW(CPU *c) {
  u32 src = A(1), cnt = A(2), out = A(3);
  for (u32 i = 0; i < cnt; i++) {
    u16 ch = (u16)Rd16(src + i * 2); u16 t = 0;
    if (ch >= '0' && ch <= '9') t = 0x0084;
    else if (ch >= 'A' && ch <= 'Z') t = 0x0101;
    else if (ch >= 'a' && ch <= 'z') t = 0x0102;
    else if (ch == ' ' || ch == '\t') t = 0x0048;
    Wr16(out + i * 2, t);
  }
  RET(4, 1);
}
static u32 lcmap(CPU *c, u32 flags, u32 src, i32 slen, u32 dst, u32 dlen, int wide) {
  u32 n = slen < 0 ? 0 : (u32)slen;
  if (slen < 0) { /* null-terminated */
    if (wide) { while (Rd16(src + n * 2)) n++; n++; }
    else { while (Rd8(src + n)) n++; n++; }
  }
  if (dlen == 0) return n;
  for (u32 i = 0; i < n && i < dlen; i++) {
    u32 ch = wide ? Rd16(src + i * 2) : Rd8(src + i);
    if ((flags & 0x100) && ch >= 'a' && ch <= 'z') ch -= 32;   /* UPPER */
    if ((flags & 0x200) && ch >= 'A' && ch <= 'Z') ch += 32;   /* LOWER */
    if (wide) Wr16(dst + i * 2, (u16)ch); else Wr8(dst + i, (u8)ch);
  }
  return n;
}
void imp_LCMapStringA(CPU *c) { RET(6, lcmap(c, A(1), A(2), (i32)A(3), A(4), A(5), 0)); }
void imp_LCMapStringW(CPU *c) { RET(6, lcmap(c, A(1), A(2), (i32)A(3), A(4), A(5), 1)); }
static u32 cmpstr(CPU *c, u32 a, i32 al, u32 b, i32 bl, int wide) {
  u32 i = 0;
  for (;;) {
    int ae = (al >= 0 && (i32)i >= al);
    int be = (bl >= 0 && (i32)i >= bl);
    u32 ca = wide ? Rd16(a + i*2) : Rd8(a + i);
    u32 cb = wide ? Rd16(b + i*2) : Rd8(b + i);
    if (ae || ca == 0) ca = 0;
    if (be || cb == 0) cb = 0;
    if (ca != cb) return ca < cb ? 1 : 3;   /* CSTR_LESS_THAN / GREATER */
    if (ca == 0) return 2;                  /* CSTR_EQUAL */
    i++;
  }
}
void imp_CompareStringA(CPU *c) { RET(6, cmpstr(c, A(2), (i32)A(3), A(4), (i32)A(5), 0)); }
void imp_CompareStringW(CPU *c) { RET(6, cmpstr(c, A(2), (i32)A(3), A(4), (i32)A(5), 1)); }
void imp_MultiByteToWideChar(CPU *c) {
  u32 src = A(2); i32 slen = (i32)A(3); u32 dst = A(4), dlen = A(5);
  u32 n = slen < 0 ? 0 : (u32)slen;
  if (slen < 0) { while (Rd8(src + n)) n++; n++; }
  if (dlen == 0) RET(6, n);
  u32 w = 0;
  for (u32 i = 0; i < n && w < dlen; i++, w++) Wr16(dst + w * 2, Rd8(src + i));
  RET(6, w);
}
void imp_WideCharToMultiByte(CPU *c) {
  u32 src = A(2); i32 slen = (i32)A(3); u32 dst = A(4), dlen = A(5);
  u32 n = slen < 0 ? 0 : (u32)slen;
  if (slen < 0) { while (Rd16(src + n * 2)) n++; n++; }
  if (dlen == 0) RET(8, n);
  u32 w = 0;
  for (u32 i = 0; i < n && w < dlen; i++, w++) Wr8(dst + w, (u8)Rd16(src + i * 2));
  RET(8, w);
}
void imp_GetLocaleInfoA(CPU *c) {
  u32 out = A(2), len = A(3);
  if (len) { Wr8(out, 0); }
  RET(4, 1);
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

/* ---- exception machinery (should not fire) ---------------------------- */
void imp_RaiseException(CPU *c) { (void)c; rg_host_panic("RaiseException"); }
void imp_RtlUnwind(CPU *c) { (void)c; rg_host_panic("RtlUnwind"); }
void imp_UnhandledExceptionFilter(CPU *c) { RET(1, 1); }
void imp_SetUnhandledExceptionFilter(CPU *c) { RET(1, 0); }
