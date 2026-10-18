/* Public API exposed to the JS/Node embedder.
 *
 * Boot sequence mirrors what the standalone ResetGeneration.java host did:
 *   1. load the image, init runtime subsystems
 *   2. run the DLL entry (CRT init + C++ static constructors)
 *   3. setHomeDirectory, init(backbuffer, ...), then notify each .pak
 *   4. per frame: tick(); read the pixel buffer; sleep what the game asked for
 */
#include "rg_defs.h"
#include "rg_mem.h"
#include "rg_fs.h"
#include "rg_jni.h"
#include "rg_host.h"
#include "rg_win32.h"
#include "rg_image_meta.h"

_Static_assert(RG_IMAGE_BASE == RG_GUEST_BASE, "image must load at its preferred base");
_Static_assert(RG_IMAGE_BASE + RG_IMAGE_SIZE <= RG_TIB_ADDR, "image overlaps the runtime area");

#define EXPORT(n) __attribute__((export_name(#n)))
#define RG_SOUND_MAX 4096u       /* bytes per onSoundBufferEmpty call (Java used 1024) */

static CPU cpu;
static u32 g_backbuf_mem;        /* guest VA of the pixel memory */
static u32 g_soundbuf;           /* direct buffer object for audio */
static u32 g_soundbuf_mem;
static u32 g_sw, g_sh;           /* screen size */

/* resolved Java_rg_natives_* export addresses */
static u32 e_init, e_tick, e_setHomeDirectory, e_notifyPak, e_isInWaitScreen;
static u32 e_getSelectButton, e_getBackButton;
static u32 e_onKeyPressed, e_onKeyReleased, e_onKeyTyped, e_releaseAll;
static u32 e_onSoundBufferEmpty;

/* ---- helpers ---------------------------------------------------------- */
/* Exports are stdcall-decorated: "_Java_rg_natives_<Class>_<method>@<argbytes>". */
static u32 find_export(const char *cls_method) {
  static const char prefix[] = "_Java_rg_natives_";
  for (int i = 0; rg_exports[i].name; i++) {
    const char *n = rg_exports[i].name, *p = prefix, *m = cls_method;
    while (*p && *n == *p) { n++; p++; }
    if (*p) continue;
    while (*m && *n == *m) { n++; m++; }
    if (!*m && *n == '@') return rg_exports[i].addr;
  }
  return 0;
}

static void reset_cpu(void) {
  rg_memset(&cpu, 0, sizeof cpu);
  cpu.esp = RG_STACK_TOP;
  cpu.fpu_cw = 0x027f;           /* Windows default: 53-bit precision, round to nearest */
}

/* Call a guest function with stdcall/cdecl arguments; returns EAX. */
static u32 rg_call(u32 entry, const u32 *args, int n) {
  u32 save = cpu.esp;
  for (int i = n - 1; i >= 0; i--) { cpu.esp -= 4; Wr32(cpu.esp, args[i]); }
  cpu.esp -= 4; Wr32(cpu.esp, 0xdeadc0deu);   /* return address; never jumped to */
  rg_dispatch(&cpu, entry);
  cpu.esp = save;
  return cpu.eax;
}

static u32 call0(u32 fn) {
  u32 a[2] = { rg_jni_env(), rg_jni_this() };
  return fn ? rg_call(fn, a, 2) : 0;
}

/* Thread information block: SEH chain end, stack bounds, self pointer. */
static void init_tib(void) {
  rg_memset(GPTR(RG_TIB_ADDR), 0, 0x1000);
  Wr32(RG_TIB_ADDR + 0x00, 0xffffffffu);
  Wr32(RG_TIB_ADDR + 0x04, RG_STACK_TOP);
  Wr32(RG_TIB_ADDR + 0x08, RG_STACK_LIMIT);
  Wr32(RG_TIB_ADDR + 0x18, RG_TIB_ADDR);
}

