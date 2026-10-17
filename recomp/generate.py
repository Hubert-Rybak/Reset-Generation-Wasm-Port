"""Drive the recompiler: emit all generated C into runtime/gen/."""

import os
import sys
import time

import iced_x86 as ix

from codegen import CodeGen, cname, impname, IMPORT_ARGBYTES
from disasm import Analysis
from pe import Image

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "..", "runtime", "gen")
NSHARDS = 8


def main(dll_path):
    t0 = time.time()
    img = Image(dll_path)
    an = Analysis(img)
    funcs = an.discover()
    print(f"discovered {len(funcs)} functions in {time.time()-t0:.1f}s")
    os.makedirs(OUT, exist_ok=True)
    gen = CodeGen(img, an)

    entries = sorted(funcs)
    # Emit function bodies into shards.
    shards = [[] for _ in range(NSHARDS)]
    t1 = time.time()
    errors = 0
    for idx, e in enumerate(entries):
        try:
            shards[idx % NSHARDS].append(gen.emit_func(e))
        except Exception as ex:
            errors += 1
            if errors < 20:
                print(f"  ERROR emitting {e:#x}: {ex}")
    if errors:
        print(f"{errors} functions failed to emit")
    for i, body in enumerate(shards):
        with open(os.path.join(OUT, f"rg_gen_{i}.c"), "w") as f:
            f.write('#include "rg_funcs.h"\n#include "rg_imports.h"\n\n')
            f.write("\n\n".join(body))
            f.write("\n")
    print(f"emitted {len(entries)} functions in {time.time()-t1:.1f}s")

    # Prototypes header.
    with open(os.path.join(OUT, "rg_funcs.h"), "w") as f:
        f.write("#ifndef RG_FUNCS_H\n#define RG_FUNCS_H\n")
        f.write('#include "rg_defs.h"\n')
        for e in entries:
            f.write(f"void {cname(e)}(CPU *c);\n")
        f.write("#endif\n")

    # Dispatch table: sorted {addr, fn} for binary search + export table.
    with open(os.path.join(OUT, "rg_dispatch.c"), "w") as f:
        f.write('#include "rg_funcs.h"\n\n')
        f.write("const RGEntry rg_entries[] = {\n")
        for e in entries:
            f.write(f"  {{0x{e:08x}u, {cname(e)}}},\n")
        f.write("};\n")
        f.write(f"const unsigned rg_entry_count = {len(entries)};\n\n")
        # Exported Java_* functions by name.
        f.write("const RGExport rg_exports[] = {\n")
        for name, va in sorted(img.exports.items()):
            if va in funcs:
                cn = name
                f.write(f'  {{"{name}", 0x{va:08x}u}},\n')
        f.write("  {0, 0}\n};\n")

    # Import shims + IAT resolution tables.
    used_imports = sorted({name for (_dll, name) in img.imports.values()})
    idx_of = {name: i for i, name in enumerate(used_imports)}
    with open(os.path.join(OUT, "rg_imports.h"), "w") as f:
        f.write("#ifndef RG_IMPORTS_H\n#define RG_IMPORTS_H\n")
        f.write('#include "rg_defs.h"\n')
        for name in used_imports:
            f.write(f"void {impname(name)}(CPU *c);\n")
        f.write("#endif\n")
    with open(os.path.join(OUT, "rg_iat.c"), "w") as f:
        f.write('#include "rg_funcs.h"\n#include "rg_imports.h"\n\n')
        f.write("void (*const rg_imp_fns[])(CPU *) = {\n")
        for name in used_imports:
            f.write(f"  {impname(name)},\n")
        f.write("};\n")
        f.write(f"const unsigned rg_imp_count = {len(used_imports)};\n\n")
        f.write("const RGIatSlot rg_iat[] = {\n")
        for slot, (_dll, name) in sorted(img.imports.items()):
            f.write(f"  {{0x{slot:08x}u, {idx_of[name]}}},\n")
        f.write("};\n")
        f.write(f"const unsigned rg_iat_count = {len(img.imports)};\n")

    # Flat image blob: already at preferred base, so no fixups needed.
    with open(os.path.join(OUT, "..", "rg_image.bin"), "wb") as f:
        f.write(bytes(img.mem))
    with open(os.path.join(OUT, "rg_image_meta.h"), "w") as f:
        f.write("#ifndef RG_IMAGE_META_H\n#define RG_IMAGE_META_H\n")
        f.write(f"#define RG_IMAGE_BASE 0x{img.base:08x}u\n")
        f.write(f"#define RG_IMAGE_SIZE 0x{img.size:08x}u\n")
        f.write(f"#define RG_IMAGE_ENTRY 0x{img.entry:08x}u\n")
        f.write("#endif\n")

    # Report any unhandled instructions that slipped into the output.
    nbad = 0
    for i in range(NSHARDS):
        with open(os.path.join(OUT, f"rg_gen_{i}.c")) as f:
            txt = f.read()
        nbad += txt.count("rg_unhandled(")
    print(f"{nbad} unhandled instruction sites emitted")
    print(f"total {time.time()-t0:.1f}s")


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else
         os.path.join(HERE, "..", "original", "ResetGeneration",
                      "ResetGeneration-Win32-Web.dll"))
