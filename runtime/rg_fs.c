#include "rg_fs.h"
#include "rg_host.h"

typedef struct {
  char name[64];
  u8  *data;      /* host pointer */
  u32  size;
  u32  cap;
  int  used;
  int  dirty;
} File;

static File g_files[RG_FS_MAX];
static int g_nfiles;

typedef struct {
  int file;
  u32 pos;
  int writable;
  int used;
} Handle;
#define RG_MAXH 32
static Handle g_handles[RG_MAXH];
#define HBASE 0x40000000u   /* file handles encoded as HBASE+index */

void rg_fs_init(void) {
  g_nfiles = 0;
  for (int i = 0; i < RG_FS_MAX; i++) g_files[i].used = 0;
  for (int i = 0; i < RG_MAXH; i++) g_handles[i].used = 0;
}

static const char *basename_of(const char *p) {
  const char *b = p;
  for (const char *q = p; *q; q++) if (*q == '/' || *q == '\\') b = q + 1;
  return b;
}

static void copyname(char *dst, const char *src) {
  int i = 0;
  for (; src[i] && i < 63; i++) dst[i] = src[i];
  dst[i] = 0;
}

static int streq(const char *a, const char *b) {
  while (*a && *b) { if (*a != *b) return 0; a++; b++; }
  return *a == *b;
}

int rg_fs_add(const char *name, const u8 *data, u32 size) {
  if (g_nfiles >= RG_FS_MAX) return -1;
  int i = g_nfiles++;
  copyname(g_files[i].name, basename_of(name));
  u8 *buf = (u8 *)rg_host_alloc(size ? size : 1);
  for (u32 k = 0; k < size; k++) buf[k] = data[k];
  g_files[i].data = buf;
  g_files[i].size = size;
  g_files[i].cap = size;
  g_files[i].used = 1;
  g_files[i].dirty = 0;
  return i;
}

int rg_fs_find(const char *basename) {
  const char *b = basename_of(basename);
  for (int i = 0; i < g_nfiles; i++)
    if (g_files[i].used && streq(g_files[i].name, b)) return i;
  return -1;
}

int rg_fs_create(const char *basename) {
  int i = rg_fs_find(basename);
  if (i >= 0) { g_files[i].size = 0; g_files[i].dirty = 1; return i; }
  if (g_nfiles >= RG_FS_MAX) return -1;
  i = g_nfiles++;
  copyname(g_files[i].name, basename_of(basename));
  g_files[i].cap = 4096;
  g_files[i].data = (u8 *)rg_host_alloc(g_files[i].cap);
  g_files[i].size = 0;
  g_files[i].used = 1;
  g_files[i].dirty = 1;
  return i;
}

int rg_fs_count(void) { return g_nfiles; }
const char *rg_fs_name(int i) { return g_files[i].name; }
u8 *rg_fs_data(int i) { return g_files[i].data; }
u32 rg_fs_len(int i) { return g_files[i].size; }
int rg_fs_dirty(int i) { return g_files[i].dirty; }
void rg_fs_clear_dirty(int i) { g_files[i].dirty = 0; }

static void ensure_cap(int fi, u32 need) {
  File *f = &g_files[fi];
  if (need <= f->cap) return;
  u32 nc = f->cap ? f->cap : 4096;
  while (nc < need) nc *= 2;
  u8 *nb = (u8 *)rg_host_alloc(nc);
  for (u32 k = 0; k < f->size; k++) nb[k] = f->data[k];
  f->data = nb;
  f->cap = nc;
}

u32 rg_fs_open(const char *path, int create, int truncate) {
  int fi = rg_fs_find(path);
  if (fi < 0) {
    if (!create) return 0xffffffffu;   /* INVALID_HANDLE_VALUE */
    fi = rg_fs_create(path);
    if (fi < 0) return 0xffffffffu;
  } else if (truncate) {
    g_files[fi].size = 0;
    g_files[fi].dirty = 1;
  }
  for (int h = 0; h < RG_MAXH; h++) {
    if (!g_handles[h].used) {
      g_handles[h].used = 1;
      g_handles[h].file = fi;
      g_handles[h].pos = 0;
      g_handles[h].writable = create || truncate;
      return HBASE + h;
    }
  }
  return 0xffffffffu;
}

int rg_fs_is_handle(u32 h) {
  return h >= HBASE && h < HBASE + RG_MAXH && g_handles[h - HBASE].used;
}

u32 rg_fs_read(u32 h, u32 guest_buf, u32 n) {
  Handle *H = &g_handles[h - HBASE];
  File *f = &g_files[H->file];
  u32 avail = f->size > H->pos ? f->size - H->pos : 0;
  if (n > avail) n = avail;
  for (u32 k = 0; k < n; k++) *(u8 *)GPTR(guest_buf + k) = f->data[H->pos + k];
  H->pos += n;
  return n;
}

u32 rg_fs_write(u32 h, u32 guest_buf, u32 n) {
  Handle *H = &g_handles[h - HBASE];
  File *f = &g_files[H->file];
  ensure_cap(H->file, H->pos + n);
  for (u32 k = 0; k < n; k++) f->data[H->pos + k] = *(u8 *)GPTR(guest_buf + k);
  H->pos += n;
  if (H->pos > f->size) f->size = H->pos;
  f->dirty = 1;
  return n;
}

u32 rg_fs_seek(u32 h, i32 dist, u32 method, u32 *newpos_out) {
  Handle *H = &g_handles[h - HBASE];
  File *f = &g_files[H->file];
  i64 base = method == 1 ? (i64)H->pos : method == 2 ? (i64)f->size : 0;
  i64 np = base + dist;
  if (np < 0) np = 0;
  H->pos = (u32)np;
  if (newpos_out) *newpos_out = H->pos;
  return H->pos;
}

u32 rg_fs_size_of(u32 h) { return g_files[g_handles[h - HBASE].file].size; }

void rg_fs_close(u32 h) {
  if (rg_fs_is_handle(h)) g_handles[h - HBASE].used = 0;
}

/* ---- find-first/next (pattern ignored beyond extension suffix) -------- */
static int g_find_i;
static char g_find_suffix[16];

static int suffix_match(const char *name, const char *suf) {
  if (!suf[0]) return 1;
  u32 ln = 0, ls = 0;
  while (name[ln]) ln++;
  while (suf[ls]) ls++;
  if (ls > ln) return 0;
  for (u32 k = 0; k < ls; k++) if (name[ln - ls + k] != suf[k]) return 0;
  return 1;
}

int rg_fs_find_first(const char *pattern) {
  const char *b = basename_of(pattern);
  /* capture suffix after '*' (e.g. "*.pak" -> ".pak") */
  int i = 0; const char *p = b;
  while (*p && *p != '*') p++;
  if (*p == '*') p++;
  while (*p && i < 15) g_find_suffix[i++] = *p++;
  g_find_suffix[i] = 0;
  g_find_i = -1;
  return rg_fs_find_next(-2);
}

int rg_fs_find_next(int cookie) {
  int start = (cookie == -2) ? 0 : g_find_i + 1;
  for (int i = start; i < g_nfiles; i++) {
    if (g_files[i].used && suffix_match(g_files[i].name, g_find_suffix)) {
      g_find_i = i;
      return i;
    }
  }
  return -1;
}
