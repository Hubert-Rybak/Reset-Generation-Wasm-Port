"""Code discovery for the static recompiler.

Finds every guest function reachable from the DLL's entry point, its exports
and the code pointers revealed by the base-relocation table, and builds an
instruction-level control-flow graph for each of them.

A "function" here is simply an entry address plus every instruction that is
reachable from it through non-call control flow.  Jumps to another function's
entry are treated as tail calls, so a block that is reachable from two entries
is simply emitted twice.  That keeps the translation semantically safe even for
code that does not follow a clean one-entry-per-function layout.
"""

import collections

import iced_x86 as ix

FC = ix.FlowControl
M = ix.Mnemonic
OK = ix.OpKind
R = ix.Register


class Insn:
    __slots__ = (
        "va",
        "len",
        "ins",
        "mn",
        "flow",
        "reloc_offs",
        "text",
    )

    @property
    def next(self):
        return self.va + self.len


class FuncInfo:
    def __init__(self, entry):
        self.entry = entry
        self.insns = []  # sorted list of instruction VAs
        self.succ = {}  # va -> list of intra-procedural successor VAs
        self.labels = set()  # VAs that need a label (branch targets)
        self.tailcalls = {}  # va -> target VA (direct jmp/jcc to another entry)
        self.tables = {}  # va of indirect jmp -> list of targets
        self.dyn_jumps = set()  # vas of indirect jumps we could not resolve
        self.has_ret = False
        self.noreturn_calls = set()  # call sites that never return
        self.import_tails = {}  # va of `jmp [IAT]` -> (dll, name)
        self.warnings = []


