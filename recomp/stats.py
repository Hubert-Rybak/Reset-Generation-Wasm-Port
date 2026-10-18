"""Report code discovery coverage for a PE image (development aid)."""

import collections
import os
import sys
import time

import iced_x86 as ix

from disasm import Analysis
from pe import Image


def main(path):
    t0 = time.time()
    img = Image(path)
    print(f"image base {img.base:#x} size {img.size:#x} entry {img.entry:#x}")
    print(f"{len(img.relocs)} relocations, {len(img.imports)} imports, {len(img.exports)} exports")
    an = Analysis(img)
    funcs = an.discover()
    print(f"discovery took {time.time() - t0:.1f}s")
    covered = set()
    total_insns = 0
    for f in funcs.values():
        total_insns += len(f.insns)
        for va in f.insns:
            insn = an.cache[va]
            covered.update(range(va, va + insn.len))
    print(f"{len(funcs)} functions, {total_insns} emitted instructions (with duplication)")
    uniq = set()
    for f in funcs.values():
        uniq.update(f.insns)
    print(f"{len(uniq)} unique instructions")
    t = img.text
    code = img.read(t.va, t.vsize)
    unc = []
    va = t.va
    while va < t.end:
        if va in covered or va in an.table_data or (va & ~3) in an.table_data:
            va += 1
            continue
        if code[va - t.va] in (0xCC, 0x90, 0x00):
            va += 1
            continue
        start = va
        while va < t.end and va not in covered and not ((va & ~3) in an.table_data):
            va += 1
        unc.append((start, va))
    nbytes = sum(e - s for s, e in unc)
    print(f"{len(unc)} uncovered regions, {nbytes} bytes")
    for s, e in unc[:60]:
        print(f"  {s:#x}-{e:#x} ({e - s}) {img.read(s, min(16, e - s)).hex()}")
    warn = collections.Counter()
    for f in funcs.values():
        for w in f.warnings:
            warn[w.split(" at ")[0].split(" ")[0:4].__str__()] += 1
    print("warnings:", warn.most_common(20))
    for f in list(funcs.values()):
        for w in f.warnings[:3]:
            print(f"  {f.entry:#x}: {w}")
    dyn = [(f.entry, j) for f in funcs.values() for j in f.dyn_jumps]
    print(f"{len(dyn)} dynamic jumps:")
    for e, j in dyn[:40]:
        print(f"  in {e:#x}: {j:#x} {an.text_of(an.cache[j])}")
    print(f"{len(an.noreturn)} noreturn functions: {[hex(x) for x in sorted(an.noreturn)][:40]}")
    tails = sum(len(f.tailcalls) for f in funcs.values())
    print(f"{tails} tail calls, {sum(len(f.tables) for f in funcs.values())} jump tables")
    mn = collections.Counter(an.cache[va].ins.code for va in uniq)
    names = {v: k for k, v in ix.Code.__dict__.items() if not k.startswith("_")}
    print(f"{len(mn)} distinct opcode forms:")
    for c, n in mn.most_common():
        print(f"{n:8d} {names.get(c, c)}")


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else
         os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "original",
                      "ResetGeneration", "ResetGeneration-Win32-Web.dll"))