/* ---- image loading ---------------------------------------------------- */
/* JS copies the flat image blob (rg_image_size bytes) to rg_image_ptr. */
EXPORT(rg_image_ptr) u32 rg_image_ptr(void) { return (u32)GPTR(RG_IMAGE_BASE); }
EXPORT(rg_image_size) u32 rg_image_size(void) { return RG_IMAGE_SIZE; }

EXPORT(rg_setup) void rg_setup(void) {
  rg_mem_init();
  init_tib();
  rg_fs_init();
  rg_win32_init();
  rg_jni_init();
  reset_cpu();
}

EXPORT(rg_boot) int rg_boot(void) {
  rg_fill_iat();                 /* resolve import slots to trampolines */
  /* Run the DLL entry: DllMain(hinst, DLL_PROCESS_ATTACH, 0). */
  u32 args[3] = { RG_IMAGE_BASE, 1, 0 };
  if (!rg_call(RG_IMAGE_ENTRY, args, 3)) return 0;

  e_init             = find_export("NativeGameEngine_init");
  e_tick             = find_export("NativeGameEngine_tick");
  e_setHomeDirectory = find_export("NativeGameEngine_setHomeDirectory");
  e_notifyPak        = find_export("NativeGameEngine_notifyDataPackageReady");
  e_isInWaitScreen   = find_export("NativeGameEngine_isInWaitScreen");
  e_getSelectButton  = find_export("NativeGameEngine_getSelectButton");
  e_getBackButton    = find_export("NativeGameEngine_getBackButton");
  e_onKeyPressed     = find_export("NativeInputEventListener_onKeyPressed");
  e_onKeyReleased    = find_export("NativeInputEventListener_onKeyReleased");
  e_onKeyTyped       = find_export("NativeInputEventListener_onKeyTyped");
  e_releaseAll       = find_export("NativeInputEventListener_releaseAllControls");
  e_onSoundBufferEmpty = find_export("NativeSoundOutputDevice_onSoundBufferEmpty");
  return e_init && e_tick;
}

/* ---- file registration ------------------------------------------------ */
/* The embedder writes file contents into rg_alloc'd memory and hands the
 * buffer over; the filesystem keeps it without copying. */
EXPORT(rg_alloc) u32 rg_alloc(u32 n) { return (u32)rg_host_alloc(n); }

EXPORT(rg_add_file) int rg_add_file(u32 name_host, u32 data_host, u32 size) {
  return rg_fs_add((const char *)name_host, (u8 *)data_host, size);
}

/* ---- init ------------------------------------------------------------- */
EXPORT(rg_init) u32 rg_init(u32 w, u32 h, u32 playerMode) {
  g_sw = w; g_sh = h;
  u32 pxsize = w * (h + 1) * 4;  /* the Java host allocates one spare row */
  g_backbuf_mem = rg_heap_alloc(pxsize);
  rg_memset(GPTR(g_backbuf_mem), 0, pxsize);
  u32 backbuf = rg_jbuffer_new(g_backbuf_mem, pxsize);

  g_soundbuf_mem = rg_heap_alloc(RG_SOUND_MAX);
  g_soundbuf = rg_jbuffer_new(g_soundbuf_mem, RG_SOUND_MAX);

  u32 env = rg_jni_env(), self = rg_jni_this();
  if (e_setHomeDirectory) {
    u32 a[3] = { env, self, rg_jstr_new("/rg") };
    rg_call(e_setHomeDirectory, a, 3);
  }
  /* init(backbuffer, lang, playerMode, w, h, cmdline, user, timeout, interval) */
  u32 a[11] = { env, self, backbuf, rg_jstr_new("en"), playerMode, w, h,
                rg_jstr_new(""), rg_jstr_new(""), 0, 0 };
  rg_call(e_init, a, 11);
  return g_backbuf_mem;
}

