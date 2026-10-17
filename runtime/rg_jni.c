/* Fake JNI environment.
 *
 * The guest native code reaches Java through the JNIEnv function table.  We
 * build that table in guest memory with every slot pointing at a unique
 * trampoline address; rg_dispatch() routes those addresses here.  Objects are
 * represented directly in guest memory:
 *   jstring       -> guest pointer to a NUL-terminated UTF-8 buffer
 *   direct buffer -> guest pointer to {u32 address, u32 capacity}
 *   jclass/jobject/jmethodID -> opaque non-zero tokens
 */
#include "rg_jni.h"
#include "rg_mem.h"
#include "rg_host.h"

#define JNI_GetVersion            4
#define JNI_FindClass             6
#define JNI_Throw                 13
#define JNI_ThrowNew              14
#define JNI_ExceptionOccurred     15
#define JNI_ExceptionDescribe     16
#define JNI_ExceptionClear        17
#define JNI_NewGlobalRef          21
#define JNI_DeleteGlobalRef       22
#define JNI_DeleteLocalRef        23
#define JNI_IsSameObject          24
#define JNI_GetObjectClass        31
#define JNI_GetMethodID           33
#define JNI_CallObjectMethod      34
#define JNI_CallBooleanMethod     37
#define JNI_CallIntMethod         49
#define JNI_CallVoidMethod        61
#define JNI_GetFieldID            94
#define JNI_GetStaticMethodID     113
#define JNI_GetStaticFieldID      144
#define JNI_NewStringUTF          167
#define JNI_GetStringUTFLength    168
#define JNI_GetStringUTFChars     169
#define JNI_ReleaseStringUTFChars 170
#define JNI_GetArrayLength        171
#define JNI_ExceptionCheck        228
#define JNI_NewDirectByteBuffer   229
#define JNI_GetDirectBufferAddress 230
#define JNI_GetDirectBufferCapacity 231

#define TRAMP 0x10000000u  /* unused; real value from rg_mem.h */

static u32 g_env;
static u32 g_this;
static const char *g_laststr = "";

void rg_jni_init(void) {
  /* function table: 512 entries just after the JNIEnv* slot */
  u32 tbl = RG_JNI_ENV + 4;
  for (u32 i = 0; i < RG_JNI_TRAMP_N; i++)
    Wr32(tbl + i * 4, RG_JNI_TRAMP + i * RG_JNI_TRAMP_STRIDE);
  Wr32(RG_JNI_ENV, tbl);       /* *env = table */
  g_env = RG_JNI_ENV;
  g_this = 0x00c0ffeeu;        /* dummy jobject */
}

u32 rg_jni_env(void) { return g_env; }
u32 rg_jni_this(void) { return g_this; }
const char *rg_jstr_last(void) { return g_laststr; }

u32 rg_jstr_new(const char *utf8) {
  u32 n = rg_strlen(utf8);
  u32 p = rg_heap_alloc(n + 1);
  for (u32 i = 0; i < n; i++) Wr8(p + i, (u8)utf8[i]);
  Wr8(p + n, 0);
  return p;
}

u32 rg_jbuffer_new(u32 addr, u32 cap) {
  u32 p = rg_heap_alloc(8);
  Wr32(p, addr);
  Wr32(p + 4, cap);
  return p;
}

#define A(i) Rd32(c->esp + 4u + 4u * (i))
/* stdcall return: pop ret addr + nargs dwords */
#define RET(nargs, val) do { c->eax = (u32)(val); c->esp += 4u + 4u * (nargs); return; } while (0)
#define RETV(nargs)     do { c->esp += 4u + 4u * (nargs); return; } while (0)
/* cdecl return (varargs Call*Method): caller cleans args, pop only ret addr */
#define RETC(val)       do { c->eax = (u32)(val); c->esp += 4u; return; } while (0)

static u32 guest_strlen(u32 p) { u32 n = 0; while (Rd8(p + n)) n++; return n; }

void rg_jni_call(CPU *c, u32 index) {
  switch (index) {
    case JNI_GetVersion: RET(1, 0x00010006u);

    case JNI_GetDirectBufferAddress: {       /* (env, buf) */
      u32 buf = A(1);
      RET(2, buf ? Rd32(buf) : 0);
    }
    case JNI_GetDirectBufferCapacity: {      /* (env, buf) -> jlong */
      u32 buf = A(1);
      c->edx = 0;
      RET(2, buf ? Rd32(buf + 4) : 0);
    }
    case JNI_NewDirectByteBuffer: {          /* (env, addr, capacity:jlong) */
      u32 addr = A(1);
      u32 cap = A(2);                        /* low 32 of jlong */
      RET(4, rg_jbuffer_new(addr, cap));     /* jlong occupies 2 slots */
    }

    case JNI_GetStringUTFChars: {            /* (env, jstr, isCopy) */
      u32 jstr = A(1), isCopy = A(2);
      if (isCopy) Wr8(isCopy, 0);
      RET(3, jstr);
    }
    case JNI_ReleaseStringUTFChars:          /* (env, jstr, chars) */
      RETV(3);
    case JNI_GetStringUTFLength:             /* (env, jstr) */
      RET(2, guest_strlen(A(1)));
    case JNI_NewStringUTF: {                 /* (env, const char*) */
      u32 src = A(1);
      if (!src) RET(2, 0);
      u32 n = guest_strlen(src);
      u32 p = rg_heap_alloc(n + 1);
      for (u32 i = 0; i <= n; i++) Wr8(p + i, Rd8(src + i));
      RET(2, p);
    }

    case JNI_FindClass: RET(2, 0x00010001u);
    case JNI_GetObjectClass: RET(2, 0x00010001u);
    case JNI_GetMethodID: RET(4, 0x00020001u);
    case JNI_GetStaticMethodID: RET(4, 0x00020002u);
    case JNI_GetFieldID: RET(4, 0x00030001u);
    case JNI_GetStaticFieldID: RET(4, 0x00030002u);

    case JNI_ExceptionOccurred: RET(1, 0);
    case JNI_ExceptionCheck: RET(1, 0);
    case JNI_ExceptionClear: RETV(1);
    case JNI_ExceptionDescribe: RETV(1);
    case JNI_Throw: RET(2, 0);
    case JNI_ThrowNew: RET(3, 0);

    case JNI_NewGlobalRef: RET(2, A(1));
    case JNI_DeleteGlobalRef: RETV(2);
    case JNI_DeleteLocalRef: RETV(2);
    case JNI_IsSameObject: RET(3, A(1) == A(2));
    case JNI_GetArrayLength: RET(2, 0);

    /* varargs Call*Method: __cdecl, caller cleans the stack */
    case JNI_CallVoidMethod: RETC(0);
    case JNI_CallIntMethod: RETC(0);
    case JNI_CallBooleanMethod: RETC(0);
    case JNI_CallObjectMethod: RETC(0);

    default: {
      /* Report the offending table index to help extend coverage. */
      static char m[48];
      const char *q = "JNI index "; char *p = m;
      while (*q) *p++ = *q++;
      u32 v = index; char tmp[12]; int n = 0;
      do { tmp[n++] = '0' + (v % 10); v /= 10; } while (v);
      while (n) *p++ = tmp[--n];
      *p = 0;
      rg_host_panic(m);
    }
  }
}
