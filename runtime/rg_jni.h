#ifndef RG_JNI_H
#define RG_JNI_H
#include "rg_defs.h"

void rg_jni_init(void);        /* build JNIEnv + function table in guest mem */
u32  rg_jni_env(void);         /* guest JNIEnv* to pass as first arg */
u32  rg_jni_this(void);        /* dummy jobject */
u32  rg_jstr_new(const char *utf8);        /* host string -> guest jstring */
u32  rg_jbuffer_new(u32 addr, u32 cap);    /* direct buffer object */
const char *rg_jstr_last(void);            /* debug */

void rg_jni_call(CPU *c, u32 index);

#endif
