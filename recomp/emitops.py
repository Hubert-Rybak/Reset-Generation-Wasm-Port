"""Operand -> C expression helpers for the x86 -> C translator.

The generated code operates on a `CPU *c` and a flat guest memory array.  Guest
registers live in the `CPU` struct; sub-register reads and writes preserve the
surrounding bits exactly as the hardware does.
"""

import iced_x86 as ix

R = ix.Register
OK = ix.OpKind

# Full 32-bit register -> CPU struct field.
FIELD = {
    R.EAX: "eax", R.ECX: "ecx", R.EDX: "edx", R.EBX: "ebx",
    R.ESP: "esp", R.EBP: "ebp", R.ESI: "esi", R.EDI: "edi",
}
# 16-bit -> parent 32-bit register.
PARENT16 = {
    R.AX: R.EAX, R.CX: R.ECX, R.DX: R.EDX, R.BX: R.EBX,
    R.SP: R.ESP, R.BP: R.EBP, R.SI: R.ESI, R.DI: R.EDI,
}
# 8-bit low -> parent.
PARENT8L = {R.AL: R.EAX, R.CL: R.ECX, R.DL: R.EDX, R.BL: R.EBX}
# 8-bit high -> parent.
PARENT8H = {R.AH: R.EAX, R.CH: R.ECX, R.DH: R.EDX, R.BH: R.EBX}


def reg_size(r):
    if r in FIELD:
        return 4
    if r in PARENT16:
        return 2
    if r in PARENT8L or r in PARENT8H:
        return 1
    raise KeyError(f"unsupported register {r}")


def reg_read(r):
    if r in FIELD:
        return f"c->{FIELD[r]}"
    if r in PARENT16:
        return f"(c->{FIELD[PARENT16[r]]} & 0xffffu)"
    if r in PARENT8L:
        return f"(c->{FIELD[PARENT8L[r]]} & 0xffu)"
    if r in PARENT8H:
        return f"((c->{FIELD[PARENT8H[r]]} >> 8) & 0xffu)"
    raise KeyError(f"unsupported register read {r}")


def reg_write(r, val):
    """Return a C statement assigning `val` (a C expression) to register r."""
    if r in FIELD:
        return f"c->{FIELD[r]} = (u32)({val});"
    if r in PARENT16:
        f = FIELD[PARENT16[r]]
        return f"c->{f} = (c->{f} & 0xffff0000u) | ((u32)({val}) & 0xffffu);"
    if r in PARENT8L:
        f = FIELD[PARENT8L[r]]
        return f"c->{f} = (c->{f} & 0xffffff00u) | ((u32)({val}) & 0xffu);"
    if r in PARENT8H:
        f = FIELD[PARENT8H[r]]
        return f"c->{f} = (c->{f} & 0xffff00ffu) | (((u32)({val}) & 0xffu) << 8);"
    raise KeyError(f"unsupported register write {r}")


# ---------------------------------------------------------------------------
# Memory effective address.
FS_BASE = "RG_FS_BASE"  # defined in the runtime header


def mem_ea(ins):
    """C expression (u32) for the effective address of a memory operand."""
    parts = []
    base = ins.memory_base
    index = ins.memory_index
    if base != R.NONE:
        parts.append(reg_read(base))
    if index != R.NONE:
        scale = ins.memory_index_scale
        ir = reg_read(index)
        parts.append(f"{ir}*{scale}" if scale != 1 else ir)
    disp = ins.memory_displacement & 0xFFFFFFFF
    if disp or not parts:
        parts.append(f"0x{disp:x}u")
    expr = " + ".join(parts)
    if ins.memory_segment == R.FS:
        expr = f"{FS_BASE} + ({expr})"
    return f"(u32)({expr})"


# Memory access width (bytes) from the instruction's memory operand.
def mem_bytes(ins):
    sz = ix.MemorySizeExt.size(ins.memory_size)
    return sz


# ---------------------------------------------------------------------------
def imm_val(ins, i):
    """32-bit operand value of an immediate operand (already extended)."""
    return ins.immediate(i) & 0xFFFFFFFF


def op_size(ins, i):
    """Byte width of operand i."""
    k = ins.op_kind(i)
    if k == OK.REGISTER:
        return reg_size(ins.op_register(i))
    if k == OK.MEMORY:
        return mem_bytes(ins)
    # immediate / branch: fall back to the instruction's first register/mem.
    for j in range(ins.op_count):
        kj = ins.op_kind(j)
        if kj == OK.REGISTER:
            return reg_size(ins.op_register(j))
        if kj == OK.MEMORY:
            return mem_bytes(ins)
    return 4


class Operands:
    """Reads/writes for the operands of one instruction, within a function.

    `load_mem`/`store_mem` are filled in by the code generator so that a memory
    operand is computed once (its EA captured in a temp) and reused for a
    read-modify-write.
    """

    def __init__(self, gen, ins):
        self.gen = gen
        self.ins = ins

    def size(self, i):
        return op_size(self.ins, i)

    def rd(self, i):
        ins = self.ins
        k = ins.op_kind(i)
        if k == OK.REGISTER:
            return reg_read(ins.op_register(i))
        if k == OK.MEMORY:
            n = mem_bytes(ins)
            return self.gen.mem_read(ins, n)
        # immediate
        return f"0x{imm_val(ins, i):x}u"

    def wr(self, i, val):
        ins = self.ins
        k = ins.op_kind(i)
        if k == OK.REGISTER:
            return reg_write(ins.op_register(i), val)
        if k == OK.MEMORY:
            n = mem_bytes(ins)
            return self.gen.mem_write(ins, n, val)
        raise ValueError("cannot write immediate operand")
