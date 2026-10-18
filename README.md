# Reset Generation — WebAssembly port

**Play it: <https://hubert-rybak.github.io/Reset-Generation-Wasm-Port/>** (desktop
and mobile browsers).

A browser port of **Reset Generation**, the 2008 N‑Gage turn‑based puzzle/strategy
game by RedLynx. No source code exists: the whole game lives in a stripped native
library (`ResetGeneration-Win32-Web.dll`, a 32‑bit x86 Windows DLL that a thin
Java applet drove). This project makes it run in the browser by **statically
recompiling that DLL's machine code to C**, compiling the C to WebAssembly, and
reimplementing the thin Java/JNI host in JavaScript.

There is no emulator and no x86 interpreter at run time. The game's own code is
translated ahead of time into about 880,000 lines of C across 13,572 functions,
and then compiled into one `.wasm` module.

## Controls

| | Keyboard | Touch (shown automatically on phones and tablets) | Gamepad |
|---|---|---|---|
| Move | arrow keys | D‑pad (slide between directions) | D‑pad / left stick |
| Select | <kbd>Ctrl</kbd> | **SELECT** | A / Start |
| Back | <kbd>Shift</kbd> | **BACK** | B / Back |

Select and Back follow the keys chosen in the game's own *Custom Keys* menu.
The buttons beside the screen mute the sound, show or hide the touch controls, and
go fullscreen where the browser allows it. Saves and
settings are kept in the browser's IndexedDB, and the game data (about 47 MB) is
cached after the first visit.

## How it works

The original standalone build is a Java applet (`ResetGeneration.jar`) that does
almost nothing but shuttle bytes between AWT and the native engine:

- It allocates a direct `IntBuffer` as the framebuffer and passes it to the native
  `init()`. The engine software‑renders every frame straight into that buffer.
- It calls the native `tick()` in a loop and blits the buffer to the screen. The
  engine paces itself at 30 fps by calling `Sleep()`.
- Keyboard events are forwarded to native `onKeyPressed/Released/Typed`, with OS
  auto‑repeat filtered out.
- A sound thread pulls audio from native `onSoundBufferEmpty()` in 1 KB chunks
  (8 kHz, 16‑bit, mono).
- The engine reads its `.pak` archives itself through the Win32 file API and
  inflates them with a statically linked zlib.

So everything is in the DLL: menus, cutscenes, the board, the AI and the audio
mixer. The port reproduces exactly the small surface the DLL touches.

### 1. Static recompiler (`recomp/`, Python + [iced‑x86](https://github.com/icedland/iced))

- **`pe.py`** maps the DLL's sections at their preferred base address
  (`0x10000000`). Because the image loads at that base, every absolute address baked
  into code and data (vtables, jump tables, string pointers) is already correct, so
  **no relocations are applied at run time**.
