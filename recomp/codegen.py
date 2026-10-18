"""x86 -> C static recompiler for ResetGeneration-Win32-Web.dll.

Each discovered guest function becomes a C function `sub_<addr>(CPU *c)` that
manipulates the CPU register struct and the flat guest memory.  Control flow:

  * direct call        -> push return addr, call sub_<t>(c)        (host call)
  * indirect call      -> push return addr, rg_dispatch(c, target)
  * ret / ret imm      -> adjust esp, C return
  * direct jmp/jcc     -> goto local label, or tail-call sub_<t> + return
  * jump table         -> switch on loaded pointer -> goto local labels
  * indirect jmp       -> rg_dispatch(c, target); return   (vtable tail call)
  * import call/jmp    -> imp_<name>(c)                    (stdcall shim)

Guest and host share the image's preferred base, so absolute addresses baked
into the code and data are already correct and need no fixups at run time.
"""

import os

import iced_x86 as ix

import emitops as EO
import flags as FL
from disasm import Analysis
from pe import Image

M = ix.Mnemonic
OK = ix.OpKind
R = ix.Register
Code = ix.Code

MNEM_NAME = {int(v): k for k, v in ix.Mnemonic.__dict__.items()
             if isinstance(v, int) and not k.startswith("_")}


def st_index(reg):
    return reg - R.ST0


def is_xmm(reg):
    return R.XMM0 <= reg <= R.XMM7


def cname(va):
    return f"sub_{va:08x}"


def impname(name):
    return f"imp_{name}"


