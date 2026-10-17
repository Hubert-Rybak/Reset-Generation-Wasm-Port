/* Public API exposed to the JS/Node embedder.
 *
 * Boot sequence mirrors what ResetGeneration.java / LauncherApplet did:
 *   1. load the image, init runtime subsystems
 *   2. run the DLL entry (CRT init + C++ static constructors)
 *   3. setHomeDirectory, init(backbuffer, ...), then notify each .pak
 *   4. per frame: tick(); read the pixel buffer
 */
#include "rg_defs.h"
#include "rg_mem.h"
#include "rg_fs.h"
#include "rg_jni.h"
#include "rg_host.h"

#define EXPORT(n) __attribute__((export_name(#n)))

void rg_win32_init(void);
void rg_mem_init(void);

extern unsigned char __heap_base;

static CPU cpu;
static u32 g_entry;              /* DLL entry point VA */
static u32 g_backbuf;            /* direct buffer object for pixels */
static u32 g_backbuf_mem;        /* raw pixel memory VA */
static u32 g_soundbuf;           /* direct buffer object for audio */
static u32 g_soundbuf_mem;
static u32 g_sw, g_sh;           /* screen size */

/* resolved export addresses */
static u32 e_init, e_deinit, e_tick, e_getLanguage, e_setHomeDirectory;
static u32 e_notifyPak, e_isInWaitScreen, e_getSelectButton, e_getBackButton;
static u32 e_getTime, e_getTotalTime, e_setTime, e_setVolume;
static u32 e_onKeyPressed, e_onKeyReleased, e_onKeyTyped, e_onMousePressed;
static u32 e_releaseAll, e_onSoundBufferEmpty;

/* ---- helpers ---------------------------------------------------------- */
static int contains(const char *hay, const char *needle) {
  for (const char *h = hay; *h; h++) {
    const char *a = h, *b = needle;
    while (*a && *b && *a == *b) { a++; b++; }
    if (!*b) return 1;
  }
  return 0;
}

static u32 find_export(const char *needle) {
  for (int i = 0; rg_exports[i].name; i++)
    if (contains(rg_exports[i].name, needle)) return rg_exports[i].addr;
  return 0;
}

static void reset_cpu(void) {
  rg_memset(&cpu, 0, sizeof cpu);
  cpu.esp = RG_STACK_TOP;
  cpu.fpu_top = 0;
  cpu.fpu_cw = 0x037f;
}

static u32 rg_call(u32 entry, const u32 *args, int n) {
  u32 save = cpu.esp;
  for (int i = n - 1; i >= 0; i--) { cpu.esp -= 4; Wr32(cpu.esp, args[i]); }
  cpu.esp -= 4; Wr32(cpu.esp, 0xdeadc0deu);   /* sentinel return address */
  rg_dispatch(&cpu, entry);
  cpu.esp = save;
  return cpu.eax;
}

/* ---- image loading ---------------------------------------------------- */
/* JS copies the flat image blob into guest memory at the preferred base. */
EXPORT(rg_image_ptr) u32 rg_image_ptr(void) { return (u32)GPTR(RG_GUEST_BASE); }

EXPORT(rg_setup) void rg_setup(void) {
  rg_mem_init();
  rg_fs_init();
  rg_win32_init();
  rg_jni_init();
  reset_cpu();
}

EXPORT(rg_boot) int rg_boot(u32 entry_va) {
  g_entry = entry_va;
  rg_fill_iat();                 /* resolve import slots to trampolines */
  /* Run the DLL entry: DllMain(hinst, DLL_PROCESS_ATTACH, 0). */
  u32 args[3] = { RG_GUEST_BASE, 1, 0 };
  rg_call(g_entry, args, 3);

  e_init             = find_export("NativeGameEngine_init");
  e_deinit           = find_export("NativeGameEngine_deinit");
  e_tick             = find_export("NativeGameEngine_tick");
  e_getLanguage      = find_export("NativeGameEngine_getLanguage");
  e_setHomeDirectory = find_export("NativeGameEngine_setHomeDirectory");
  e_notifyPak        = find_export("NativeGameEngine_notifyDataPackageReady");
  e_isInWaitScreen   = find_export("NativeGameEngine_isInWaitScreen");
  e_getSelectButton  = find_export("NativeGameEngine_getSelectButton");
  e_getBackButton    = find_export("NativeGameEngine_getBackButton");
  e_getTime          = find_export("NativeGameEngine_getTime");
  e_getTotalTime     = find_export("NativeGameEngine_getTotalTime");
  e_setTime          = find_export("NativeGameEngine_setTime");
  e_setVolume        = find_export("NativeGameEngine_setVolume");
  e_onKeyPressed     = find_export("NativeInputEventListener_onKeyPressed");
  e_onKeyReleased    = find_export("NativeInputEventListener_onKeyReleased");
  e_onKeyTyped       = find_export("NativeInputEventListener_onKeyTyped");
  e_onMousePressed   = find_export("NativeInputEventListener_onMousePressed");
  e_releaseAll       = find_export("NativeInputEventListener_releaseAllControls");
  e_onSoundBufferEmpty = find_export("NativeSoundOutputDevice_onSoundBufferEmpty");
  return e_init != 0 && e_tick != 0;
}

/* ---- file registration ------------------------------------------------ */
EXPORT(rg_alloc) u32 rg_alloc(u32 n) { return (u32)rg_host_alloc(n); }