EXPORT(rg_notify_pak) void rg_notify_pak(u32 name_host) {
  if (!e_notifyPak) return;
  u32 a[3] = { rg_jni_env(), rg_jni_this(), rg_jstr_new((const char *)name_host) };
  rg_call(e_notifyPak, a, 3);
}

/* ---- per-frame -------------------------------------------------------- */
EXPORT(rg_tick) int rg_tick(void) {
  return (int)(call0(e_tick) & 0xff);   /* jboolean: false once the game quits */
}

/* Milliseconds the game asked to Sleep() since the last call: after a tick,
 * how long it wants to wait before the next frame (it targets 30 fps). */
EXPORT(rg_sleep_ms) u32 rg_sleep_ms(void) { return rg_win32_take_sleep(); }

EXPORT(rg_backbuffer) u32 rg_backbuffer(void) {
  return (u32)GPTR(g_backbuf_mem);       /* host byte offset; pixels are 0x00RRGGBB */
}

EXPORT(rg_is_wait) int rg_is_wait(void) { return (int)(call0(e_isInWaitScreen) & 0xff); }
EXPORT(rg_select_key) u32 rg_select_key(void) { return call0(e_getSelectButton) & 0xffff; }
EXPORT(rg_back_key) u32 rg_back_key(void) { return call0(e_getBackButton) & 0xffff; }

/* ---- input (AWT key codes) ------------------------------------------- */
EXPORT(rg_key_down) void rg_key_down(u32 when, u32 keycode, u32 keychar, u32 keyloc) {
  if (!e_onKeyPressed) return;
  u32 a[6] = { rg_jni_env(), rg_jni_this(), when, keycode, keychar, keyloc };
  rg_call(e_onKeyPressed, a, 6);
}
EXPORT(rg_key_up) void rg_key_up(u32 when, u32 keycode, u32 keychar, u32 keyloc) {
  if (!e_onKeyReleased) return;
  u32 a[6] = { rg_jni_env(), rg_jni_this(), when, keycode, keychar, keyloc };
  rg_call(e_onKeyReleased, a, 6);
}
EXPORT(rg_key_typed) void rg_key_typed(u32 when, u32 keychar) {
  if (!e_onKeyTyped) return;
  u32 a[4] = { rg_jni_env(), rg_jni_this(), when, keychar };
  rg_call(e_onKeyTyped, a, 4);
}
EXPORT(rg_release_all) void rg_release_all(void) { call0(e_releaseAll); }

/* ---- audio: 8 kHz, 16-bit signed, mono -------------------------------- */
EXPORT(rg_sound_fill) u32 rg_sound_fill(u32 nbytes) {
  if (nbytes > RG_SOUND_MAX) nbytes = RG_SOUND_MAX;
  if (e_onSoundBufferEmpty) {
    u32 a[4] = { rg_jni_env(), rg_jni_this(), g_soundbuf, nbytes };
    rg_call(e_onSoundBufferEmpty, a, 4);
  }
  return (u32)GPTR(g_soundbuf_mem);
}

/* ---- in-memory files (for save persistence) --------------------------- */
EXPORT(rg_file_count) int rg_file_count(void) { return rg_fs_count(); }
EXPORT(rg_file_name) u32 rg_file_name(int i) { return (u32)rg_fs_name(i); }
EXPORT(rg_file_ptr) u32 rg_file_ptr(int i) { return (u32)rg_fs_data(i); }
EXPORT(rg_file_len) u32 rg_file_len(int i) { return rg_fs_len(i); }
EXPORT(rg_file_dirty) int rg_file_dirty(int i) { return rg_fs_dirty(i); }
EXPORT(rg_file_deleted) int rg_file_deleted(int i) { return rg_fs_deleted(i); }
EXPORT(rg_file_clear_dirty) void rg_file_clear_dirty(int i) { rg_fs_clear_dirty(i); }

/* ---- diagnostics ------------------------------------------------------ */
EXPORT(rg_heap_top) u32 rg_heap_top(void) { return rg_heap_brk(); }
