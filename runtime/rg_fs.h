#ifndef RG_FS_H
#define RG_FS_H
#include "rg_defs.h"

/* Tiny in-memory filesystem matched by basename.  The game opens pak/save
 * files by full path (home dir + name); we key on the final path component. */
#define RG_FS_MAX 64

void rg_fs_init(void);
int  rg_fs_add(const char *name, const u8 *data, u32 size);  /* host ptr */
int  rg_fs_find(const char *basename);                        /* -1 if none */
int  rg_fs_create(const char *basename);                      /* empty, grows */

/* File contents for the embedder to read back (saves/settings). */
int  rg_fs_count(void);
const char *rg_fs_name(int i);
u8  *rg_fs_data(int i);
u32  rg_fs_len(int i);
int  rg_fs_dirty(int i);
void rg_fs_clear_dirty(int i);

/* Handle-based access for the Win32 shims. */
u32  rg_fs_open(const char *path, int create, int truncate);  /* -> handle */
int  rg_fs_is_handle(u32 h);
u32  rg_fs_read(u32 h, u32 guest_buf, u32 n);
u32  rg_fs_write(u32 h, u32 guest_buf, u32 n);
u32  rg_fs_seek(u32 h, i32 dist, u32 method, u32 *newpos_out);
u32  rg_fs_size_of(u32 h);
void rg_fs_close(u32 h);

/* FindFirstFile/FindNextFile enumeration. */
int  rg_fs_find_first(const char *pattern);   /* enumeration cookie or -1 */
int  rg_fs_find_next(int cookie);

#endif