EXPORT(rg_add_file) int rg_add_file(u32 name_host, u32 data_host, u32 size) {
  return rg_fs_add((const char *)name_host, (const u8 *)data_host, size);
}

/* ---- init ------------------------------------------------------------- */
EXPORT(rg_init) u32 rg_init(u32 w, u32 h, u32 playerMode) {
  g_sw = w; g_sh = h;
  u32 pxsize = w * (h + 1) * 4;
  g_backbuf_mem = rg_heap_alloc(pxsize);
  for (u32 i = 0; i < pxsize; i++) Wr8(g_backbuf_mem + i, 0);
  g_backbuf = rg_jbuffer_new(g_backbuf_mem, pxsize);

  g_soundbuf_mem = rg_heap_alloc(4096);
  g_soundbuf = rg_jbuffer_new(g_soundbuf_mem, 4096);

  u32 env = rg_jni_env(), self = rg_jni_this();
  u32 lang = rg_jstr_new("en");
  u32 cmd = rg_jstr_new("");
  u32 user = rg_jstr_new("");
  u32 home = rg_jstr_new("/rg");

  if (e_setHomeDirectory) {
    u32 a[3] = { env, self, home };
    rg_call(e_setHomeDirectory, a, 3);
  }
  /* init(backbuffer, lang, playerMode, w, h, cmdline, user, timeout, interval) */
  u32 a[11] = { env, self, g_backbuf, lang, playerMode, w, h, cmd, user, 0, 0 };
  rg_call(e_init, a, 11);
  return g_backbuf_mem;
}

EXPORT(rg_notify_pak) void rg_notify_pak(u32 name_host) {
  if (!e_notifyPak) return;
  u32 js = rg_jstr_new((const char *)name_host);
  u32 a[3] = { rg_jni_env(), rg_jni_this(), js };
  rg_call(e_notifyPak, a, 3);
}

/* ---- per-frame -------------------------------------------------------- */
EXPORT(rg_tick) int rg_tick(void) {
  u32 a[2] = { rg_jni_env(), rg_jni_this() };
  return (int)(rg_call(e_tick, a, 2) & 0xff);   /* jboolean */
}

EXPORT(rg_backbuffer) u32 rg_backbuffer(void) {
  return (u32)GPTR(g_backbuf_mem);   /* host byte offset for JS */
}
EXPORT(rg_backbuffer_size) u32 rg_backbuffer_size(void) {
  return g_sw * (g_sh + 1) * 4;
}

EXPORT(rg_is_wait) int rg_is_wait(void) {
  if (!e_isInWaitScreen) return 0;
  u32 a[2] = { rg_jni_env(), rg_jni_this() };
  return (int)(rg_call(e_isInWaitScreen, a, 2) & 0xff);
}

EXPORT(rg_select_key) u32 rg_select_key(void) {
  if (!e_getSelectButton) return 0;
  u32 a[2] = { rg_jni_env(), rg_jni_this() };
  return rg_call(e_getSelectButton, a, 2) & 0xffff;
}
EXPORT(rg_back_key) u32 rg_back_key(void) {
  if (!e_getBackButton) return 0;
  u32 a[2] = { rg_jni_env(), rg_jni_this() };
  return rg_call(e_getBackButton, a, 2) & 0xffff;
}

/* ---- input ------------------------------------------------------------ */
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
EXPORT(rg_mouse_pressed) void rg_mouse_pressed(u32 button) {
  if (!e_onMousePressed) return;
  u32 a[3] = { rg_jni_env(), rg_jni_this(), button };
  rg_call(e_onMousePressed, a, 3);
}
EXPORT(rg_release_all) void rg_release_all(void) {
  if (!e_releaseAll) return;
  u32 a[2] = { rg_jni_env(), rg_jni_this() };
  rg_call(e_releaseAll, a, 2);
}

/* ---- audio ------------------------------------------------------------ */
EXPORT(rg_sound_fill) u32 rg_sound_fill(u32 nbytes) {
  if (!e_onSoundBufferEmpty) return 0;
  if (nbytes > 4096) nbytes = 4096;
  u32 a[4] = { rg_jni_env(), rg_jni_this(), g_soundbuf, nbytes };
  rg_call(e_onSoundBufferEmpty, a, 4);
  return (u32)GPTR(g_soundbuf_mem);
}

EXPORT(rg_set_volume) void rg_set_volume(u32 vol) {
  if (!e_setVolume) return;
  u32 a[3] = { rg_jni_env(), rg_jni_this(), vol };
  rg_call(e_setVolume, a, 3);
}

/* host pointer of any guest address (for JS reads) */
EXPORT(rg_hostptr) u32 rg_hostptr(u32 va) { return (u32)GPTR(va); }

/* ---- save-file enumeration (for IndexedDB persistence) ---------------- */
EXPORT(rg_file_count) int rg_file_count(void) { return rg_fs_count(); }
EXPORT(rg_file_name) u32 rg_file_name(int i) { return (u32)rg_fs_name(i); }
EXPORT(rg_file_ptr) u32 rg_file_ptr(int i) { return (u32)rg_fs_data(i); }
EXPORT(rg_file_len) u32 rg_file_len(int i) { return rg_fs_len(i); }
EXPORT(rg_file_dirty) int rg_file_dirty(int i) { return rg_fs_dirty(i); }
EXPORT(rg_file_clear_dirty) void rg_file_clear_dirty(int i) { rg_fs_clear_dirty(i); }
