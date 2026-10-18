#ifndef RG_FS_H
#define RG_FS_H
#include "rg_defs.h"

/* Tiny in-memory filesystem.  The game opens pak and save files by full path
 * (home directory + name); files are keyed by the final path component,
 * compared case-insensitively like on Windows. */
#define RG_FS_MAX 64
#define RG_FS_BAD 0xffffffffu      /* INVALID_HANDLE_VALUE */

void rg_fs_init(void);
int  rg_fs_add(const char *name, u8 *data, u32 size);   /* adopts host buffer */
int  rg_fs_find(const char *path);                      /* file index or -1 */
int  rg_fs_create(const char *path);                    /* new or truncated */
int  rg_fs_delete(const char *path);                    /* 1 if it existed */

/* File contents for the embedder (save persistence). */
int  rg_fs_count(void);
const char *rg_fs_name(int i);
u8  *rg_fs_data(int i);
u32  rg_fs_len(int i);
int  rg_fs_dirty(int i);        /* changed since the last rg_fs_clear_dirty */
int  rg_fs_deleted(int i);      /* removed by the game */
void rg_fs_clear_dirty(int i);

/* Handle-based access for the Win32 shims. */
u32  rg_fs_open(int fi);                                /* -> handle */
int  rg_fs_is_handle(u32 h);
u32  rg_fs_read(u32 h, u32 guest_buf, u32 n);
u32  rg_fs_write(u32 h, u32 guest_buf, u32 n);
int  rg_fs_seek(u32 h, i64 dist, u32 method, u32 *newpos);  /* 0 on error */
void rg_fs_set_eof(u32 h);
u32  rg_fs_size(u32 h);
void rg_fs_close(u32 h);

/* FindFirstFile/FindNextFile: `*` and `?` wildcards over the basename. */
u32  rg_fs_find_first(const char *pattern, int *fi);   /* -> find handle */
int  rg_fs_find_next(u32 fh);                          /* file index or -1 */
int  rg_fs_is_find_handle(u32 fh);
void rg_fs_find_close(u32 fh);

#endif
