#ifndef RG_HOST_H
#define RG_HOST_H
#include "rg_defs.h"

/* Host services (backed by wasm intrinsics + JS imports). */
void  *rg_host_alloc(u32 size);           /* grow linear memory, never freed */
void   rg_host_panic(const char *msg) __attribute__((noreturn));
void   rg_host_logn(const char *msg, u32 len);
double rg_host_now_ms(void);              /* monotonic clock the game runs on */
double rg_host_wall_ms(void);             /* wall clock: ms since 1970 (UTC) */

/* Freestanding libc subset used by the runtime. */
void  *rg_memcpy(void *d, const void *s, u32 n);
void  *rg_memset(void *d, int v, u32 n);
u32    rg_strlen(const char *s);

#endif
