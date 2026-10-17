#!/usr/bin/env bash
# Build the ResetGeneration WASM port.
#   ./build.sh            # regenerate C from the DLL (if needed) and link wasm
#   ./build.sh gen        # force regenerate the recompiled C
set -euo pipefail
cd "$(dirname "$0")"

DLL=original/ResetGeneration/ResetGeneration-Win32-Web.dll
GEN=runtime/gen

if [ "${1:-}" = "gen" ] || [ ! -f "$GEN/rg_dispatch.c" ]; then
  echo "[*] recompiling DLL -> C"
  python3 recomp/generate.py "$DLL"
fi

mkdir -p build
CC=clang
CFLAGS="--target=wasm32 -O2 -ffreestanding -fno-builtin -nostdlib \
  -Wno-implicit-function-declaration -Wno-parentheses-equality -Iruntime -Iruntime/gen \
  -mbulk-memory -msign-ext -mmutable-globals"

SRCS=(runtime/rg_host.c runtime/rg_mem.c runtime/rg_core.c \
      runtime/rg_win32.c runtime/rg_fs.c runtime/rg_jni.c runtime/rg_api.c \
      $GEN/rg_dispatch.c $GEN/rg_iat.c)
for i in 0 1 2 3 4 5 6 7; do SRCS+=("$GEN/rg_gen_$i.c"); done

echo "[*] compiling ${#SRCS[@]} translation units"
OBJS=()
for s in "${SRCS[@]}"; do
  o="build/$(echo "$s" | tr '/.' '__').o"
  $CC $CFLAGS -c "$s" -o "$o" &
  OBJS+=("$o")
  # throttle to nproc parallel jobs
  while [ "$(jobs -r | wc -l)" -ge "$(nproc)" ]; do wait -n; done
done
wait

echo "[*] linking reset.wasm"
wasm-ld "${OBJS[@]}" -o build/reset.wasm \
  --no-entry --allow-undefined --export-dynamic \
  --initial-memory=$((64*1024*1024)) --max-memory=$((1024*1024*1024)) \
  -z stack-size=1048576 \
  --export=rg_boot --export=rg_setup --export=rg_init --export=rg_tick --export=rg_image_ptr \
  --export=rg_add_file --export=rg_alloc --export=rg_notify_pak \
  --export=rg_backbuffer --export=rg_backbuffer_size --export=rg_hostptr \
  --export=rg_key_down --export=rg_key_up --export=rg_key_typed \
  --export=rg_mouse_pressed --export=rg_release_all --export=rg_sound_fill \
  --export=rg_is_wait --export=rg_select_key --export=rg_back_key \
  --export=rg_set_volume --export=__heap_base --export=memory \
  --export=rg_file_count --export=rg_file_name --export=rg_file_ptr \
  --export=rg_file_len --export=rg_file_dirty --export=rg_file_clear_dirty

ls -la build/reset.wasm

echo "[*] assembling dist/"
mkdir -p dist/data
cp web/index.html web/rg.js dist/
cp build/reset.wasm dist/
cp runtime/rg_image.bin dist/
cp original/ResetGeneration/*.pak dist/data/
echo "[*] done -> dist/ (serve with: python3 -m http.server -d dist 8080)"
