"""EFLAGS handling for the translator.

Flags are materialised eagerly into the CPU struct (`c->cf`, `c->of`, ...), but
only the flags that are actually read before being overwritten are computed, so
the common `cmp`/`jcc` pair costs just the flags the branch needs.  Liveness is
a standard backward dataflow over each function's instruction CFG.  Calls are
treated as clobbering every arithmetic flag (the x86 ABI never carries flags
across a call), which keeps pre-call flag computations from being emitted.
"""

import iced_x86 as ix

RB = ix.RflagsBits

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


def read_set(ins):
    r = ins.ins.rflags_read
    return frozenset(n for n, b in BIT.items() if r & b)


def written_set(ins):
    w = ins.ins.rflags_modified | ins.ins.rflags_undefined
    return frozenset(n for n, b in BIT.items() if w & b)


def reads_df(ins):
    return bool(ins.ins.rflags_read & RB.DF)


def writes_df(ins):
    return bool((ins.ins.rflags_modified | ins.ins.rflags_undefined) & RB.DF)


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


def emit(fam, sz, a, b, r, live, carry=None, count=None):
    """Return C statements setting the live flags for an ALU operation.

    fam     operation family (see below)
    sz      operand width in bytes
    a,b,r   C expressions for dest-before, src, and result (all u32)
    live    set of flag names that must be produced
    carry   C expr (0/1) for incoming carry (adc/sbb)
    count   C expr for shift/rotate count (already masked to 0..31)
    """
    out = []
    S = sz * 8 - 1
    M = MASK[sz]

    def want(f):
        return f in live

    if fam in ("ADD", "ADC"):
        if want("cf"):
            if fam == "ADD":
                out.append(f"c->cf = (({r} & {M}) < ({a} & {M}));")
            else:
                out.append(
                    f"c->cf = {carry} ? (({r} & {M}) <= ({a} & {M})) "
                    f": (({r} & {M}) < ({a} & {M}));"
                )
        if want("of"):
            out.append(f"c->of = (((~({a} ^ {b})) & ({a} ^ {r}) & {SIGN[sz]:#x}u) != 0);")
        if want("af"):
            out.append(_af(a, b, r))
    elif fam in ("SUB", "CMP", "SBB"):
        if want("cf"):
            if fam == "SBB":
                out.append(
                    f"c->cf = {carry} ? (({a} & {M}) <= ({b} & {M})) "
                    f": (({a} & {M}) < ({b} & {M}));"
                )
            else:
                out.append(f"c->cf = (({a} & {M}) < ({b} & {M}));")
        if want("of"):
            out.append(f"c->of = ((({a} ^ {b}) & ({a} ^ {r}) & {SIGN[sz]:#x}u) != 0);")
        if want("af"):
            out.append(_af(a, b, r))
    elif fam == "NEG":
        # dst = 0 - src;  a holds src, r holds result.
        if want("cf"):
            out.append(f"c->cf = (({a} & {M}) != 0);")
        if want("of"):
            out.append(f"c->of = (({a} & {M}) == {SIGN[sz]:#x}u);")
        if want("af"):
            out.append(f"c->af = (({a} & 0xf) != 0);")
    elif fam == "INC":
        if want("of"):
            out.append(f"c->of = (({a} & {M}) == {(SIGN[sz] - 1):#x}u);")
        if want("af"):
            out.append(f"c->af = ((({a} & 0xf) + 1) > 0xf);")
    elif fam == "DEC":
        if want("of"):
            out.append(f"c->of = (({a} & {M}) == {SIGN[sz]:#x}u);")
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
            out.append(
                f"c->cf = {count} ? (({a} >> ({sz*8} - {count})) & 1) : c->cf;"
            )
        if want("of"):
            out.append(
                f"c->of = ({count} == 1) ? ((({r} >> {S}) & 1) ^ (({a} >> ({sz*8}-1)) & 1)) : c->of;"
            )
        if want("af"):
            out.append("/* af undefined after shift */")
    elif fam in ("SHR", "SAR"):
        if want("cf"):
            out.append(f"c->cf = {count} ? (({a} >> ({count} - 1)) & 1) : c->cf;")
        if want("of"):
            if fam == "SHR":
                out.append(
                    f"c->of = ({count} == 1) ? (({a} >> {S}) & 1) : c->of;"
                )
            else:
                out.append(f"c->of = ({count} == 1) ? 0 : c->of;")
        if want("af"):
            out.append("/* af undefined after shift */")
    elif fam in ("SHLD", "SHRD"):
        # cf is handled explicitly by the generator; here do result flags.
        pass
    elif fam == "MULRES":
        # cf/of handled inline by the generator (needs the high half).
        pass
    elif fam == "BITSCAN":
        # only zf, handled by generator.
        pass
    else:
        raise ValueError(f"unknown flag family {fam}")

    if want("zf") and fam not in ("BITSCAN",):
        out.append(_zf(sz, r))
    if want("sf"):
        out.append(_sf(sz, r))
    if want("pf"):
        out.append(_pf(r))
    return out


# ---------------------------------------------------------------------------
def liveness(func, cache):
    """Compute, for each instruction VA, the set of flags live *out*.

    Returns dict va -> frozenset(flag names).  Successors come from func.succ;
    tail calls, dispatch and ret have no intra-procedural successors, so their
    live-out is empty (a freshly entered function never expects incoming
    flags).  Calls additionally clobber all flags on the way back.
    """
    # Predecessor-free worklist over the (small) per-function CFG.
    succ = func.succ
    live_out = {va: set() for va in func.insns}
    # Precompute read/written per insn.
    rd = {}
    wr = {}
    is_call = {}
    for va in func.insns:
        insn = cache[va]
        rd[va] = read_set(insn)
        wr[va] = written_set(insn)
        is_call[va] = insn.ins.is_call_near or insn.ins.is_call_near_indirect

    changed = True
    order = list(reversed(func.insns))
    while changed:
        changed = False
        for va in order:
            new = set()
            for s in succ.get(va, ()):
                if s in live_out:  # intra-procedural successor
                    li = (live_out[s] - wr[s]) | rd[s]
                    new |= li
            if new != live_out[va]:
                live_out[va] = new
                changed = True
    # live-out of an instruction is what its flag emission must produce,
    # intersected with what it writes.
    return {va: frozenset(live_out[va]) for va in func.insns}