- **`disasm.py`** does recursive‑descent code discovery from the entry point, the
  43 exports, and every code pointer in the relocation table. It recovers basic
  blocks, jump tables (including the CRT's negatively biased ones), tail calls and
  `noreturn` functions. No reachable instruction is left untranslated.
- **`flags.py`** computes, for every instruction, which EFLAGS bits are still read
  later. The analysis is interprocedural: a call clobbers flags unless the callee
  reads them, and a function whose callers read flags after it returns keeps them
  live at its `ret`. One CRT helper used by `sin`/`cos` returns its result in ZF.
  Shifts by a variable count leave flags untouched when the count is zero, so they
  are not treated as definite writes.
- **`codegen.py`** emits one C function `sub_<addr>(CPU *c)` per guest function.
  Registers live in a `CPU` struct; guest memory is one flat array.
  - **Flags** are materialised eagerly, but only the ones that are live, so a
    `cmp`/`jcc` pair compiles to just what the branch needs.
  - **Calls** use the host call stack. A guest `call` pushes its return address
    (so `esp`‑relative code stays correct) and invokes the C function directly;
    `ret` is a C `return`. Indirect calls and vtable tail jumps go through a
    binary‑search dispatcher that maps guest addresses to C functions.
  - **x87** is modelled as an 8‑deep stack of doubles. The CRT runs the FPU at
    53‑bit precision, so results match. **SSE2** covers the handful of opcodes the
    CRT uses. String ops, `mul`/`div`, `shld`/`shrd`, `bsr`/`bsf`, `setcc`, `cmov`
    and the rest are covered too.

`python3 recomp/stats.py` prints discovery coverage and the opcode census.

### 2. Runtime (`runtime/`, freestanding C → wasm32)

- **`rg_mem.c`**: the guest address space, plus a first‑fit heap with coalescing
  behind `HeapAlloc`/`VirtualAlloc`.
- **`rg_win32.c`**: the 87 KERNEL32/WINMM imports. Heap and files (over the
  in‑memory filesystem, with Windows creation, deletion and wildcard‑search
  semantics), TLS, single‑threaded synchronisation, clocks, codepage and ctype
  tables. `Sleep()` is not a no‑op: it reports the requested time to the page,
  which schedules the next frame.
- **`rg_fs.c`**: the in‑memory filesystem that holds the `.pak` archives and
  whatever the game writes (saves, settings), with change and deletion tracking
  for persistence.
- **`rg_jni.c`**: a fake `JNIEnv`. The function table lives in guest memory and
  every slot points at a trampoline that the dispatcher routes to a C handler.
  The only Java callbacks the engine makes are the online service's; those report
  "not logged in".
- **`rg_core.c`, `rg_math.c`**: the dispatcher, the x87/SSE helpers (exact 80‑bit
  conversions, rounding per the control word, `FPREM`/`FPREM1`, `FSIN`/`FCOS`),
  `cpuid`, and libm replacements (freestanding wasm has no libm). `fmod` and
  `remainder` are exact; `sin` and `cos` are within 1 ulp.
- **`rg_host.c`**: the JS imports, a linear‑memory allocator, and `mem*` built on
  bulk‑memory instructions.
- **`rg_api.c`**: the exported API (boot, init, tick, input, audio, files).

### 3. Frontend (`web/`)

`rg.js` wraps the module. It keeps the game's clock and paces `tick()` the way
the engine expects. The engine keeps an absolute 30 fps schedule and only streams
in data in its idle time, so `rg.js` trims the clock it sees in two ways. A single
tick counts for at most 19 ms, and a tick may start at most 14 ms late. Small
display jitter is still caught up, but a slow frame, a hidden tab or a device that
cannot keep 30 fps produces slow motion instead of a burst of fast‑forward or a
frozen loading screen.

`index.html` handles:
- downloading the assets in parallel, with progress and Cache API caching;
- the screen (whole‑pixel scaling on standard‑density displays);
- touch, keyboard and gamepad input;
- audio pulled on demand like the Java sound thread, resampled from 8 kHz;
- save persistence;
- the PWA manifest, wake lock and fullscreen.

## Building

Requirements: `clang` and `wasm-ld` (LLVM ≥ 15 with the `wasm32` target), plus
`python3` with `iced-x86` and `pefile` (`pip install iced-x86 pefile`).

```sh
./build.sh          # regenerate the C if the recompiler/DLL changed, compile, link, assemble dist/
./build.sh gen      # force the recompiler to run first
python3 -m http.server -d dist 8080     # then open http://localhost:8080
```

The recompiler writes `runtime/gen/` (the translated C) and `runtime/rg_image.bin`
(the flat image) from the DLL in `original/`. Both are generated and not
committed. `dist/` is the complete static site.

## Tests

```sh
npm install                 # playwright-core, for the browser test only
node tests/run.mjs          # boot + 20 s of simulated time -> build/frame.png
node tests/play.mjs         # scripted playthrough into a level -> build/playthrough.png
node tests/soak.mjs 15      # 15 simulated minutes of random input
node tests/browser.mjs [url] # Chromium: desktop, phone and landscape phone
```

The browser test checks:
- that the touch controls appear on load and never cover the screen;
- that the game renders and reacts to input;
- that it runs at 30 ticks/s;
- that a settings change survives a page reload.

## Deploying

The site is static. GitHub Pages serves the `gh-pages` branch, which holds the
contents of `dist/` plus a `.nojekyll` file:

```sh
./build.sh
git worktree add /tmp/pages gh-pages && rsync -a --delete --exclude .git dist/ /tmp/pages/
git -C /tmp/pages add -A && git -C /tmp/pages commit -m "Deploy" && git -C /tmp/pages push origin gh-pages
```

## Status and limitations

Everything single‑player works: boot, the menus, settings (including key
remapping), the tutorial, story and arcade modes, cutscenes, the battle board,
audio, and saving.

- **Online play and Quick Match** used the N‑Gage Arena (SNAP) service, which no
  longer exists. The networking calls report "not logged in", and these menu
  entries return to the main menu.
- The applet's help and focus overlays were drawn by Java, not the engine, so
  they are not reproduced. The replay viewer (`setVolume`, `playReplay`, …) is not
  exposed either; the page has its own mute control instead.
- C++ exceptions and SEH unwinding are not supported. The game only reaches them
  on fatal errors.
- `FPREM`/`FPREM1` always complete in one step, and their quotient bits are not
  modelled (the CRT only checks C2). `sin`/`cos` lose accuracy beyond about
  ±1.6·10⁶ radians.

## Legal

Reset Generation is © RedLynx / Nokia. This repository contains the recompiler, the
runtime and the web frontend; the original game files in `original/` are included
only as the input to the port. It is a preservation project for a game whose
official distribution ended years ago.
