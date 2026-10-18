#ifndef RG_WIN32_H
#define RG_WIN32_H
#include "rg_defs.h"

void rg_win32_init(void);
u32  rg_win32_take_sleep(void);   /* ms passed to Sleep() since the last call */

#endif
