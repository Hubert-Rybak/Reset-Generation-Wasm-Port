"""EFLAGS handling for the translator.

Flags are materialised eagerly into the CPU struct (`c->cf`, `c->of`, ...), but
only the flags that are actually read before being overwritten are computed, so
the common `cmp`/`jcc` pair costs just the flags the branch needs.

Liveness is a backward dataflow over each function's instruction CFG, made
interprocedural so that it stays exact for hand-written assembly:

  * a direct call kills every flag except those the callee reads on entry;
  * a callee's `ret` keeps alive whatever flags any caller reads after the
    call returns (some CRT helpers return a result in ZF);
  * a tail jump into another function passes on that function's entry
    liveness, and shares the caller's return liveness.

Indirect calls and imports are assumed to follow the ABI (flags dead).
"""

import iced_x86 as ix

RB = ix.RflagsBits
M = ix.Mnemonic
OK = ix.OpKind
R = ix.Register

# Our tracked arithmetic flags.
BIT = {
    "cf": RB.CF,
    "pf": RB.PF,
    "af": RB.AF,
    "zf": RB.ZF,
    "sf": RB.SF,
    "of": RB.OF,
}
ALL = frozenset(BIT)
NONE = frozenset()

# Shifts and rotates leave every flag untouched when the masked count is zero.
COUNTED = {M.SHL, M.SAL, M.SHR, M.SAR, M.ROL, M.ROR, M.RCL, M.RCR, M.SHLD, M.SHRD}


def _flags(mask):
    return frozenset(n for n, b in BIT.items() if mask & b)


def read_set(insn):
    return _flags(insn.ins.rflags_read)


def shift_count(ins):
    """Masked immediate count of a shift/rotate, or None if it comes from CL."""
    k = ins.op_kind(ins.op_count - 1)
    if k == OK.REGISTER:
        return None
    return ins.immediate(ins.op_count - 1) & 31


def written_set(insn):
    """Flags the instruction may write (what its emitter has to produce)."""
    return _flags(insn.ins.rflags_modified | insn.ins.rflags_undefined)


def kill_set(insn):
    """Flags the instruction writes on every execution (liveness kill set)."""
    ins = insn.ins
    if ins.mnemonic in COUNTED and ins.op_count >= 2 and shift_count(ins) in (None, 0):
        return NONE
    return written_set(insn)


MASK = {1: "0xffu", 2: "0xffffu", 4: "0xffffffffu"}
SIGN = {1: 0x80, 2: 0x8000, 4: 0x80000000}


def _zf(sz, r):
    return f"c->zf = (({r} & {MASK[sz]}) == 0);"


def _sf(sz, r):
    return f"c->sf = (({r} >> {sz * 8 - 1}) & 1);"


def _pf(r):
    return f"c->pf = rg_parity({r});"


def _af(a, b, r):
    return f"c->af = ((({a} ^ {b} ^ {r}) >> 4) & 1);"


def emit(fam, sz, a, b, r, live, carry=None, count=None, sa=None):
    """Return C statements setting the live flags for an ALU operation.

    fam     operation family (see below)
    sz      operand width in bytes
    a,b,r   C expressions for dest-before, src, and result (all u32; `a`
            masked to the operand width)
    live    set of flag names that must be produced
    carry   C expr (0/1) for incoming carry (adc/sbb)
    count   C expr for a shift count; the caller guarantees 1 <= count <= 31
    sa      C expr for `a` sign-extended to 32 bits (SAR)
    """
    out = []
    S = sz * 8 - 1
    M_ = MASK[sz]

    def want(f):
        return f in live

    if fam in ("ADD", "ADC"):
        if want("cf"):
            if fam == "ADD":
                out.append(f"c->cf = (({r} & {M_}) < ({a} & {M_}));")
            else:
                out.append(
                    f"c->cf = {carry} ? (({r} & {M_}) <= ({a} & {M_})) "
                    f": (({r} & {M_}) < ({a} & {M_}));"
                )
        if want("of"):
            out.append(f"c->of = (((~({a} ^ {b})) & ({a} ^ {r}) & {SIGN[sz]:#x}u) != 0);")
        if want("af"):
            out.append(_af(a, b, r))
    elif fam in ("SUB", "CMP", "SBB"):
        if want("cf"):
            if fam == "SBB":
                out.append(
                    f"c->cf = {carry} ? (({a} & {M_}) <= ({b} & {M_})) "
                    f": (({a} & {M_}) < ({b} & {M_}));"
                )
            else:
                out.append(f"c->cf = (({a} & {M_}) < ({b} & {M_}));")
        if want("of"):
            out.append(f"c->of = ((({a} ^ {b}) & ({a} ^ {r}) & {SIGN[sz]:#x}u) != 0);")
        if want("af"):
            out.append(_af(a, b, r))
    elif fam == "NEG":
        # dst = 0 - src;  a holds src, r holds result.
        if want("cf"):
            out.append(f"c->cf = (({a} & {M_}) != 0);")
        if want("of"):
            out.append(f"c->of = (({a} & {M_}) == {SIGN[sz]:#x}u);")
        if want("af"):
            out.append(f"c->af = (({a} & 0xf) != 0);")
    elif fam == "INC":
        if want("of"):
            out.append(f"c->of = (({a} & {M_}) == {(SIGN[sz] - 1):#x}u);")
        if want("af"):
            out.append(f"c->af = (({a} & 0xf) == 0xf);")
    elif fam == "DEC":
        if want("of"):
            out.append(f"c->of = (({a} & {M_}) == {SIGN[sz]:#x}u);")
        if want("af"):
            out.append(f"c->af = (({a} & 0xf) == 0);")
    elif fam == "LOGIC":
        if want("cf"):
            out.append("c->cf = 0;")
        if want("of"):
            out.append("c->of = 0;")
        if want("af"):
            out.append("c->af = 0;")
    elif fam == "SHL":
        if want("cf"):
            # Last bit shifted out; counts past the width give 0 (undefined).
            if sz == 4:
                out.append(f"c->cf = ({a} >> (32 - {count})) & 1;")
            else:
                out.append(f"c->cf = (({a} << {count}) >> {sz * 8}) & 1;")
        if want("of"):
            out.append(f"if ({count} == 1) c->of = (({r} >> {S}) ^ ({a} >> {S})) & 1;")
    elif fam == "SHR":
        if want("cf"):
            out.append(f"c->cf = ({a} >> ({count} - 1)) & 1;")
        if want("of"):
            out.append(f"if ({count} == 1) c->of = ({a} >> {S}) & 1;")
    elif fam == "SAR":
        if want("cf"):
            out.append(f"c->cf = (u32)({sa} >> ({count} - 1)) & 1;")
        if want("of"):
            out.append(f"if ({count} == 1) c->of = 0;")
    else:
        raise ValueError(f"unknown flag family {fam}")
    # AF is undefined after shifts; leaving it untouched is a valid result.

    if want("zf"):
        out.append(_zf(sz, r))
    if want("sf"):
        out.append(_sf(sz, r))
    if want("pf"):
        out.append(_pf(r))
    return out