class CodeGen:
    def __init__(self, img, an, log=print):
        self.img = img
        self.an = an
        self.funcs = an.funcs
        self.live = FL.FlagAnalysis(an).run(log)  # entry -> {va: live-out flags}

    # ---- memory operand hooks used by emitops.Operands ------------------
    def mem_read(self, ins, n):
        ea = EO.mem_ea(ins)
        return f"Rd{n * 8}({ea})"

    def mem_write(self, ins, n, val):
        ea = EO.mem_ea(ins)
        return f"Wr{n * 8}({ea}, {val});"

    # ---------------------------------------------------------------------
    def emit_func(self, entry):
        f = self.funcs[entry]
        live = self.live[entry]
        lines = [f"void {cname(entry)}(CPU *c) {{"]
        insns = f.insns
        # The body is emitted in address order, but the function's entry may not
        # be its lowest address (e.g. a helper reached by a backward jump into a
        # shared tail).  Jump to the real entry on C function entry.
        if insns and insns[0] != entry:
            lines.append(f"  goto L{entry:08x};")
        for va in insns:
            insn = self.an.cache[va]
            if va in f.labels or va == entry:
                lines.append(f" L{va:08x}:;")
            body = self.emit_insn(insn, f, live.get(va, frozenset()))
            for ln in body:
                lines.append("  " + ln)
        lines.append("}")
        return "\n".join(lines)

    # ---------------------------------------------------------------------
    def goto_or_tail(self, f, tgt, cond=None):
        """Emit a branch to tgt.

        A target that belongs to this function's own body is a local jump; a
        target that is another function's entry is a tail call.  `explore`
        guarantees these are disjoint: a jump to a foreign entry is never
        pulled into f.insns, so membership in f.insns is the exact test.
        """
        if tgt in f.insns_set:
            stmt = f"goto L{tgt:08x};"
        elif tgt in self.funcs:
            stmt = f"{cname(tgt)}(c); return;"
        else:
            stmt = f"rg_bad_target(c, 0x{tgt:08x}u); return;"
        if cond is None:
            return [stmt]
        return [f"if ({cond}) {{ {stmt} }}"]

    def ret_addr(self, insn):
        return insn.next

    # ---------------------------------------------------------------------
    def emit_insn(self, insn, f, live):
        ins = insn.ins
        mn = ins.mnemonic
        op = EO.Operands(self, ins)
        va = insn.va
        code = ins.code

        # --- control flow -------------------------------------------------
        if ins.is_call_near:
            tgt = ins.near_branch_target
            ra = self.ret_addr(insn)
            call = f"c->esp -= 4; Wr32(c->esp, 0x{ra:08x}u); {cname(tgt)}(c);"
            if tgt in self.an.noreturn:
                return [call, "return;"]
            return [call]
        if ins.is_call_near_indirect:
            tgt = self.indirect_target(ins, op)
            ra = self.ret_addr(insn)
            imp = self.an.iat_target(insn)
            if imp:
                # Emulate the call's own stack push; the stdcall shim pops it.
                return [f"c->esp -= 4; Wr32(c->esp, 0x{ra:08x}u); {impname(imp[1])}(c);"]
            return [f"{{ u32 t = {tgt}; c->esp -= 4; Wr32(c->esp, 0x{ra:08x}u); rg_dispatch(c, t); }}"]
        if ins.is_jmp_short_or_near:
            tgt = ins.near_branch_target
            return self.goto_or_tail(f, tgt)
        if ins.is_jmp_near_indirect:
            imp = self.an.iat_target(insn)
            if imp:
                return [f"{impname(imp[1])}(c); return;"]
            if va in f.tables:
                return self.emit_jumptable(insn, f)
            tgt = self.indirect_target(ins, op)
            return [f"{{ u32 t = {tgt}; rg_dispatch(c, t); return; }}"]
        if ins.is_jcc_short_or_near:
            tgt = ins.near_branch_target
            cond = self.cc_expr(ins.condition_code)
            return self.goto_or_tail(f, tgt, cond)
        if code == Code.JECXZ_REL8_32:
            tgt = ins.near_branch_target
            return self.goto_or_tail(f, tgt, "c->ecx == 0")
        if insn.flow == ix.FlowControl.RETURN:
            n = 4
            if ins.op_count >= 1 and ins.op0_kind == OK.IMMEDIATE16:
                n += ins.immediate(0) & 0xFFFF
            return [f"c->esp += {n}; return;"]
        if mn in (M.INT3, M.HLT, M.UD2):
            return [f'rg_trap(c, 0x{va:08x}u, "int3/hlt");', "return;"]

        # --- plain mnemonics ---------------------------------------------
        name = MNEM_NAME.get(int(mn), "")
        if name.startswith("SET"):
            return self._setcc(op, ins.condition_code)
        if name.startswith("CMOV"):
            return [f"if ({self.cc_expr(ins.condition_code)}) {{ {op.wr(0, op.rd(1))} }}"]
        h = getattr(self, "i_" + name.lower(), None)
        if h is None:
            return self.unhandled(insn)
        return h(insn, op, live)

    def indirect_target(self, ins, op):
        # operand 0 is the call/jmp target (register or memory)
        k = ins.op0_kind
        if k == OK.REGISTER:
            return EO.reg_read(ins.op_register(0))
        if k == OK.MEMORY:
            return f"Rd32({EO.mem_ea(ins)})"
        raise ValueError("bad indirect target")

    def emit_jumptable(self, insn, f):
        tgts = f.tables[insn.va]
        ea = EO.mem_ea(insn.ins)
        out = [f"{{ u32 t = Rd32({ea});", "  switch (t) {"]
        seen = set()
        for t in tgts:
            if t in seen:
                continue
            seen.add(t)
            if t in f.insns_set:
                out.append(f"  case 0x{t:08x}u: goto L{t:08x};")
            else:
                out.append(f"  case 0x{t:08x}u: {cname(t)}(c); return;")
        out.append(f'  default: rg_bad_target(c, t); return;')
        out.append("  } }")
        return out

    # ---- condition codes -------------------------------------------------
    def cc_expr(self, cc):
        CC = ix.ConditionCode
        return {
            CC.O: "c->of", CC.NO: "!c->of",
            CC.B: "c->cf", CC.AE: "!c->cf",
            CC.E: "c->zf", CC.NE: "!c->zf",
            CC.BE: "(c->cf || c->zf)", CC.A: "(!c->cf && !c->zf)",
            CC.S: "c->sf", CC.NS: "!c->sf",
            CC.P: "c->pf", CC.NP: "!c->pf",
            CC.L: "(c->sf != c->of)", CC.GE: "(c->sf == c->of)",
            CC.LE: "(c->zf || (c->sf != c->of))", CC.G: "(!c->zf && (c->sf == c->of))",
        }[cc]

    def unhandled(self, insn):
        txt = self.an.text_of(insn).replace('"', "'")
        return [f'rg_unhandled(c, 0x{insn.va:08x}u, "{txt}");', "return;"]

    # =====================================================================
    # Instruction handlers.  Each returns a list of C statements.
    # =====================================================================
    def _alu(self, op, expr_fmt, fam, live, writeback=True):
        """Binary ALU op: dest = dest <op> src, set flags."""
        ins = op.ins
        sz = op.size(0)
        a = op.rd(0)
        b = op.rd(1)
        lines = [f"{{ u32 ta = {a}, tb = {b}, tr;"]
        lines.append(f"  tr = {expr_fmt.format(a='ta', b='tb')};")
        if writeback:
            lines.append("  " + op.wr(0, "tr"))
        fl = FL.emit(fam, sz, "ta", "tb", "tr", live)
        for s in fl:
            lines.append("  " + s)
        lines.append("}")
        return lines

    def i_mov(self, insn, op, live):
        return [op.wr(0, op.rd(1))]

    def i_movzx(self, insn, op, live):
        srcsz = op.size(1)
        mask = {1: "0xffu", 2: "0xffffu"}[srcsz]
        return [op.wr(0, f"({op.rd(1)} & {mask})")]

    def i_movsx(self, insn, op, live):
        srcsz = op.size(1)
        cast = {1: "(i32)(i8)", 2: "(i32)(i16)"}[srcsz]
        return [op.wr(0, f"(u32)({cast}({op.rd(1)}))")]

    def i_lea(self, insn, op, live):
        return [op.wr(0, EO.mem_ea(insn.ins))]

    def i_push(self, insn, op, live):
        v = op.rd(0)
        return [f"{{ u32 tv = {v}; c->esp -= 4; Wr32(c->esp, tv); }}"]

    def i_pop(self, insn, op, live):
        # An esp-based destination address is computed after the increment.
        return [f"{{ u32 tv = Rd32(c->esp); c->esp += 4; {op.wr(0, 'tv')} }}"]

    def i_add(self, insn, op, live):
        return self._alu(op, "{a} + {b}", "ADD", live)

    def i_sub(self, insn, op, live):
        return self._alu(op, "{a} - {b}", "SUB", live)

    def i_adc(self, insn, op, live):
        sz = op.size(0)
        lines = [f"{{ u32 ta = {op.rd(0)}, tb = {op.rd(1)}, tc = c->cf, tr;"]
        lines.append("  tr = ta + tb + tc;")
        lines.append("  " + op.wr(0, "tr"))
        for s in FL.emit("ADC", sz, "ta", "tb", "tr", live, carry="tc"):
            lines.append("  " + s)
        lines.append("}")
        return lines

    def i_sbb(self, insn, op, live):
        sz = op.size(0)
        lines = [f"{{ u32 ta = {op.rd(0)}, tb = {op.rd(1)}, tc = c->cf, tr;"]
        lines.append("  tr = ta - tb - tc;")
        lines.append("  " + op.wr(0, "tr"))
        for s in FL.emit("SBB", sz, "ta", "tb", "tr", live, carry="tc"):
            lines.append("  " + s)
        lines.append("}")
        return lines

    def i_cmp(self, insn, op, live):
        return self._alu(op, "{a} - {b}", "CMP", live, writeback=False)

    def i_and(self, insn, op, live):
        return self._alu(op, "{a} & {b}", "LOGIC", live)

    def i_or(self, insn, op, live):
        return self._alu(op, "{a} | {b}", "LOGIC", live)

    def i_xor(self, insn, op, live):
        return self._alu(op, "{a} ^ {b}", "LOGIC", live)

    def i_test(self, insn, op, live):
        return self._alu(op, "{a} & {b}", "LOGIC", live, writeback=False)

    def i_inc(self, insn, op, live):
        sz = op.size(0)
        a = op.rd(0)
        lines = [f"{{ u32 ta = {a}, tr = ta + 1;", "  " + op.wr(0, "tr")]
        for s in FL.emit("INC", sz, "ta", "1u", "tr", live):
            lines.append("  " + s)
        lines.append("}")
        return lines

    def i_dec(self, insn, op, live):
        sz = op.size(0)
        a = op.rd(0)
        lines = [f"{{ u32 ta = {a}, tr = ta - 1;", "  " + op.wr(0, "tr")]
        for s in FL.emit("DEC", sz, "ta", "1u", "tr", live):
            lines.append("  " + s)
        lines.append("}")
        return lines

    def i_neg(self, insn, op, live):
        sz = op.size(0)
        a = op.rd(0)
        lines = [f"{{ u32 ta = {a}, tr = (u32)(0 - ta);", "  " + op.wr(0, "tr")]
        for s in FL.emit("NEG", sz, "ta", "0u", "tr", live):
            lines.append("  " + s)
        lines.append("}")
        return lines

    def i_not(self, insn, op, live):
        return [op.wr(0, f"(~{op.rd(0)})")]

    # ---- shifts ----------------------------------------------------------
    def _shift(self, op, kind, live):
        """SHL/SHR/SAR.  The count is masked to 5 bits; a zero count changes
        neither the operand nor any flag."""
        ins = op.ins
        sz = op.size(0)
        imm = FL.shift_count(ins)
        if imm == 0:
            return []
        cnt = f"{imm}u" if imm is not None else f"({op.rd(1)} & 31)"
        sext = {1: "(i32)(i8)ta", 2: "(i32)(i16)ta", 4: "(i32)ta"}[sz]
        lines = [f"{{ u32 ta = {op.rd(0)} & {FL.MASK[sz]}, tn = {cnt}, tr;"]
        body = []
        if kind == "SHL":
            body.append("tr = ta << tn;")
        elif kind == "SHR":
            body.append("tr = ta >> tn;")
        else:  # SAR on the sign-extended value; tn <= 31 keeps C shifts defined
            body.append(f"tr = (u32)({sext} >> tn);")
        body.append(op.wr(0, "tr"))
        body += FL.emit(kind, sz, "ta", "0u", "tr", live, count="tn", sa=f"({sext})")
        if imm is None:
            lines.append("  if (tn) { " + " ".join(body) + " }")
        else:
            lines += ["  " + s for s in body]
        lines.append("}")
        return lines

    def i_shl(self, insn, op, live):
        return self._shift(op, "SHL", live)

    def i_sal(self, insn, op, live):
        return self._shift(op, "SHL", live)

    def i_shr(self, insn, op, live):
        return self._shift(op, "SHR", live)

    def i_sar(self, insn, op, live):
        return self._shift(op, "SAR", live)

    def i_rcr(self, insn, op, live):
        # Only the 32-bit rotate-through-carry by one is used by the binary.
        if op.size(0) != 4 or FL.shift_count(insn.ins) != 1:
            return self.unhandled(insn)
        lines = [f"{{ u32 ta = {op.rd(0)}, tr = (ta >> 1) | ((u32)c->cf << 31);"]
        lines.append("  " + op.wr(0, "tr"))
        if "cf" in live:
            lines.append("  c->cf = ta & 1;")
        if "of" in live:
            lines.append("  c->of = ((tr >> 31) ^ (tr >> 30)) & 1;")
        lines.append("}")
        return lines

    def _shiftd(self, insn, op, kind, live):
        """SHLD/SHRD r/m32, r32, imm8/CL (only 32-bit forms are used)."""
        ins = op.ins
        if op.size(0) != 4:
            return self.unhandled(insn)
        imm = FL.shift_count(ins)
        if imm == 0:
            return []
        cnt = f"{imm}u" if imm is not None else f"({op.rd(2)} & 31)"
        lines = [f"{{ u32 td = {op.rd(0)}, ts = {op.rd(1)}, tn = {cnt}, tr;"]
        if kind == "SHLD":
            body = ["tr = (td << tn) | (ts >> (32 - tn));"]
            if "cf" in live:
                body.append("c->cf = (td >> (32 - tn)) & 1;")
        else:
            body = ["tr = (td >> tn) | (ts << (32 - tn));"]
            if "cf" in live:
                body.append("c->cf = (td >> (tn - 1)) & 1;")
        body.append(op.wr(0, "tr"))
        if "of" in live:  # sign change on a 1-bit shift
            body.append("if (tn == 1) c->of = ((tr ^ td) >> 31) & 1;")
        if "zf" in live:
            body.append("c->zf = (tr == 0);")
        if "sf" in live:
            body.append("c->sf = tr >> 31;")
        if "pf" in live:
            body.append("c->pf = rg_parity(tr);")
        if imm is None:
            lines.append("  if (tn) { " + " ".join(body) + " }")
        else:
            lines += ["  " + s for s in body]
        lines.append("}")
        return lines

    def i_shld(self, insn, op, live):
        return self._shiftd(insn, op, "SHLD", live)

    def i_shrd(self, insn, op, live):
        return self._shiftd(insn, op, "SHRD", live)

    # ---- mul / div -------------------------------------------------------
    def i_mul(self, insn, op, live):
        sz = op.size(0)
        src = op.rd(0)
        if sz == 4:
            lines = [f"{{ u64 p = (u64)c->eax * (u64)({src});",
                     "  c->eax = (u32)p; c->edx = (u32)(p >> 32);"]
            if "cf" in live or "of" in live:
                lines.append("  c->cf = c->of = (c->edx != 0);")
        elif sz == 2:
            lines = [f"{{ u32 p = (c->eax & 0xffff) * (({src}) & 0xffff);",
                     "  c->eax = (c->eax & 0xffff0000u) | (p & 0xffff);",
                     "  c->edx = (c->edx & 0xffff0000u) | ((p >> 16) & 0xffff);"]
            if "cf" in live or "of" in live:
                lines.append("  c->cf = c->of = ((p >> 16) != 0);")
        else:
            lines = [f"{{ u32 p = (c->eax & 0xff) * (({src}) & 0xff);",
                     "  c->eax = (c->eax & 0xffff0000u) | (p & 0xffff);"]
            if "cf" in live or "of" in live:
                lines.append("  c->cf = c->of = ((p >> 8) != 0);")
        lines.append("}")
        return lines

    def i_imul(self, insn, op, live):
        ins = op.ins
        n = ins.op_count
        sz = op.size(0)
        if n == 1:
            src = op.rd(0)
            if sz == 4:
                lines = [f"{{ i64 p = (i64)(i32)c->eax * (i64)(i32)({src});",
                         "  c->eax = (u32)p; c->edx = (u32)((u64)p >> 32);"]
                if "cf" in live or "of" in live:
                    lines.append("  c->cf = c->of = ((i64)(i32)c->eax != p);")
            elif sz == 2:
                lines = [f"{{ i32 p = (i32)(i16)(c->eax) * (i32)(i16)({src});",
                         "  c->eax = (c->eax & 0xffff0000u) | ((u32)p & 0xffff);",
                         "  c->edx = (c->edx & 0xffff0000u) | (((u32)p >> 16) & 0xffff);"]
                if "cf" in live or "of" in live:
                    lines.append("  c->cf = c->of = ((i32)(i16)p != p);")
            else:
                lines = [f"{{ i32 p = (i32)(i8)(c->eax) * (i32)(i8)({src});",
                         "  c->eax = (c->eax & 0xffff0000u) | ((u32)p & 0xffff);"]
                if "cf" in live or "of" in live:
                    lines.append("  c->cf = c->of = ((i32)(i8)p != p);")
            lines.append("}")
            return lines
        # two or three operand forms (result truncated to the operand size)
        if n == 2:
            a, b = op.rd(0), op.rd(1)
        else:
            a, b = op.rd(1), op.rd(2)
        sx = {2: "(i64)(i16)", 4: "(i64)(i32)"}[sz]
        lines = [f"{{ i64 p = {sx}({a}) * {sx}({b}); u32 tr = (u32)p;",
                 "  " + op.wr(0, "tr")]
        if "cf" in live or "of" in live:
            lines.append(f"  c->cf = c->of = ({sx}tr != p);")
        lines.append("}")
        return lines

    def i_div(self, insn, op, live):
        sz = op.size(0)
        src = op.rd(0)
        if sz == 4:
            return [f"{{ u64 n = ((u64)c->edx << 32) | c->eax; u32 d = {src};",
                    "  if (!d) { rg_div0(c); return; }",
                    "  c->eax = (u32)(n / d); c->edx = (u32)(n % d); }"]
        if sz == 2:
            return [f"{{ u32 n = ((c->edx & 0xffff) << 16) | (c->eax & 0xffff); u32 d = {src} & 0xffff;",
                    "  if (!d) { rg_div0(c); return; }",
                    "  c->eax = (c->eax & 0xffff0000u) | ((n / d) & 0xffff);",
                    "  c->edx = (c->edx & 0xffff0000u) | ((n % d) & 0xffff); }"]
        return [f"{{ u32 n = c->eax & 0xffff; u32 d = {src} & 0xff;",
                "  if (!d) { rg_div0(c); return; }",
                "  c->eax = (c->eax & 0xffff0000u) | ((n / d) & 0xff) | (((n % d) & 0xff) << 8); }"]

    def i_idiv(self, insn, op, live):
        sz = op.size(0)
        src = op.rd(0)
        if sz == 4:
            return [f"{{ i64 n = (i64)(((u64)c->edx << 32) | c->eax); i32 d = (i32)({src});",
                    "  if (!d) { rg_div0(c); return; }",
                    "  c->eax = (u32)(i32)(n / d); c->edx = (u32)(i32)(n % d); }"]
        if sz == 2:
            return [f"{{ i32 n = (i32)(((u32)(c->edx & 0xffff) << 16) | (c->eax & 0xffff)); i32 d = (i32)(i16)({src});",
                    "  if (!d) { rg_div0(c); return; }",
                    "  c->eax = (c->eax & 0xffff0000u) | ((u32)(i32)(n / d) & 0xffff);",
                    "  c->edx = (c->edx & 0xffff0000u) | ((u32)(i32)(n % d) & 0xffff); }"]
        return [f"{{ i32 n = (i32)(i16)(c->eax & 0xffff); i32 d = (i32)(i8)({src});",
                "  if (!d) { rg_div0(c); return; }",
                "  c->eax = (c->eax & 0xffff0000u) | ((u32)(i32)(n / d) & 0xff) | (((u32)(i32)(n % d) & 0xff) << 8); }"]

    def i_cdq(self, insn, op, live):
        return ["c->edx = (c->eax & 0x80000000u) ? 0xffffffffu : 0u;"]

    def i_cwde(self, insn, op, live):
        return ["c->eax = (u32)(i32)(i16)(c->eax & 0xffff);"]

    def i_cbw(self, insn, op, live):
        return ["c->eax = (c->eax & 0xffff0000u) | ((u32)(i16)(i8)(c->eax & 0xff) & 0xffff);"]

    def i_cwd(self, insn, op, live):
        return ["c->edx = (c->edx & 0xffff0000u) | (((c->eax & 0x8000u) ? 0xffffu : 0u));"]

    # ---- bit scan --------------------------------------------------------
    def i_bsr(self, insn, op, live):
        src = op.rd(1)
        lines = [f"{{ u32 ts = {src};"]
        if "zf" in live:
            lines.append("  c->zf = (ts == 0);")
        lines.append("  if (ts) { " + op.wr(0, "(31 - (u32)__builtin_clz(ts))") + " } }")
        return lines

    def i_bsf(self, insn, op, live):
        src = op.rd(1)
        lines = [f"{{ u32 ts = {src};"]
        if "zf" in live:
            lines.append("  c->zf = (ts == 0);")
        lines.append("  if (ts) { " + op.wr(0, "(u32)__builtin_ctz(ts)") + " } }")
        return lines

    # ---- setcc -----------------------------------------------------------
    def _setcc(self, op, cc):
        return [op.wr(0, f"({self.cc_expr(cc)} ? 1u : 0u)")]

    # ---- xchg ------------------------------------------------------------
    def i_xchg(self, insn, op, live):
        return [f"{{ u32 t0 = {op.rd(0)}, t1 = {op.rd(1)};",
                "  " + op.wr(0, "t1"),
                "  " + op.wr(1, "t0") + " }"]

    def i_leave(self, insn, op, live):
        return ["c->esp = c->ebp; c->ebp = Rd32(c->esp); c->esp += 4;"]

    def i_nop(self, insn, op, live):
        return []

    def i_wait(self, insn, op, live):
        return []

    def i_fwait(self, insn, op, live):
        return []

    def i_fnclex(self, insn, op, live):
        # Clears the exception flags and busy bit; condition codes survive.
        return ["c->fpu_sw &= ~0x80ffu;"]

    # ---- flags direct ----------------------------------------------------
    def i_clc(self, insn, op, live):
        return ["c->cf = 0;"]

    def i_stc(self, insn, op, live):
        return ["c->cf = 1;"]

    def i_cmc(self, insn, op, live):
        return ["c->cf ^= 1;"]

    def i_cld(self, insn, op, live):
        return ["c->df = 0;"]

    def i_std(self, insn, op, live):
        return ["c->df = 1;"]

    def i_sahf(self, insn, op, live):
        return ["{ u32 ah = (c->eax >> 8) & 0xff;",
                "  c->cf = ah & 1; c->pf = (ah >> 2) & 1; c->af = (ah >> 4) & 1;",
                "  c->zf = (ah >> 6) & 1; c->sf = (ah >> 7) & 1; }"]

    def i_lahf(self, insn, op, live):
        return ["{ u32 ah = 2 | c->cf | (c->pf << 2) | (c->af << 4) | (c->zf << 6) | (c->sf << 7);",
                "  c->eax = (c->eax & 0xffff00ffu) | (ah << 8); }"]

    def i_pushfd(self, insn, op, live):
        return ["{ u32 fl = 2 | c->cf | (c->pf<<2) | (c->af<<4) | (c->zf<<6) | (c->sf<<7) | (c->df<<10) | (c->of<<11);",
                "  c->esp -= 4; Wr32(c->esp, fl); }"]

    def i_popfd(self, insn, op, live):
        return ["{ u32 fl = Rd32(c->esp); c->esp += 4;",
                "  c->cf=fl&1; c->pf=(fl>>2)&1; c->af=(fl>>4)&1; c->zf=(fl>>6)&1;",
                "  c->sf=(fl>>7)&1; c->df=(fl>>10)&1; c->of=(fl>>11)&1; }"]

    def i_cpuid(self, insn, op, live):
        return ["rg_cpuid(c);"]

    # ---- string ops ------------------------------------------------------
    def _rep_prefix(self, ins):
        if ins.has_rep_prefix or ins.has_repe_prefix:
            return "rep"
        if ins.has_repne_prefix:
            return "repne"
        return None

    def i_movsw(self, insn, op, live):
        return self._stringop(insn, 2, "movs")

    def i_movsb(self, insn, op, live):
        return self._stringop(insn, 1, "movs")

    def i_stosd(self, insn, op, live):
        return self._stringop(insn, 4, "stos")

    def i_stosw(self, insn, op, live):
        return self._stringop(insn, 2, "stos")

    def i_stosb(self, insn, op, live):
        return self._stringop(insn, 1, "stos")

    def i_lodsd(self, insn, op, live):
        return self._stringop(insn, 4, "lods")

    def i_lodsb(self, insn, op, live):
        return self._stringop(insn, 1, "lods")

    def i_cmpsb(self, insn, op, live):
        return self._stringop(insn, 1, "cmps", live)

    def i_cmpsw(self, insn, op, live):
        return self._stringop(insn, 2, "cmps", live)

    def i_cmpsd(self, insn, op, live):
        return self._stringop(insn, 4, "cmps", live)

    def i_scasb(self, insn, op, live):
        return self._stringop(insn, 1, "scas", live)

    def i_scasw(self, insn, op, live):
        return self._stringop(insn, 2, "scas", live)

    def i_scasd(self, insn, op, live):
        return self._stringop(insn, 4, "scas", live)

    def _stringop(self, insn, n, kind, live=frozenset()):
        ins = insn.ins
        rep = self._rep_prefix(ins)
        step = f"(c->df ? (u32)(-{n}) : {n}u)"
        bits = n * 8
        body = []
        if kind == "movs":
            body = [f"Wr{bits}(c->edi, Rd{bits}(c->esi));",
                    "c->esi += st; c->edi += st;"]
        elif kind == "stos":
            areg = {1: "(c->eax & 0xff)", 2: "(c->eax & 0xffff)", 4: "c->eax"}[n]
            body = [f"Wr{bits}(c->edi, {areg});", "c->edi += st;"]
        elif kind == "lods":
            body = [f"{{ u32 v = Rd{bits}(c->esi); c->esi += st;"]
            if n == 4:
                body.append("  c->eax = v; }")
            elif n == 2:
                body.append("  c->eax = (c->eax & 0xffff0000u) | (v & 0xffff); }")
            else:
                body.append("  c->eax = (c->eax & 0xffffff00u) | (v & 0xff); }")
        elif kind == "cmps":
            body = [f"{{ u32 x = Rd{bits}(c->esi), y = Rd{bits}(c->edi); c->esi += st; c->edi += st;",
                    f"  u32 r = x - y;"]
            for s in FL.emit("CMP", n, "x", "y", "r", FL.ALL):
                body.append("  " + s)
            body.append("}")
        elif kind == "scas":
            areg = {1: "(c->eax & 0xff)", 2: "(c->eax & 0xffff)", 4: "c->eax"}[n]
            body = [f"{{ u32 x = {areg}, y = Rd{bits}(c->edi); c->edi += st;",
                    f"  u32 r = x - y;"]
            for s in FL.emit("CMP", n, "x", "y", "r", FL.ALL):
                body.append("  " + s)
            body.append("}")

        inner = "\n    ".join(body)
        if rep is None:
            return [f"{{ u32 st = {step};", f"  {inner}", "}"]
        # repeated
        lines = [f"{{ u32 st = {step};", "  while (c->ecx) {", f"    {inner}", "    c->ecx--;"]
        if kind in ("cmps", "scas"):
            if rep == "rep":  # repe
                lines.append("    if (!c->zf) break;")
            else:  # repne
                lines.append("    if (c->zf) break;")
        lines.append("  } }")
        return lines

    # ---- x87 -------------------------------------------------------------
    # iced lists the implicit ST(0) of two-register forms such as
    # `fxch st(0),st(i)` and `fcom st(0),st(i)` as operand 0; the register the
    # instruction names is the last operand.
    def i_fld(self, insn, op, live):
        ins = insn.ins
        if ins.op0_kind == OK.MEMORY:
            ea = EO.mem_ea(ins)
            n = EO.mem_bytes(ins)
            if n == 4:
                return [f"rg_fpush(c, (double)rg_rdf32({ea}));"]
            if n == 8:
                return [f"rg_fpush(c, rg_rdf64({ea}));"]
            if n == 10:
                return [f"rg_fpush(c, rg_rdf80({ea}));"]
        # fld st(i)
        i = st_index(ins.op0_register)
        return [f"{{ double v = rg_st(c, {i}); rg_fpush(c, v); }}"]

    def i_fild(self, insn, op, live):
        ins = insn.ins
        ea = EO.mem_ea(ins)
        n = EO.mem_bytes(ins)
        if n == 2:
            return [f"rg_fpush(c, (double)(i16)Rd16({ea}));"]
        if n == 4:
            return [f"rg_fpush(c, (double)(i32)Rd32({ea}));"]
        return [f"rg_fpush(c, (double)(i64)Rd64({ea}));"]

    def _fstore(self, insn, pop):
        ins = insn.ins
        if ins.op0_kind == OK.MEMORY:
            ea = EO.mem_ea(ins)
            n = EO.mem_bytes(ins)
            st0 = "rg_st(c, 0)"
            if n == 4:
                s = [f"rg_wrf32({ea}, (float){st0});"]
            elif n == 8:
                s = [f"rg_wrf64({ea}, {st0});"]
            else:
                s = [f"rg_wrf80({ea}, {st0});"]
        else:
            i = st_index(ins.op0_register)
            s = [f"rg_setst(c, {i}, rg_st(c, 0));"]
        if pop:
            s.append("rg_fpop(c);")
        return s

    def i_fst(self, insn, op, live):
        return self._fstore(insn, False)

    def i_fstp(self, insn, op, live):
        return self._fstore(insn, True)

    def _fistore(self, insn, pop):
        ins = insn.ins
        ea = EO.mem_ea(ins)
        n = EO.mem_bytes(ins)
        if n == 2:
            s = [f"Wr16({ea}, (u16)(i16)rg_f2i(c, rg_st(c,0)));"]
        elif n == 4:
            s = [f"Wr32({ea}, (u32)(i32)rg_f2i(c, rg_st(c,0)));"]
        else:
            s = [f"Wr64({ea}, (u64)(i64)rg_f2ll(c, rg_st(c,0)));"]
        if pop:
            s.append("rg_fpop(c);")
        return s

    def i_fist(self, insn, op, live):
        return self._fistore(insn, False)

    def i_fistp(self, insn, op, live):
        return self._fistore(insn, True)

    def _farith(self, insn, oper, rev=False, pop=False, toint=False):
        ins = insn.ins
        # forms: Fop m32/m64 ; Fop st,st(i) ; Fop st(i),st ; FopP st(i),st
        if ins.op0_kind == OK.MEMORY:
            ea = EO.mem_ea(ins)
            n = EO.mem_bytes(ins)
            if toint:
                src = f"(double)(i32)Rd32({ea})" if n == 4 else f"(double)(i16)Rd16({ea})"
            else:
                src = f"(double)rg_rdf32({ea})" if n == 4 else f"rg_rdf64({ea})"
            a, b = "rg_st(c,0)", src
            lhs = 0
        else:
            # `fop st(0),st(i)` writes ST(0); `fop[p] st(i),st(0)` writes ST(i).
            r0, r1 = st_index(ins.op0_register), st_index(ins.op1_register)
            if r0 == 0:
                a, b = "rg_st(c,0)", f"rg_st(c,{r1})"
                lhs = 0
            else:
                a, b = f"rg_st(c,{r0})", "rg_st(c,0)"
                lhs = r0
        x, y = (b, a) if rev else (a, b)
        expr = {"+": f"{x} + {y}", "-": f"{x} - {y}", "*": f"{x} * {y}", "/": f"{x} / {y}"}[oper]
        s = [f"rg_setst(c, {lhs}, {expr});"]
        if pop:
            s.append("rg_fpop(c);")
        return s

    def i_fadd(self, insn, op, live):
        return self._farith(insn, "+")

    def i_fsub(self, insn, op, live):
        return self._farith(insn, "-")

    def i_fsubr(self, insn, op, live):
        return self._farith(insn, "-", rev=True)

    def i_fmul(self, insn, op, live):
        return self._farith(insn, "*")

    def i_fdiv(self, insn, op, live):
        return self._farith(insn, "/")

    def i_fdivr(self, insn, op, live):
        return self._farith(insn, "/", rev=True)

    def i_faddp(self, insn, op, live):
        return self._farith(insn, "+", pop=True)

    def i_fsubp(self, insn, op, live):
        return self._farith(insn, "-", pop=True)

    def i_fsubrp(self, insn, op, live):
        return self._farith(insn, "-", rev=True, pop=True)

    def i_fmulp(self, insn, op, live):
        return self._farith(insn, "*", pop=True)

    def i_fdivp(self, insn, op, live):
        return self._farith(insn, "/", pop=True)

    def i_fdivrp(self, insn, op, live):
        return self._farith(insn, "/", rev=True, pop=True)

    def i_fiadd(self, insn, op, live):
        return self._farith(insn, "+", toint=True)

    def i_fimul(self, insn, op, live):
        return self._farith(insn, "*", toint=True)

    def i_fisub(self, insn, op, live):
        return self._farith(insn, "-", toint=True)

    def i_fidiv(self, insn, op, live):
        return self._farith(insn, "/", toint=True)

    def i_fchs(self, insn, op, live):
        return ["rg_setst(c, 0, -rg_st(c,0));"]

    def i_fabs(self, insn, op, live):
        return ["rg_setst(c, 0, rg_fabs(rg_st(c,0)));"]

    def i_fsqrt(self, insn, op, live):
        return ["rg_setst(c, 0, rg_sqrt(rg_st(c,0)));"]

    def i_fsin(self, insn, op, live):
        return ["rg_fsin(c);"]

    def i_fcos(self, insn, op, live):
        return ["rg_fcos(c);"]

    def i_frndint(self, insn, op, live):
        return ["rg_setst(c, 0, rg_rint(c, rg_st(c,0)));"]

    def i_fprem1(self, insn, op, live):
        return ["rg_fprem1(c);"]

    def i_fprem(self, insn, op, live):
        return ["rg_fprem(c);"]

    def i_fldz(self, insn, op, live):
        return ["rg_fpush(c, 0.0);"]

    def i_fld1(self, insn, op, live):
        return ["rg_fpush(c, 1.0);"]

    def i_fldpi(self, insn, op, live):
        return ["rg_fpush(c, 3.14159265358979323846);"]

    def i_fldl2e(self, insn, op, live):
        return ["rg_fpush(c, 1.44269504088896340736);"]

    def i_fldln2(self, insn, op, live):
        return ["rg_fpush(c, 0.693147180559945309417);"]

    def i_fxch(self, insn, op, live):
        ins = insn.ins
        i = st_index(ins.op_register(ins.op_count - 1)) if ins.op_count else 1
        return [f"rg_fxch(c, {i});"]

    def i_fcom(self, insn, op, live):
        return self._fcom(insn, 0)

    def i_fcomp(self, insn, op, live):
        return self._fcom(insn, 1)

    def i_fcompp(self, insn, op, live):
        return self._fcom(insn, 2)

    def i_fucom(self, insn, op, live):
        return self._fcom(insn, 0)

    def i_fucomp(self, insn, op, live):
        return self._fcom(insn, 1)

    def i_fucompp(self, insn, op, live):
        return self._fcom(insn, 2)

    def _fcom(self, insn, pops):
        ins = insn.ins
        if ins.op_count >= 1 and ins.op0_kind == OK.MEMORY:
            ea = EO.mem_ea(ins)
            n = EO.mem_bytes(ins)
            other = f"(double)rg_rdf32({ea})" if n == 4 else f"rg_rdf64({ea})"
        elif ins.op_count >= 1:  # fcom st(0),st(i)
            other = f"rg_st(c, {st_index(ins.op_register(ins.op_count - 1))})"
        else:                    # fcompp / fucompp
            other = "rg_st(c, 1)"
        s = [f"rg_fcom(c, rg_st(c,0), {other});"]
        for _ in range(pops):
            s.append("rg_fpop(c);")
        return s

    def i_fnstsw(self, insn, op, live):
        ins = insn.ins
        if ins.op0_kind == OK.REGISTER:  # AX
            return ["c->eax = (c->eax & 0xffff0000u) | (rg_fsw(c) & 0xffff);"]
        ea = EO.mem_ea(ins)
        return [f"Wr16({ea}, (u16)rg_fsw(c));"]

    def i_fnstcw(self, insn, op, live):
        ea = EO.mem_ea(insn.ins)
        return [f"Wr16({ea}, (u16)c->fpu_cw);"]

    def i_fldcw(self, insn, op, live):
        ea = EO.mem_ea(insn.ins)
        return [f"c->fpu_cw = Rd16({ea});"]

    def i_ffree(self, insn, op, live):
        return []

    def i_fincstp(self, insn, op, live):
        return ["c->fpu_top = (c->fpu_top + 1) & 7;"]

    def i_fdecstp(self, insn, op, live):
        return ["c->fpu_top = (c->fpu_top - 1) & 7;"]

    # ---- SSE2 (only the forms used by the CRT double rounder) -----------
    def _xreg(self, ins, i):
        r = ins.op_register(i)
        if not is_xmm(r):
            raise ValueError(f"operand {i} is not an xmm register")
        return r - R.XMM0

    def i_movq(self, insn, op, live):
        ins = insn.ins
        if ins.op0_kind == OK.REGISTER and ins.op1_kind == OK.REGISTER:
            return [f"c->xmm[{self._xreg(ins,0)}].q[0] = c->xmm[{self._xreg(ins,1)}].q[0]; c->xmm[{self._xreg(ins,0)}].q[1] = 0;"]
        if ins.op0_kind == OK.REGISTER:  # movq xmm, m64
            return [f"c->xmm[{self._xreg(ins,0)}].q[0] = Rd64({EO.mem_ea(ins)}); c->xmm[{self._xreg(ins,0)}].q[1] = 0;"]
        return [f"Wr64({EO.mem_ea(ins)}, c->xmm[{self._xreg(ins,1)}].q[0]);"]  # movq m64, xmm

    def i_movapd(self, insn, op, live):
        ins = insn.ins
        if ins.op0_kind == OK.REGISTER and ins.op1_kind == OK.REGISTER:
            d, s = self._xreg(ins, 0), self._xreg(ins, 1)
            return [f"c->xmm[{d}] = c->xmm[{s}];"]
        if ins.op0_kind == OK.REGISTER:
            d = self._xreg(ins, 0)
            ea = EO.mem_ea(ins)
            return [f"c->xmm[{d}].q[0] = Rd64({ea}); c->xmm[{d}].q[1] = Rd64(({ea}) + 8);"]
        s = self._xreg(ins, 1)
        ea = EO.mem_ea(ins)
        return [f"Wr64({ea}, c->xmm[{s}].q[0]); Wr64(({ea}) + 8, c->xmm[{s}].q[1]);"]

    def i_movsd(self, insn, op, live):
        ins = insn.ins
        # Disambiguate SSE movsd (xmm) from string movsd.
        if ins.code == Code.MOVSD_M32_M32:
            return self._stringop(insn, 4, "movs")
        if ins.op0_kind == OK.REGISTER and ins.op1_kind == OK.REGISTER:
            return [f"c->xmm[{self._xreg(ins,0)}].q[0] = c->xmm[{self._xreg(ins,1)}].q[0];"]
        if ins.op0_kind == OK.REGISTER:
            return [f"c->xmm[{self._xreg(ins,0)}].q[0] = Rd64({EO.mem_ea(ins)}); c->xmm[{self._xreg(ins,0)}].q[1]=0;"]
        return [f"Wr64({EO.mem_ea(ins)}, c->xmm[{self._xreg(ins,1)}].q[0]);"]

    def _xmm_src_q(self, ins, idx, lane):
        if ins.op_kind(idx) == OK.REGISTER:
            return f"c->xmm[{self._xreg(ins,idx)}].q[{lane}]"
        ea = EO.mem_ea(ins)
        return f"Rd64(({ea}) + {lane*8})"

    def i_psrlq(self, insn, op, live):
        ins = insn.ins
        d = self._xreg(ins, 0)
        if ins.op1_kind == OK.IMMEDIATE8:
            sh = ins.immediate(1) & 0xFF
            if sh > 63:
                return [f"c->xmm[{d}].q[0] = 0; c->xmm[{d}].q[1] = 0;"]
            return [f"c->xmm[{d}].q[0] >>= {sh}; c->xmm[{d}].q[1] >>= {sh};"]
        sh = self._xmm_src_q(ins, 1, 0)
        return [f"{{ u64 sh = {sh}; if (sh > 63) {{ c->xmm[{d}].q[0]=0; c->xmm[{d}].q[1]=0; }} else {{ c->xmm[{d}].q[0] >>= sh; c->xmm[{d}].q[1] >>= sh; }} }}"]

    def i_psllq(self, insn, op, live):
        ins = insn.ins
        d = self._xreg(ins, 0)
        if ins.op1_kind == OK.IMMEDIATE8:
            sh = ins.immediate(1) & 0xFF
            if sh > 63:
                return [f"c->xmm[{d}].q[0] = 0; c->xmm[{d}].q[1] = 0;"]
            return [f"c->xmm[{d}].q[0] <<= {sh}; c->xmm[{d}].q[1] <<= {sh};"]
        sh = self._xmm_src_q(ins, 1, 0)
        return [f"{{ u64 sh = {sh}; if (sh > 63) {{ c->xmm[{d}].q[0]=0; c->xmm[{d}].q[1]=0; }} else {{ c->xmm[{d}].q[0] <<= sh; c->xmm[{d}].q[1] <<= sh; }} }}"]

    def i_psubd(self, insn, op, live):
        ins = insn.ins
        d = self._xreg(ins, 0)
        out = []
        for k in range(4):
            if ins.op1_kind == OK.REGISTER:
                s = f"c->xmm[{self._xreg(ins,1)}].d[{k}]"
            else:
                s = f"Rd32(({EO.mem_ea(ins)}) + {k*4})"
            out.append(f"c->xmm[{d}].d[{k}] -= {s};")
        return out

    def i_andpd(self, insn, op, live):
        ins = insn.ins
        d = self._xreg(ins, 0)
        return [f"c->xmm[{d}].q[0] &= {self._xmm_src_q(ins,1,0)}; c->xmm[{d}].q[1] &= {self._xmm_src_q(ins,1,1)};"]

    def i_addsd(self, insn, op, live):
        ins = insn.ins
        d = self._xreg(ins, 0)
        if ins.op1_kind == OK.REGISTER:
            s = f"c->xmm[{self._xreg(ins,1)}].f[0]"
        else:
            s = f"rg_rdf64({EO.mem_ea(ins)})"
        return [f"c->xmm[{d}].f[0] += {s};"]

    def i_cmppd(self, insn, op, live):
        ins = insn.ins
        d = self._xreg(ins, 0)
        imm = ins.immediate(2) & 0xFF
        out = ["{ double b0, b1;"]
        if ins.op1_kind == OK.REGISTER:
            s = self._xreg(ins, 1)
            out.append(f"  b0 = c->xmm[{s}].f[0]; b1 = c->xmm[{s}].f[1];")
        else:
            ea = EO.mem_ea(ins)
            out.append(f"  b0 = rg_rdf64({ea}); b1 = rg_rdf64(({ea})+8);")
        pred0, pred1 = self._cmppd_pred(imm, f"c->xmm[{d}].f[0]", "b0"), self._cmppd_pred(imm, f"c->xmm[{d}].f[1]", "b1")
        out.append(f"  c->xmm[{d}].q[0] = ({pred0}) ? 0xffffffffffffffffull : 0;")
        out.append(f"  c->xmm[{d}].q[1] = ({pred1}) ? 0xffffffffffffffffull : 0; }}")
        return out

    def _cmppd_pred(self, imm, a, b):
        # 0 EQ,1 LT,2 LE,3 UNORD,4 NEQ,5 NLT,6 NLE,7 ORD
        return {
            0: f"{a} == {b}", 1: f"{a} < {b}", 2: f"{a} <= {b}",
            3: f"(rg_isnan({a}) || rg_isnan({b}))", 4: f"{a} != {b}",
            5: f"!({a} < {b})", 6: f"!({a} <= {b})",
            7: f"(!rg_isnan({a}) && !rg_isnan({b}))",
        }[imm]

    def i_ucomisd(self, insn, op, live):
        ins = insn.ins
        a = f"c->xmm[{self._xreg(ins,0)}].f[0]"
        if ins.op1_kind == OK.REGISTER:
            b = f"c->xmm[{self._xreg(ins,1)}].f[0]"
        else:
            b = f"rg_rdf64({EO.mem_ea(ins)})"
        return [f"rg_ucomisd(c, {a}, {b});"]

    def i_movd(self, insn, op, live):
        ins = insn.ins
        if not (ins.op0_kind == OK.REGISTER and is_xmm(ins.op0_register)):
            # movd r/m32, xmm
            return [op.wr(0, f"c->xmm[{self._xreg(ins,1)}].d[0]")]
        # movd xmm, r/m32 (zero-extends into the whole register)
        d = self._xreg(ins, 0)
        return [f"c->xmm[{d}].d[0] = {op.rd(1)}; c->xmm[{d}].d[1] = 0; c->xmm[{d}].q[1] = 0;"]

    def i_stmxcsr(self, insn, op, live):
        return [f"Wr32({EO.mem_ea(insn.ins)}, 0x1f80u);"]

    def i_ldmxcsr(self, insn, op, live):
        return []
