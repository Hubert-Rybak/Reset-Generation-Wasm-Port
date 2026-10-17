# Reset Generation — WebAssembly port

A browser port of **Reset Generation**, the 2008 N‑Gage turn‑based puzzle/strategy
game by RedLynx. The original has no source code available: the game logic lives
entirely inside a stripped native library (`ResetGeneration-Win32-Web.dll`, a
32‑bit x86 Windows DLL that was driven by a thin Java applet). This project makes
it run in the browser by **statically recompiling that DLL's machine code to C**,
compiling the C to WebAssembly, and reimplementing the thin Java/JNI host in
JavaScript.

No emulator, no x86 interpreter at runtime — the game's own code is translated
ahead of time into ~476,000 lines of C across 13,572 functions, then compiled to
a single `.wasm` module.

<p align="center"><em>RedLynx splash → intro → main menu → Story Mode → the battle board, all running in WASM.</em></p>

## How it works

The original standalone build is a Java applet (`ResetGeneration.jar`) that does
almost nothing but shuffle bytes between AWT and a native engine:

- It allocates a direct `IntBuffer` as the framebuffer and hands it to the native
  `init()`. The engine software‑renders each frame straight into that buffer.
- Each frame it calls the native `tick()`, then blits the buffer to the screen.
- Keyboard/mouse events are forwarded to native `onKeyPressed/Released/...`.
- Audio is pulled from native `onSoundBufferEmpty()` (8 kHz / 16‑bit / mono).
- The engine reads its `.pak` asset archives itself through the Win32 file API
  and decompresses them with a statically‑linked zlib.

So the entire game — menus, cutscenes, the puzzle board, AI, audio synthesis — is
in the DLL. The port reproduces exactly the small surface the DLL touches.

### 1. Static recompiler (`recomp/`, Python + [iced‑x86](https://github.com/icedland/iced))

- **`pe.py`** maps the DLL's sections at their preferred base address
  (`0x10000000`). Because the load base matches the preferred base, the absolute
  addresses baked into the code and data (vtables, jump tables, string pointers)
  are already correct — **no relocations need to be applied at run time**.
- **`disasm.py`** does recursive‑descent code discovery from the entry point,
  the 43 exports, and every code pointer revealed by the relocation table. It
  recovers basic blocks, jump tables (including negatively‑biased CRT tables),
  tail calls, and `noreturn` functions. Coverage is complete: 0 reachable
  instructions are left untranslated.
- **`codegen.py`** emits one C function `sub_<addr>(CPU *c)` per guest function.
  Registers live in a `CPU` struct; guest memory is one flat array. Highlights:
  - **Flags** are materialised eagerly but only where a later instruction
    actually reads them (backward flag‑liveness analysis), so `cmp`/`jcc`
    compiles to just the flags the branch needs.
  - **Calls** use the host call stack: a guest `call` pushes a return address
    (to keep `esp`‑relative accesses correct) and invokes the C function
    directly; `ret` is a C `return`. Indirect calls and vtable tail‑jumps go
    through a binary‑search dispatcher (guest address → C function).
  - **x87 FPU** is modelled with a 8‑deep `double` stack; the handful of reachable
    **SSE2** opcodes (a CRT double‑rounding helper) are implemented on an `xmm`
    register file; string ops, `mul`/`div`, `shld/shrd`, `bsr/bsf`, `setcc`,
    `cmov`, etc. are all covered.

### 2. Runtime (`runtime/`, hand‑written C → wasm32, freestanding)

- **`rg_mem.c`** — guest address space and a simple heap allocator (`HeapAlloc`/
  `VirtualAlloc`).
- **`rg_win32.c`** — the 87 imported KERNEL32/WINMM functions: heap, files
  (over an in‑memory FS), TLS, critical sections (single‑threaded no‑ops), time,
  locale/codepage, module/version stubs.
- **`rg_jni.c`** — a fake `JNIEnv`: the function table is built in guest memory
  with every slot pointing at a trampoline the dispatcher routes to a C handler.
  Implements direct‑buffer access, UTF‑8 strings, exception checks, etc.
- **`rg_fs.c`** — in‑memory filesystem (keyed by basename) holding the `.pak`
  archives and the save files.
- **`rg_core.c`** — dispatcher, x87/SSE/flags helpers, `cpuid`, and a from‑scratch
  `sin`/`cos`/`fmod` (freestanding wasm has no libm).
- **`rg_host.c`** — bump allocator over linear memory, a tiny libc (`memcpy`…),
  and the JS import glue.
- **`rg_api.c`** — the exported API: `rg_boot` (runs the DLL entry so the CRT and
  C++ static constructors initialise), `rg_init`, `rg_tick`, input, audio, saves.

### 3. Frontend (`web/`)

`index.html` + `rg.js`: fetches the wasm, the image blob and the `.pak` files
(with a progress bar), runs the frame loop into a scaled `<canvas>`, maps the
keyboard (and on‑screen touch controls) to the AWT key codes the engine expects
(**Ctrl = select, Shift = back, arrows = move**), plays audio through WebAudio,
and persists save files to IndexedDB.

## Building

Requirements: `clang`/`wasm-ld` (LLVM ≥ 15, with the `wasm32` target),
`python3` with `iced-x86` and `pefile` (`pip install iced-x86 pefile`).

```sh
./build.sh          # compile the runtime + generated C to build/reset.wasm, assemble dist/
./build.sh gen      # also re-run the recompiler (DLL -> C) first
```

The recompiler step produces `runtime/gen/` (the translated C, git‑ignored) and
`runtime/rg_image.bin` (the flat image blob) from the DLL in `original/`.

## Running

```sh
./build.sh
python3 -m http.server -d dist 8080
# open http://localhost:8080
```

Controls: **arrow keys** move, **Ctrl** selects, **Shift** goes back. Click the
screen twice for the in‑game help overlay. On touch devices an on‑screen D‑pad and
Select/Back buttons appear (toggle with the ⚙ button).

### Headless checks

```sh
node web/node_run.mjs 600        # boot + 600 ticks, writes build/frame.png
node web/node_play.mjs           # scripted playthrough into gameplay (filmstrip)
```

## Status

Working: boot/CRT init, all assets, menus, character/story screens, cutscenes,
the battle board and gameplay, input, and audio synthesis — in both Node and
browsers.

Known limitations:

- **Online/multiplayer** is gone (the N‑Gage Arena servers are dead; the SNAP
  networking layer is stubbed). Single‑player is unaffected.
- `setVolume` touches a mixer object the pull‑based audio path doesn't fully
  initialise, so programmatic volume changes are a no‑op (audio itself plays
  fine); the frontend doesn't call it.
- C++ exceptions / `longjmp` are mapped to traps — they're only reachable on
  genuine fatal errors (e.g. OOM), which don't occur in normal play.

## Legal

Reset Generation is © RedLynx / Nokia. This repository contains tooling and a
host runtime; the original game assets in `original/` are included only as the
input to the port. It is a preservation/compatibility project for a game whose
official distribution ended years ago.