# ---------------------------------------------------------------------------
def _is_call(ins):
    return ins.is_call_near or ins.is_call_near_indirect


class FlagAnalysis:
    """Interprocedural flag liveness over every discovered function."""

    def __init__(self, an):
        self.an = an
        self.funcs = an.funcs
        self.cache = an.cache
        self.ret_live = {e: NONE for e in self.funcs}   # flags callers read after return
        self.entry_live = {e: NONE for e in self.funcs}  # flags a function reads on entry
        self.live = {}                                   # entry -> {va: live-out}

    def _foreign(self, f):
        """Per-instruction foreign targets: (direct callee, tail-call targets)."""
        callee = {}
        tails = {}
        for va in f.insns:
            ins = self.cache[va].ins
            if ins.is_call_near and ins.near_branch_target in self.funcs:
                callee[va] = ins.near_branch_target
        for va, tgt in f.tailcalls.items():
            if tgt in self.funcs:
                tails.setdefault(va, []).append(tgt)
        for va, tbl in f.tables.items():
            for t in tbl:
                if t not in f.insns_set and t in self.funcs:
                    tails.setdefault(va, []).append(t)
        return callee, tails

    def _solve(self, f, callee, tails):
        cache = self.cache
        rd, kill, rets = {}, {}, set()
        for va in f.insns:
            insn = cache[va]
            rd[va] = read_set(insn)
            kill[va] = kill_set(insn)
            if insn.flow == ix.FlowControl.RETURN:
                rets.add(va)
        ret_live = self.ret_live[f.entry]

        def live_in(va, out):
            ins = cache[va].ins
            if _is_call(ins):
                # The callee decides what it reads; everything else is clobbered.
                t = callee.get(va)
                return self.entry_live[t] if t is not None else NONE
            return (out - kill[va]) | rd[va]

        live_out = {va: set() for va in f.insns}
        order = list(reversed(f.insns))
        changed = True
        while changed:
            changed = False
            for va in order:
                new = set()
                for s in f.succ.get(va, ()):
                    if s in live_out:
                        new |= live_in(s, live_out[s])
                for t in tails.get(va, ()):
                    new |= self.entry_live[t]
                if va in rets:
                    new |= ret_live
                if new != live_out[va]:
                    live_out[va] = new
                    changed = True
        out = {va: frozenset(s) for va, s in live_out.items()}
        entry_in = frozenset(live_in(f.entry, out[f.entry])) if f.entry in out else NONE
        after_calls = {}
        for va, t in callee.items():
            after = out[va]
            if after:
                after_calls[t] = after_calls.get(t, NONE) | after
        return out, entry_in, after_calls

    def run(self, log=print):
        prep = {}
        for e, f in self.funcs.items():
            f.insns_set = set(f.insns)
            prep[e] = self._foreign(f)
        callers = {}  # function -> functions that call or tail-jump into it
        for e, (callee, tails) in prep.items():
            for t in callee.values():
                callers.setdefault(t, set()).add(e)
            for t in self._tail_targets(tails):
                callers.setdefault(t, set()).add(e)

        work = set(self.funcs)
        rounds = 0
        while work:
            rounds += 1
            nxt = set()
            for e in sorted(work):
                f = self.funcs[e]
                out, entry_in, after_calls = self._solve(f, *prep[e])
                self.live[e] = out
                if entry_in != self.entry_live[e]:
                    self.entry_live[e] = entry_in
                    nxt |= callers.get(e, set())
                for t, flags_ in after_calls.items():
                    if not flags_ <= self.ret_live[t]:
                        self.ret_live[t] = self.ret_live[t] | flags_
                        nxt.add(t)
            # A tail-called function returns to its tail-caller's caller.
            for e in list(self.funcs):
                rl = self.ret_live[e]
                if rl:
                    for t in self._tail_targets(prep[e][1]):
                        if not rl <= self.ret_live[t]:
                            self.ret_live[t] = self.ret_live[t] | rl
                            nxt.add(t)
            work = nxt
        flagged = sorted(e for e, s in self.ret_live.items() if s)
        log(f"flag liveness converged in {rounds} rounds; "
            f"{len(flagged)} functions return flags: "
            + ", ".join(f"{e:#x}:{'/'.join(sorted(self.ret_live[e]))}" for e in flagged))
        return self.live

    @staticmethod
    def _tail_targets(tails):
        for tgts in tails.values():
            yield from tgts
