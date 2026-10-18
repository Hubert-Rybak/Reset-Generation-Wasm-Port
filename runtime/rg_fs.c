#include "rg_fs.h"
#include "rg_host.h"

typedef struct {
  char name[64];
  u8  *data;      /* host pointer */
  u32  size;
  u32  cap;
  u8   used;      /* slot holds a live file */
  u8   dirty;     /* changed since the embedder last persisted it */
  u8   deleted;   /* removed by the game; the embedder should drop its copy */
} File;

static File g_files[RG_FS_MAX];
static int g_nfiles;

typedef struct { int file; u32 pos; int used; } Handle;
#define RG_MAXH 32
#define HBASE 0x40000000u   /* file handles are HBASE + slot */
static Handle g_handles[RG_MAXH];

/* The CRT's _findclose is not linked into the game, so it never closes its
 * find handles; when all slots are taken the least recently used is reused. */
typedef struct { char pattern[64]; int pos; int used; u32 stamp; } Finder;
#define RG_MAXF 8
#define FBASE 0x46100000u   /* find handles are FBASE + slot */
static Finder g_finders[RG_MAXF];
static u32 g_find_clock;

void rg_fs_init(void) {
  g_nfiles = 0;
  rg_memset(g_files, 0, sizeof g_files);
  rg_memset(g_handles, 0, sizeof g_handles);
  rg_memset(g_finders, 0, sizeof g_finders);
}

/* ---- names ------------------------------------------------------------- */
static const char *basename_of(const char *p) {
  const char *b = p;
  for (const char *q = p; *q; q++) if (*q == '/' || *q == '\\' || *q == ':') b = q + 1;
  return b;
}

static void copyname(char *dst, const char *src) {
  int i = 0;
  for (; src[i] && i < 63; i++) dst[i] = src[i];
  dst[i] = 0;
}

static int lower(int ch) { return ch >= 'A' && ch <= 'Z' ? ch + 32 : ch; }

static int name_eq(const char *a, const char *b) {
  while (*a && lower((u8)*a) == lower((u8)*b)) { a++; b++; }
  return lower((u8)*a) == lower((u8)*b);
}

/* Windows-style wildcard match: `*` any run, `?` any one character. */
static int wild(const char *p, const char *s) {
  for (; *p; p++, s++) {
    if (*p == '*') {
      while (p[1] == '*') p++;
      if (!p[1]) return 1;
      for (; *s; s++) if (wild(p + 1, s)) return 1;
      return 0;
    }
    if (!*s || (*p != '?' && lower((u8)*p) != lower((u8)*s))) return 0;
  }
  return *s == 0;
}

static int pattern_match(const char *pat, const char *name) {
  if (name_eq(pat, "*.*")) return 1;            /* also matches names without a dot */
  return wild(pat, name);
}

/* ---- files -------------------------------------------------------------- */
int rg_fs_find(const char *path) {
  const char *b = basename_of(path);
  for (int i = 0; i < g_nfiles; i++)
    if (g_files[i].used && name_eq(g_files[i].name, b)) return i;
  return -1;
}

/* A slot for `name`: a pending deletion of the same name, a recycled slot,
 * or a fresh one. */
static int new_slot(const char *name) {
  int free_slot = -1;
  for (int i = 0; i < g_nfiles; i++) {
    File *f = &g_files[i];
    if (f->used) continue;
    if (f->deleted && name_eq(f->name, name)) return i;
    if (!f->dirty && free_slot < 0) free_slot = i;
  }
  if (free_slot >= 0) return free_slot;
  return g_nfiles < RG_FS_MAX ? g_nfiles++ : -1;
}

int rg_fs_add(const char *name, u8 *data, u32 size) {
  const char *b = basename_of(name);
  int i = rg_fs_find(b);
  if (i < 0) i = new_slot(b);
  if (i < 0) return -1;
  File *f = &g_files[i];
  copyname(f->name, b);
  f->data = data;
  f->size = f->cap = size;
  f->used = 1;
  f->dirty = f->deleted = 0;
  return i;
}

int rg_fs_create(const char *path) {
  int i = rg_fs_find(path);
  if (i < 0) {
    const char *b = basename_of(path);
    i = new_slot(b);
    if (i < 0) return -1;
    File *f = &g_files[i];
    copyname(f->name, b);
    if (!f->data || f->cap == 0) {
      f->cap = 4096;
      f->data = (u8 *)rg_host_alloc(f->cap);
    }
  }
  File *f = &g_files[i];
  f->size = 0;
  f->used = 1;
  f->deleted = 0;
  f->dirty = 1;
  return i;
}

int rg_fs_delete(const char *path) {
  int i = rg_fs_find(path);
  if (i < 0) return 0;
  for (int h = 0; h < RG_MAXH; h++)       /* Windows refuses to delete open files */
    if (g_handles[h].used && g_handles[h].file == i) return 0;
  g_files[i].used = 0;
  g_files[i].deleted = 1;
  g_files[i].dirty = 1;
  g_files[i].size = 0;
  return 1;
}