class Analysis:
    def __init__(self, img, log=print):
        self.img = img
        self.log = log
        t = img.text
        self._code = img.read(t.va, t.vsize)
        self._dec = ix.Decoder(32, self._code, ip=t.va)
        self.cache = {}
        self.entries = set()
        self.table_data = set()  # VAs of jump table dwords
        self.table_starts = {}  # table base VA -> list of targets
        self.noreturn = set()  # function entries known never to return
        self.noreturn_imports = {"ExitProcess"}
        self.funcs = {}
        self.fmt = ix.Formatter(ix.FormatterSyntax.INTEL)

    # ------------------------------------------------------------------
    def decode(self, va):
        insn = self.cache.get(va)
        if insn is not None:
            return insn
        if not self.img.in_text(va):
            return None
        d = self._dec
        d.position = va - self.img.text.va
        d.ip = va
        ins = d.decode()
        co = d.get_constant_offsets(ins)
        insn = Insn()
        insn.va = va
        insn.len = ins.len
        insn.ins = ins
        insn.mn = ins.mnemonic
        insn.flow = ins.flow_control
        offs = []
        if co.has_displacement:
            offs.append(("disp", co.displacement_offset, co.displacement_size))
        if co.has_immediate:
            offs.append(("imm", co.immediate_offset, co.immediate_size))
        if co.has_immediate2:
            offs.append(("imm2", co.immediate_offset2, co.immediate_size2))
        rel = {}
        for kind, off, size in offs:
            if size == 4 and (va + off) in self.img.relocs:
                rel[kind] = va + off
        # Any relocation that does not line up with an operand is suspicious.
        for b in range(va, va + ins.len):
            if b in self.img.relocs and b not in rel.values():
                rel.setdefault("other", b)
        insn.reloc_offs = rel
        insn.text = None
        self.cache[va] = insn
        return insn

    def text_of(self, insn):
        if insn.text is None:
            insn.text = self.fmt.format(insn.ins)
        return insn.text

    # ------------------------------------------------------------------
    def iat_target(self, insn):
        """For `call/jmp dword ptr [IAT slot]` return the import name."""
        ins = insn.ins
        if (
            ins.op_count == 1
            and ins.op0_kind == OK.MEMORY
            and ins.memory_base == R.NONE
            and ins.memory_index == R.NONE
        ):
            slot = ins.memory_displacement
            return self.img.imports.get(slot)
        return None

    def jump_table(self, insn):
        """Recognise `jmp dword ptr [reg*4 + table]` and return its targets."""
        ins = insn.ins
        if not (
            ins.op0_kind == OK.MEMORY
            and ins.memory_base == R.NONE
            and ins.memory_index != R.NONE
            and ins.memory_index_scale == 4
            and "disp" in insn.reloc_offs
        ):
            return None
        base = ins.memory_displacement
        if base in self.table_starts:
            return self.table_starts[base]

        def is_entry(a):
            return a in self.img.relocs and self.img.in_text(self.img.u32(a))

        # Hand-written CRT code indexes tables with a signed bias, e.g.
        # `jmp [edx*4 - 8 + Vec]`, so the live entries can sit a few dwords
        # before or after the displacement.  Find the nearest relocated code
        # pointer within a small window, then take the maximal contiguous run
        # of code pointers around it (a non-pointer dword separates tables).
        seed = None
        for off in (0, -4, 4, -8, 8, -12, 12):
            if is_entry(base + off):
                seed = base + off
                break
        if seed is None:
            return None
        start = seed
        while is_entry(start - 4) and (start - 4) not in self.table_starts:
            start -= 4
        targets = []
        a = start
        while is_entry(a):
            if a != start and a in self.table_starts:
                break
            targets.append(self.img.u32(a))
            self.table_data.add(a)
            a += 4
        if not targets:
            return None
        self.table_starts[base] = targets
        return targets

    # ------------------------------------------------------------------
    def explore(self, entry, final=False):
        """Explore the function at `entry`.

        In the non-final pass we only harvest call targets; jumps are always
        followed.  In the final pass jumps to other entries become tail calls.
        """
        f = FuncInfo(entry)
        seen = set()
        work = [entry]
        calls = set()
        while work:
            va = work.pop()
            while True:
                if va in seen:
                    break
                insn = self.decode(va)
                if insn is None:
                    f.warnings.append(f"flow leaves .text at {va:#x}")
                    break
                if va in self.table_data:
                    f.warnings.append(f"flow runs into jump table at {va:#x}")
                seen.add(va)
                succ = []
                fl = insn.flow
                mn = insn.mn
                stop = False
                if insn.ins.code == ix.Code.INVALID:
                    f.warnings.append(f"invalid instruction at {va:#x}")
                    stop = True
                elif mn == M.INT3 or mn == M.HLT or mn == M.UD2:
                    stop = True
                elif fl == FC.NEXT:
                    succ.append(insn.next)
                elif fl == FC.CALL:
                    tgt = insn.ins.near_branch_target
                    if tgt == insn.next:
                        f.warnings.append(f"call $+5 at {va:#x}")
                    calls.add(tgt)
                    if tgt in self.noreturn:
                        f.noreturn_calls.add(va)
                        stop = True
                    else:
                        succ.append(insn.next)
                elif fl == FC.INDIRECT_CALL:
                    imp = self.iat_target(insn)
                    if imp and imp[1] in self.noreturn_imports:
                        f.noreturn_calls.add(va)
                        stop = True
                    else:
                        succ.append(insn.next)
                elif fl == FC.CONDITIONAL_BRANCH:
                    tgt = insn.ins.near_branch_target
                    if final and tgt in self.entries and tgt != entry:
                        f.tailcalls[va] = tgt
                    else:
                        succ.append(tgt)
                        f.labels.add(tgt)
                    succ.append(insn.next)
                elif fl == FC.UNCONDITIONAL_BRANCH:
                    tgt = insn.ins.near_branch_target
                    if final and tgt in self.entries and tgt != entry:
                        f.tailcalls[va] = tgt
                    elif not self.img.in_text(tgt):
                        f.warnings.append(f"jump outside text at {va:#x}")
                    else:
                        succ.append(tgt)
                        f.labels.add(tgt)
                elif fl == FC.INDIRECT_BRANCH:
                    imp = self.iat_target(insn)
                    tbl = None if imp else self.jump_table(insn)
                    if imp is not None:
                        f.import_tails[va] = imp
                    elif tbl is not None:
                        f.tables[va] = tbl
                        for t in tbl:
                            if final and t in self.entries and t != entry:
                                f.warnings.append(
                                    f"jump table entry {t:#x} is a function entry"
                                )
                            succ.append(t)
                            f.labels.add(t)
                    else:
                        f.dyn_jumps.add(va)
                elif fl == FC.RETURN:
                    f.has_ret = True
                elif fl == FC.INTERRUPT:
                    stop = True
                elif fl == FC.EXCEPTION:
                    stop = True
                else:
                    f.warnings.append(f"unhandled flow {fl} at {va:#x}")
                    stop = True
                f.succ[va] = succ
                if stop or not succ:
                    break
                # Continue linearly with the fall-through successor; push others.
                fall = insn.next if (fl in (FC.NEXT, FC.CALL, FC.INDIRECT_CALL, FC.CONDITIONAL_BRANCH)) else None
                for s in succ:
                    if s != fall:
                        work.append(s)
                if fall is None:
                    break
                va = fall
        f.insns = sorted(seen)
        f.calls = calls
        return f

    # ------------------------------------------------------------------
    def code_pointer_seeds(self):
        """Relocated values that point into .text and look like code pointers."""
        img = self.img
        seeds = set()
        for r in img.relocs:
            v = img.u32(r)
            if not img.in_text(v):
                continue
            if v in self.table_starts or v in self.table_data:
                continue
            if img.in_text(r):
                # The relocation is either inside an instruction or jump table data.
                if r in self.table_data:
                    continue
                seeds.add(("code", r, v))
            else:
                seeds.add(("data", r, v))
        return seeds

    def owner_insn(self, va, window=15):
        for a in range(va, va - window, -1):
            insn = self.cache.get(a)
            if insn is not None and a <= va < a + insn.len:
                return insn
        return None

    def discover(self, extra_entries=()):
        img = self.img
        self.entries = set([img.entry]) | set(img.exports.values()) | set(extra_entries)
        explored = {}
        pending = list(self.entries)
        rounds = 0
        while True:
            rounds += 1
            while pending:
                e = pending.pop()
                if e in explored:
                    continue
                f = self.explore(e, final=False)
                explored[e] = f
                for c in f.calls:
                    if c not in self.entries and img.in_text(c):
                        self.entries.add(c)
                        pending.append(c)
            # Harvest code pointers from relocations.
            added = 0
            for kind, r, v in self.code_pointer_seeds():
                if v in self.entries:
                    continue
                if kind == "code":
                    owner = self.owner_insn(r)
                    if owner is None:
                        continue  # data in .text we did not reach; ignore
                    ins = owner.ins
                    # `jmp/mov [table + idx*4]` table bases are excluded above.
                    if ins.mnemonic in (M.PUSH, M.MOV, M.CMP, M.LEA):
                        pass
                    else:
                        continue
                self.entries.add(v)
                pending.append(v)
                added += 1
            # Recompute noreturn set.
            nr_before = len(self.noreturn)
            self.compute_noreturn(explored)
            if not added and not pending and len(self.noreturn) == nr_before:
                break
            if len(self.noreturn) != nr_before:
                # Re-explore everything with the improved noreturn knowledge.
                explored = {}
                pending = list(self.entries)
        self.log(f"discovery converged after {rounds} rounds, {len(self.entries)} entries")
        # Final pass with tail-call detection.
        self.funcs = {}
        for e in sorted(self.entries):
            self.funcs[e] = self.explore(e, final=True)
        return self.funcs

    def compute_noreturn(self, explored):
        """A function never returns if no `ret`, import tail jump or dynamic
        jump is reachable from its entry (jumps are followed in this pass)."""
        for e, f in explored.items():
            if e in self.noreturn:
                continue
            if f.has_ret or f.dyn_jumps or f.import_tails:
                continue
            self.noreturn.add(e)
