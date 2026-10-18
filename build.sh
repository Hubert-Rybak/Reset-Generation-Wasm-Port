#!/usr/bin/env bash
# Build the Reset Generation WebAssembly port.
#   ./build.sh            # (re)generate C from the DLL if stale, compile, link, assemble dist/
#   ./build.sh gen        # force the recompiler to run first
set -euo pipefail
cd "$(dirname "$0")"

DLL=original/ResetGeneration/ResetGeneration-Win32-Web.dll
GEN=runtime/gen

stale=0
if [ ! -f "$GEN/rg_dispatch.c" ] || [ ! -f runtime/rg_image.bin ]; then
  stale=1
else
  for f in recomp/*.py "$DLL"; do
    [ "$f" -nt "$GEN/rg_dispatch.c" ] && stale=1
  done
fi
if [ "${1:-}" = "gen" ] || [ "$stale" = 1 ]; then
  echo "[*] recompiling DLL -> C"
  python3 recomp/generate.py "$DLL"
fi

mkdir -p build
CC=${CC:-clang}
WASMLD=${WASMLD:-wasm-ld}
CFLAGS="--target=wasm32 -O2 -ffreestanding -fno-builtin -nostdlib \
  -mbulk-memory -msign-ext -mmutable-globals \
  -Werror=implicit-function-declaration -Wno-parentheses-equality \
  -Iruntime -I$GEN"

SRCS=(runtime/rg_host.c runtime/rg_mem.c runtime/rg_math.c runtime/rg_core.c
      runtime/rg_win32.c runtime/rg_fs.c runtime/rg_jni.c runtime/rg_api.c
      "$GEN/rg_dispatch.c" "$GEN/rg_iat.c")
for f in "$GEN"/rg_gen_*.c; do SRCS+=("$f"); done

echo "[*] compiling ${#SRCS[@]} translation units"
OBJS=() PIDS=()
for s in "${SRCS[@]}"; do
  o="build/$(echo "$s" | tr '/.' '__').o"
  OBJS+=("$o")
  $CC $CFLAGS -c "$s" -o "$o" &
  PIDS+=($!)
  while [ "$(jobs -rp | wc -l)" -ge "$(nproc)" ]; do wait -n || true; done
done
fail=0
for p in "${PIDS[@]}"; do wait "$p" || fail=1; done
[ "$fail" = 0 ] || { echo "compilation failed" >&2; exit 1; }

echo "[*] linking build/reset.wasm"
# Exports come from export_name attributes in rg_api.c; the JS imports are
# declared with import_module attributes, so any other undefined symbol fails.
$WASMLD "${OBJS[@]}" -o build/reset.wasm --no-entry \
  --initial-memory=$((64 * 1024 * 1024)) --max-memory=$((1024 * 1024 * 1024)) \
  -z stack-size=$((1024 * 1024))
ls -la build/reset.wasm

echo "[*] assembling dist/"
rm -rf dist
mkdir -p dist/data
cp web/index.html web/rg.js web/manifest.webmanifest web/icon-192.png web/icon-512.png dist/
cp build/reset.wasm runtime/rg_image.bin dist/
cp original/ResetGeneration/*.pak dist/data/
touch dist/.nojekyll   # GitHub Pages: serve the files as they are
# Uncompressed sizes (for the progress bar) and a content hash that versions
# the page's asset cache.
python3 - <<'EOF'
import hashlib, json, os
os.chdir("dist")
files = ["reset.wasm", "rg_image.bin"] + sorted("data/" + f for f in os.listdir("data"))
h = hashlib.sha256()
sizes = {}
for f in files:
    data = open(f, "rb").read()
    sizes[f] = len(data)
    h.update(f.encode() + b"\0" + data)
json.dump({"version": h.hexdigest()[:16], "sizes": sizes}, open("assets.json", "w"), indent=1)
EOF
echo "[*] done -> dist/ (serve with: python3 -m http.server -d dist 8080)"