int rg_fs_count(void) { return g_nfiles; }
const char *rg_fs_name(int i) { return g_files[i].name; }
u8 *rg_fs_data(int i) { return g_files[i].data; }
u32 rg_fs_len(int i) { return g_files[i].size; }
int rg_fs_dirty(int i) { return g_files[i].dirty; }
int rg_fs_deleted(int i) { return g_files[i].deleted; }
void rg_fs_clear_dirty(int i) { g_files[i].dirty = 0; }

static void ensure_cap(File *f, u32 need) {
  if (need <= f->cap) return;
  u32 nc = f->cap ? f->cap : 4096;
  while (nc < need) nc = nc * 2 > nc ? nc * 2 : need;
  u8 *nb = (u8 *)rg_host_alloc(nc);
  rg_memcpy(nb, f->data, f->size);
  f->data = nb;
  f->cap = nc;
}

/* ---- handles ------------------------------------------------------------ */
u32 rg_fs_open(int fi) {
  if (fi < 0) return RG_FS_BAD;
  for (int h = 0; h < RG_MAXH; h++) {
    if (!g_handles[h].used) {
      g_handles[h].used = 1;
      g_handles[h].file = fi;
      g_handles[h].pos = 0;
      return HBASE + h;
    }
  }
  return RG_FS_BAD;
}

int rg_fs_is_handle(u32 h) {
  return h >= HBASE && h < HBASE + RG_MAXH && g_handles[h - HBASE].used;
}

u32 rg_fs_read(u32 h, u32 guest_buf, u32 n) {
  Handle *H = &g_handles[h - HBASE];
  File *f = &g_files[H->file];
  u32 avail = f->size > H->pos ? f->size - H->pos : 0;
  if (n > avail) n = avail;
  rg_memcpy(GPTR(guest_buf), f->data + H->pos, n);
  H->pos += n;
  return n;
}

u32 rg_fs_write(u32 h, u32 guest_buf, u32 n) {
  Handle *H = &g_handles[h - HBASE];
  File *f = &g_files[H->file];
  if (!n) return 0;
  ensure_cap(f, H->pos + n);
  if (H->pos > f->size) rg_memset(f->data + f->size, 0, H->pos - f->size);  /* gap after a seek */
  rg_memcpy(f->data + H->pos, GPTR(guest_buf), n);
  H->pos += n;
  if (H->pos > f->size) f->size = H->pos;
  f->dirty = 1;
  return n;
}

int rg_fs_seek(u32 h, i64 dist, u32 method, u32 *newpos) {
  Handle *H = &g_handles[h - HBASE];
  File *f = &g_files[H->file];
  i64 base = method == 1 ? (i64)H->pos : method == 2 ? (i64)f->size : 0;
  i64 np = base + dist;
  if (method > 2 || np < 0 || np > 0x7fffffff) return 0;
  H->pos = (u32)np;
  if (newpos) *newpos = H->pos;
  return 1;
}

void rg_fs_set_eof(u32 h) {
  Handle *H = &g_handles[h - HBASE];
  File *f = &g_files[H->file];
  ensure_cap(f, H->pos);
  if (H->pos > f->size) rg_memset(f->data + f->size, 0, H->pos - f->size);
  f->size = H->pos;
  f->dirty = 1;
}

u32 rg_fs_size(u32 h) { return g_files[g_handles[h - HBASE].file].size; }

void rg_fs_close(u32 h) {
  if (rg_fs_is_handle(h)) g_handles[h - HBASE].used = 0;
}

/* ---- find-first/next ---------------------------------------------------- */
int rg_fs_is_find_handle(u32 fh) {
  return fh >= FBASE && fh < FBASE + RG_MAXF && g_finders[fh - FBASE].used;
}

int rg_fs_find_next(u32 fh) {
  if (!rg_fs_is_find_handle(fh)) return -1;
  Finder *F = &g_finders[fh - FBASE];
  F->stamp = ++g_find_clock;
  for (; F->pos < g_nfiles; F->pos++) {
    File *f = &g_files[F->pos];
    if (f->used && pattern_match(F->pattern, f->name)) return F->pos++;
  }
  return -1;
}

u32 rg_fs_find_first(const char *pattern, int *fi) {
  int slot = 0;
  for (int i = 0; i < RG_MAXF; i++) {
    if (!g_finders[i].used) { slot = i; break; }
    if (g_finders[i].stamp < g_finders[slot].stamp) slot = i;
  }
  Finder *F = &g_finders[slot];
  copyname(F->pattern, basename_of(pattern));
  F->pos = 0;
  F->used = 1;
  *fi = rg_fs_find_next(FBASE + slot);
  if (*fi < 0) { F->used = 0; return RG_FS_BAD; }
  return FBASE + slot;
}

void rg_fs_find_close(u32 fh) {
  if (rg_fs_is_find_handle(fh)) g_finders[fh - FBASE].used = 0;
}
