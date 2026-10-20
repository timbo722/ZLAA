"""
arm64.py - decode one ARM64 (A64) instruction and translate it into C.

Every A64 instruction is exactly 32 bits. The top-level split uses bits 28:25
(see the Arm Architecture Reference Manual, "A64 instruction set encoding"):

    100x  data processing, immediate   (ADD #imm, MOV, AND #imm, UBFX, ...)
    101x  branches, exceptions, system (B, BL, CBZ, RET, SVC, NOP, ...)
    x1x0  loads and stores             (LDR, STR, LDP, STP, ...)
    x101  data processing, register    (ADD reg, CSEL, MUL, UDIV, ...)
    x111  SIMD and floating point      (not supported yet)

The generated C uses the CPU struct from runtime/recomp_runtime.h:
    c->x[0..30]  general registers X0-X30   (Wn is the low 32 bits of Xn)
    c->sp        stack pointer
    c->n/z/c/v   condition flags
Register number 31 means either SP or the zero register (XZR/WZR),
depending on the instruction; each translator below picks the right one.
"""

MASK64 = (1 << 64) - 1


class Unsupported(Exception):
    """Raised for instructions the translator doesn't handle (yet)."""


def bits(v, hi, lo):
    return (v >> lo) & ((1 << (hi - lo + 1)) - 1)


def bit(v, n):
    return (v >> n) & 1


def sx(v, nbits):
    """Sign-extend an nbits-wide Python int."""
    return v - (1 << nbits) if v & (1 << (nbits - 1)) else v


def mask(width):
    return (1 << width) - 1


def hx(v):
    """C literal for a 64-bit constant."""
    return f"0x{v & MASK64:x}ull"


# Condition codes (bits 3:0 of B.cond, CSEL, CCMP...) as C expressions.
COND = [
    "c->z",                       # EQ
    "!c->z",                      # NE
    "c->c",                       # CS/HS
    "!c->c",                      # CC/LO
    "c->n",                       # MI
    "!c->n",                      # PL
    "c->v",                       # VS
    "!c->v",                      # VC
    "(c->c && !c->z)",            # HI
    "!(c->c && !c->z)",           # LS
    "(c->n == c->v)",             # GE
    "(c->n != c->v)",             # LT
    "(!c->z && c->n == c->v)",    # GT
    "!(!c->z && c->n == c->v)",   # LE
    "1",                          # AL
    "1",                          # NV (behaves as AL)
]


# --------------------------------------------------------------------------
# Register access helpers. These return C expressions / statements.
# --------------------------------------------------------------------------

def R(n, sf, sp=False):
    """Read register n. sf=1: 64-bit X register, sf=0: 32-bit W register.
    sp=True means register 31 is SP; otherwise 31 is the zero register."""
    if n == 31 and not sp:
        return "0ull" if sf else "(uint32_t)0"
    base = "c->sp" if n == 31 else f"c->x[{n}]"
    return base if sf else f"(uint32_t){base}"


def W(n, sf, expr, sp=False):
    """Write expr to register n. 32-bit writes zero the upper 32 bits,
    exactly like real hardware. Writes to the zero register are discarded
    (but the expression is still evaluated, because it may set flags)."""
    if n == 31 and not sp:
        return f"(void)({expr});"
    dst = "c->sp" if n == 31 else f"c->x[{n}]"
    if sf:
        return f"{dst} = {expr};"
    return f"{dst} = (uint32_t)({expr});"


def add_flags(sf, a, b, cin):
    return f"add{64 if sf else 32}_flags(c, {a}, {b}, {cin})"


def decode_bit_masks(n, imms, immr, datasize):
    """The 'bitmask immediate' used by AND/ORR/EOR/ANDS #imm.
    A run of (S+1) ones, rotated right by R inside an element of 2..64 bits,
    then repeated to fill the register. (ARM ARM: DecodeBitMasks)"""
    combined = (n << 6) | (~imms & 0x3F)
    length = combined.bit_length() - 1
    if length < 1:
        raise Unsupported("reserved bitmask immediate")
    levels = mask(length)
    s = imms & levels
    r = immr & levels
    if s == levels:
        raise Unsupported("reserved bitmask immediate")
    esize = 1 << length
    welem = mask(s + 1)
    elem = ((welem >> r) | (welem << (esize - r))) & mask(esize) if r else welem
    result = 0
    for i in range(datasize // esize):
        result |= elem << (i * esize)
    return result


from simd import SimdMixin  # noqa: E402  (needs the helpers above)


# --------------------------------------------------------------------------
# Branch analysis (used to know which addresses need C labels)
# --------------------------------------------------------------------------

def branch_info(insn, pc):
    """Return (kind, target) for branches, else None.
    kind is one of 'b', 'bl', 'cond' (conditional, falls through), 'br', 'blr', 'ret'."""
    if bits(insn, 30, 26) == 0b00101:
        target = (pc + sx(bits(insn, 25, 0), 26) * 4) & MASK64
        return ("bl" if bit(insn, 31) else "b", target)
    if bits(insn, 30, 25) in (0b011010, 0b011011):        # CBZ/CBNZ, TBZ/TBNZ
        if bits(insn, 30, 25) == 0b011010:
            off = sx(bits(insn, 23, 5), 19)
        else:
            off = sx(bits(insn, 18, 5), 14)
        return ("cond", (pc + off * 4) & MASK64)
    if bits(insn, 31, 24) == 0b01010100 and bit(insn, 4) == 0:  # B.cond
        return ("cond", (pc + sx(bits(insn, 23, 5), 19) * 4) & MASK64)
    if (bits(insn, 31, 25) == 0b1101011 and bits(insn, 20, 16) == 0x1F
            and bits(insn, 15, 10) == 0 and bits(insn, 4, 0) == 0):   # BR/BLR/RET (non-PAC)
        opc = bits(insn, 24, 21)
        if opc in (0, 1, 2):
            return (["br", "blr", "ret"][opc], None)
    return None


# --------------------------------------------------------------------------
# The translator
# --------------------------------------------------------------------------

class Translator(SimdMixin):
    """Translates instructions of one function. `ctx` supplies:
        ctx.local_label(addr) -> label name if addr is inside this function, else None
        ctx.func_name(addr)   -> C function name if a function starts at addr, else None
    """

    def __init__(self, ctx):
        self.ctx = ctx

    # ---- control-flow helpers ----
    def jump(self, pc, target):
        """C code for an unconditional jump to target."""
        label = self.ctx.local_label(target)
        if label:
            return f"goto {label};"
        fn = self.ctx.func_name(target)
        if fn:  # jumping to the start of another function = tail call
            return f"{fn}(c); return;"
        return f"recomp_bad_jump(c, {hx(pc)}, {hx(target)}); return;"

    def call(self, target):
        fn = self.ctx.func_name(target)
        return f"{fn}(c);" if fn else f"recomp_call_addr(c, {hx(target)});"

    # ---- entry point ----
    def translate(self, insn, pc):
        """Return a list of C statements for the instruction at pc."""
        op0 = bits(insn, 28, 25)
        if op0 in (0b1000, 0b1001):
            return self.dp_imm(insn, pc)
        if op0 in (0b1010, 0b1011):
            return self.branch_sys(insn, pc)
        if op0 & 0b0101 == 0b0100:
            return self.load_store(insn, pc)
        if op0 & 0b0111 == 0b0101:
            return self.dp_reg(insn, pc)
        if op0 & 0b0111 == 0b0111:
            if bits(insn, 30, 28) == 0b001:              # bit30=0, S=0, bit28=1: scalar FP
                return self.fp_scalar(insn, pc)
            return self.simd(insn, pc)
        if op0 == 0 and bits(insn, 31, 16) == 0:
            return [f"recomp_trap(c, {hx(pc)}, 0x{insn:08x}u); return;"]  # UDF
        raise Unsupported("unallocated / unknown group")

    # ======================================================================
    # Data processing - immediate
    # ======================================================================
    def dp_imm(self, i, pc):
        op = bits(i, 25, 23)
        sf = bit(i, 31)
        rd, rn = bits(i, 4, 0), bits(i, 9, 5)

        if op in (0b000, 0b001):                         # ADR / ADRP
            imm = sx((bits(i, 23, 5) << 2) | bits(i, 30, 29), 21)
            if bit(i, 31):
                val = (pc & ~0xFFF) + (imm << 12)
            else:
                val = pc + imm
            return [W(rd, 1, hx(val))]

        if op == 0b010:                                  # ADD/SUB (immediate)
            sub, s = bit(i, 30), bit(i, 29)
            imm = bits(i, 21, 10) << (12 if bit(i, 22) else 0)
            a = R(rn, sf, sp=True)
            if s:
                b = (~imm & mask(64 if sf else 32)) if sub else imm
                return [W(rd, sf, add_flags(sf, a, hx(b), 1 if sub else 0))]
            return [W(rd, sf, f"{a} {'-' if sub else '+'} {hx(imm)}", sp=True)]

        if op == 0b100:                                  # AND/ORR/EOR/ANDS (immediate)
            opc, n = bits(i, 30, 29), bit(i, 22)
            if not sf and n:
                raise Unsupported("logical imm N=1 with 32-bit")
            imm = decode_bit_masks(n, bits(i, 15, 10), bits(i, 21, 16), 64 if sf else 32)
            a = R(rn, sf)
            expr = f"{a} {['&', '|', '^', '&'][opc]} {hx(imm)}"
            if opc == 3:
                return [W(rd, sf, f"logic{64 if sf else 32}_flags(c, {expr})")]
            return [W(rd, sf, expr, sp=True)]

        if op == 0b101:                                  # MOVN/MOVZ/MOVK
            opc, hw = bits(i, 30, 29), bits(i, 22, 21)
            if opc == 1 or (not sf and hw >= 2):
                raise Unsupported("move wide unallocated")
            shift = hw * 16
            imm = bits(i, 20, 5) << shift
            if opc == 0:
                return [W(rd, sf, hx(~imm & mask(64 if sf else 32)))]
            if opc == 2:
                return [W(rd, sf, hx(imm))]
            keep = ~(0xFFFF << shift) & MASK64
            return [W(rd, sf, f"({R(rd, 1)} & {hx(keep)}) | {hx(imm)}")]

        if op == 0b110:                                  # SBFM/BFM/UBFM (+ aliases LSL, ASR, UBFX, SXTW...)
            opc, n = bits(i, 30, 29), bit(i, 22)
            immr, imms = bits(i, 21, 16), bits(i, 15, 10)
            ds = 64 if sf else 32
            if opc == 3 or n != sf or immr >= ds or imms >= ds:
                raise Unsupported("bitfield unallocated")
            # The two cases of every bitfield instruction:
            #   imms >= immr: take bits [imms:immr] of Rn, put them at bit 0   (UBFX/SBFX/BFXIL, LSR/ASR)
            #   imms <  immr: take bits [imms:0] of Rn, put them at bit ds-immr (UBFIZ/SBFIZ/BFI, LSL)
            if imms >= immr:
                width, src_lsb, dst_lsb = imms - immr + 1, immr, 0
            else:
                width, src_lsb, dst_lsb = imms + 1, 0, ds - immr
            src = R(rn, 1)
            field = f"(({src} >> {src_lsb}) & {hx(mask(width))})"
            if opc == 2:    # UBFM: zero-extend the field
                res = f"({field} << {dst_lsb})"
            elif opc == 0:  # SBFM: sign-extend the field
                res = f"(sext({field}, {width}) << {dst_lsb})"
            else:           # BFM: insert the field, keep the other bits of Rd
                keep = ~(mask(width) << dst_lsb) & MASK64
                res = f"(({R(rd, 1)} & {hx(keep)}) | ({field} << {dst_lsb}))"
            return [W(rd, sf, res)]

        if op == 0b111:                                  # EXTR (and ROR #imm)
            if bits(i, 30, 29) != 0 or bit(i, 21) or bit(i, 22) != sf:
                raise Unsupported("extract unallocated")
            rm, lsb = bits(i, 20, 16), bits(i, 15, 10)
            ds = 64 if sf else 32
            if lsb >= ds:
                raise Unsupported("extract lsb out of range")
            if lsb == 0:
                return [W(rd, sf, R(rm, sf))]
            return [W(rd, sf, f"({R(rm, sf)} >> {lsb}) | ({R(rn, sf)} << {ds - lsb})")]

        raise Unsupported("dp imm (tagged add/sub?)")

    # ======================================================================
    # Branches, exception generation, system
    # ======================================================================
    def branch_sys(self, i, pc):
        info = branch_info(i, pc)
        if info:
            kind, target = info
            if kind == "b":
                return [self.jump(pc, target)]
            if kind == "bl":
                # BL writes the return address to X30, then calls.
                return [f"c->x[30] = {hx(pc + 4)};", self.call(target)]
            if kind == "cond":
                return [f"if ({self.condition(i)}) {{ {self.jump(pc, target)} }}"]
            rn = bits(i, 9, 5)
            if kind == "br":
                return [f"jt = {R(rn, 1)}; goto dispatch;"]
            if kind == "blr":
                # read the target first: BLR X30 is legal
                return [f"{{ uint64_t t = {R(rn, 1)}; c->x[30] = {hx(pc + 4)}; recomp_call_addr(c, t); }}"]
            if kind == "ret":
                return ["return;"]

        if bits(i, 31, 24) == 0b11010100:                # exception generation
            opc, ll = bits(i, 23, 21), bits(i, 4, 0)
            imm16 = bits(i, 20, 5)
            if opc == 0 and ll == 1:
                return [f"recomp_svc(c, {hx(pc)}, 0x{imm16:x});"]
            return [f"recomp_trap(c, {hx(pc)}, 0x{i:08x}u); return;"]

        if (i & 0xFFFFF01F) == 0xD503201F:               # HINT space: NOP, YIELD, BTI, PACIASP...
            return ["/* hint */"]
        if (i & 0xFFD00000) == 0xD5100000:               # MRS / MSR (register)
            return self.sysreg(i)
        if (i & 0xFFF80000) == 0xD5080000:               # SYS: cache maintenance (DC/IC), DC ZVA
            return self.sys_op(i)
        if (i & 0xFFFFF01F) == 0xD503301F:               # barriers: DMB, DSB, ISB, CLREX
            if bits(i, 7, 5) == 0b010:                   # CLREX: forget the exclusive monitor
                return ["c->excl_addr = ~0ull;"]
            return ["__sync_synchronize();"]
        raise Unsupported("system register / other system instruction")

    def condition(self, i):
        """C expression for a conditional branch's condition (B.cond, CBZ, TBZ)."""
        if bits(i, 31, 24) == 0b01010100:
            return COND[bits(i, 3, 0)]
        rt = bits(i, 4, 0)
        if bits(i, 30, 25) == 0b011010:                  # CBZ/CBNZ
            sf = bit(i, 31)
            return f"{R(rt, sf)} {'!=' if bit(i, 24) else '=='} 0"
        b = (bit(i, 31) << 5) | bits(i, 23, 19)          # TBZ/TBNZ test bit b
        return f"(({R(rt, 1)} >> {b}) & 1) {'!=' if bit(i, 24) else '=='} 0"

    # ======================================================================
    # Loads and stores
    # ======================================================================
    def load_store(self, i, pc):
        grp = bits(i, 29, 27)
        if bit(i, 26):                                   # SIMD & FP registers
            if grp == 0b011 and bits(i, 25, 24) == 0:
                return self.ld_literal(i, pc, vec=True)
            if grp == 0b101 and bit(i, 25) == 0:
                return self.ld_st_pair(i, vec=True)
            if grp == 0b111 and bits(i, 25, 24) == 0b01:
                return self.ld_st_single(i, "uimm", vec=True)
            if grp == 0b111 and bits(i, 25, 24) == 0b00 and bit(i, 21) == 0:
                return self.ld_st_single(i, ["unscaled", "post", "unscaled", "pre"][bits(i, 11, 10)], vec=True)
            if grp == 0b111 and bits(i, 25, 24) == 0b00 and bits(i, 11, 10) == 0b10:
                return self.ld_st_single(i, "reg", vec=True)
            if grp == 0b001 and bit(i, 31) == 0 and bits(i, 28, 25) == 0b0110:
                return self.ldst_struct(i)                # LD1-LD4 / ST1-ST4 (multiple or single lane)
            raise Unsupported("SIMD load/store unallocated")
        if grp == 0b001 and bits(i, 25, 24) == 0:
            return self.ld_st_exclusive(i)
        if grp == 0b011 and bits(i, 25, 24) == 0:
            return self.ld_literal(i, pc)
        if grp == 0b101 and bit(i, 25) == 0:
            return self.ld_st_pair(i)
        if grp == 0b111:
            if bits(i, 25, 24) == 0b01:
                return self.ld_st_single(i, "uimm")
            if bits(i, 25, 24) == 0b00:
                if bit(i, 21) == 0:
                    return self.ld_st_single(i, ["unscaled", "post", "unscaled", "pre"][bits(i, 11, 10)])
                if bits(i, 11, 10) == 0b10:
                    return self.ld_st_single(i, "reg")
        raise Unsupported("load/store form (exclusive/atomic/other)")

    def ld_st_exclusive(self, i):
        """LDXR/STXR (+ acquire/release forms LDAXR/STLXR) and LDAR/STLR.
        Encoding: size(31:30) 001000 o2(23) L(22) o1(21) Rs(20:16) o0(15) Rt2(14:10) Rn Rt"""
        size, o2, load, o1, o0 = bits(i, 31, 30), bit(i, 23), bit(i, 22), bit(i, 21), bit(i, 15)
        rs, rn, rt = bits(i, 20, 16), bits(i, 9, 5), bits(i, 4, 0)
        nbits = 8 << size
        if o1:
            raise Unsupported("exclusive pair (LDXP/STXP) or CAS")
        addr = f"uint64_t a = {R(rn, 1, sp=True)};"
        if o2 == 0:                                   # exclusives
            if load:                                  # LDXR / LDAXR
                order = "__ATOMIC_ACQUIRE" if o0 else "__ATOMIC_RELAXED"
                return [f"{{ {addr} {W(rt, 1, f'excl_ld{nbits}(c, a, {order})')} }}"]
            # STXR / STLXR: Ws = 0 on success, 1 on failure
            return [f"{{ {addr} uint32_t s = excl_st{nbits}(c, a, {R(rt, 1)}); {W(rs, 0, 's')} }}"]
        if load:                                      # LDAR (o0=1) / LDLAR (o0=0)
            return [f"{{ {addr} {W(rt, 1, f'atomic_ld{nbits}(c, a, __ATOMIC_ACQUIRE)')} }}"]
        return [f"{{ {addr} atomic_st{nbits}(c, a, {R(rt, 1)}, __ATOMIC_RELEASE); }}"]   # STLR / STLLR

    def ld_literal(self, i, pc, vec=False):
        opc, rt = bits(i, 31, 30), bits(i, 4, 0)
        addr = hx(pc + sx(bits(i, 23, 5), 19) * 4)
        if vec:                                          # LDR St/Dt/Qt, =literal
            if opc == 3:
                raise Unsupported("load literal unallocated")
            return [f"vld{32 << opc}(c, {rt}, {addr});"]
        if opc == 0:
            return [W(rt, 1, f"ld32(c, {addr})")]
        if opc == 1:
            return [W(rt, 1, f"ld64(c, {addr})")]
        if opc == 2:
            return [W(rt, 1, f"sext(ld32(c, {addr}), 32)")]
        return ["/* prfm literal */"]

    def ld_st_single(self, i, mode, vec=False):
        size, opc = bits(i, 31, 30), bits(i, 23, 22)
        rn, rt = bits(i, 9, 5), bits(i, 4, 0)
        if vec and size == 0 and opc >= 2:
            size = 4                                     # Q register: 128 bits
            opc -= 2
        elif vec and opc >= 2:
            raise Unsupported("SIMD&FP load/store unallocated")
        nbits = 8 << size
        base = R(rn, 1, sp=True)

        if mode == "uimm":
            off = hx(bits(i, 21, 10) << size)
        elif mode == "reg":
            option, s = bits(i, 15, 13), bit(i, 12)
            if option not in (0b010, 0b011, 0b110, 0b111):
                raise Unsupported("reg offset bad extend")
            off = f"({self.extend(bits(i, 20, 16), option)} << {size if s else 0})"
        else:
            off = hx(sx(bits(i, 20, 12), 9))

        if vec:                                          # LDR/STR Bt, Ht, St, Dt, Qt
            op = f"{'vld' if opc else 'vst'}{nbits}(c, {rt}, a);"
        elif opc == 0:                                   # store
            op = f"st{nbits}(c, a, {R(rt, 1)});"
        elif opc == 1:                                   # load, zero-extend
            op = W(rt, 1, f"ld{nbits}(c, a)")
        elif opc == 2:
            if size == 3:
                return ["/* prfm */"]
            op = W(rt, 1, f"sext(ld{nbits}(c, a), {nbits})")   # LDRSB/LDRSH/LDRSW Xt
        else:
            if size >= 2:
                raise Unsupported("load/store unallocated")
            op = W(rt, 0, f"sext(ld{nbits}(c, a), {nbits})")   # LDRSB/LDRSH Wt

        if mode == "post":
            return [f"{{ uint64_t a = {base}; {op} {W(rn, 1, f'a + {off}', sp=True)} }}"]
        if mode == "pre":
            return [f"{{ uint64_t a = {base} + {off}; {op} {W(rn, 1, 'a', sp=True)} }}"]
        return [f"{{ uint64_t a = {base} + {off}; {op} }}"]

    def ld_st_pair(self, i, vec=False):
        opc, mode, load = bits(i, 31, 30), bits(i, 24, 23), bit(i, 22)
        rt, rn, rt2 = bits(i, 4, 0), bits(i, 9, 5), bits(i, 14, 10)
        if opc == 3 or (opc == 1 and not load and not vec):
            raise Unsupported("pair unallocated / STGP")
        size = (4 << opc) if vec else (8 if opc == 2 else 4)   # vec: S, D or Q registers
        nbits = size * 8
        off = hx(sx(bits(i, 21, 15), 7) * size)
        base = R(rn, 1, sp=True)
        if vec:
            fn = "vld" if load else "vst"
            ops = f"{fn}{nbits}(c, {rt}, a); {fn}{nbits}(c, {rt2}, a + {size});"
        elif load:
            if opc == 1:     # LDPSW
                ops = W(rt, 1, "sext(ld32(c, a), 32)") + " " + W(rt2, 1, "sext(ld32(c, a + 4), 32)")
            else:
                ops = W(rt, 1, f"ld{nbits}(c, a)") + " " + W(rt2, 1, f"ld{nbits}(c, a + {size})")
        else:
            ops = f"st{nbits}(c, a, {R(rt, 1)}); st{nbits}(c, a + {size}, {R(rt2, 1)});"
        if mode == 0b01:     # post-index
            return [f"{{ uint64_t a = {base}; {ops} {W(rn, 1, f'a + {off}', sp=True)} }}"]
        if mode == 0b11:     # pre-index
            return [f"{{ uint64_t a = {base} + {off}; {ops} {W(rn, 1, 'a', sp=True)} }}"]
        return [f"{{ uint64_t a = {base} + {off}; {ops} }}"]  # signed offset / non-temporal

    # ======================================================================
    # Data processing - register
    # ======================================================================
    def shifted(self, rm, sf, shift, amount):
        v = R(rm, sf)
        if amount == 0:
            return v
        if shift == 0:
            return f"({v} << {amount})"
        if shift == 1:
            return f"({v} >> {amount})"
        if shift == 2:
            return f"(uint64_t)((int64_t){v} >> {amount})" if sf else f"(uint32_t)((int32_t){v} >> {amount})"
        return f"ror{64 if sf else 32}({v}, {amount})"

    def extend(self, rm, option):
        """UXTB/UXTH/UXTW/UXTX/SXTB/SXTH/SXTW/SXTX of register rm (64-bit result)."""
        v = R(rm, 1)
        return [f"(uint64_t)(uint8_t){v}", f"(uint64_t)(uint16_t){v}", f"(uint64_t)(uint32_t){v}", v,
                f"sext({v}, 8)", f"sext({v}, 16)", f"sext({v}, 32)", v][option]

    def dp_reg(self, i, pc):
        sf = bit(i, 31)
        rd, rn, rm = bits(i, 4, 0), bits(i, 9, 5), bits(i, 20, 16)
        ds = 64 if sf else 32

        if bit(i, 28) == 0:
            if bit(i, 24) == 0:                          # logical (shifted register)
                opc, shift, n, amt = bits(i, 30, 29), bits(i, 23, 22), bit(i, 21), bits(i, 15, 10)
                if amt >= ds:
                    raise Unsupported("shift amount")
                b = self.shifted(rm, sf, shift, amt)
                if n:
                    b = f"~{b}"
                expr = f"{R(rn, sf)} {['&', '|', '^', '&'][opc]} {b}"
                if opc == 3:
                    return [W(rd, sf, f"logic{ds}_flags(c, {expr})")]
                return [W(rd, sf, expr)]

            sub, s = bit(i, 30), bit(i, 29)
            if bit(i, 21) == 0:                          # add/sub (shifted register)
                shift, amt = bits(i, 23, 22), bits(i, 15, 10)
                if shift == 3 or amt >= ds:
                    raise Unsupported("add/sub shift")
                a, b, rd_sp = R(rn, sf), self.shifted(rm, sf, shift, amt), False
            else:                                        # add/sub (extended register)
                if bits(i, 23, 22) != 0 or bits(i, 12, 10) > 4:
                    raise Unsupported("add/sub ext")
                a = R(rn, sf, sp=True)
                b = f"({self.extend(rm, bits(i, 15, 13))} << {bits(i, 12, 10)})"
                rd_sp = not s
            if s:
                return [W(rd, sf, add_flags(sf, a, f"~{b}" if sub else b, 1 if sub else 0))]
            return [W(rd, sf, f"{a} {'-' if sub else '+'} {b}", sp=rd_sp)]

        op2 = bits(i, 24, 21)
        if op2 == 0b0000:                                # ADC/ADCS/SBC/SBCS
            if bits(i, 15, 10) != 0:
                raise Unsupported("rmif/setf")
            sub, s = bit(i, 30), bit(i, 29)
            b = f"~{R(rm, sf)}" if sub else R(rm, sf)
            if s:
                return [W(rd, sf, add_flags(sf, R(rn, sf), b, "c->c"))]
            return [W(rd, sf, f"{R(rn, sf)} + {b} + c->c")]

        if op2 == 0b0010:                                # CCMN/CCMP
            if bit(i, 29) == 0 or bit(i, 10) or bit(i, 4):
                raise Unsupported("ccmp unallocated")
            cond, nzcv, is_cmp = bits(i, 15, 12), bits(i, 3, 0), bit(i, 30)
            if bit(i, 11):
                imm = rm  # 5-bit immediate lives in the Rm field
                b = hx((~imm & mask(ds)) if is_cmp else imm)
            else:
                b = f"~{R(rm, sf)}" if is_cmp else R(rm, sf)
            return [f"if ({COND[cond]}) {add_flags(sf, R(rn, sf), b, 1 if is_cmp else 0)}; "
                    f"else set_nzcv(c, {nzcv});"]

        if op2 == 0b0100:                                # CSEL/CSINC/CSINV/CSNEG
            if bit(i, 29) or bit(i, 11):
                raise Unsupported("csel unallocated")
            cond = bits(i, 15, 12)
            kind = (bit(i, 30) << 1) | bit(i, 10)
            m = R(rm, sf)
            alt = [m, f"({m} + 1)", f"~{m}", f"(0 - {m})"][kind]
            return [W(rd, sf, f"({COND[cond]}) ? {R(rn, sf)} : {alt}")]

        if op2 == 0b0110:
            if bit(i, 30) == 0:                          # 2-source
                opcode = bits(i, 15, 10)
                n, m = R(rn, sf), R(rm, sf)
                amt = f"({m} & {ds - 1})"
                table = {
                    0b000010: f"udiv{ds}({n}, {m})",
                    0b000011: f"sdiv{ds}({n}, {m})",
                    0b001000: f"{n} << {amt}",
                    0b001001: f"{n} >> {amt}",
                    0b001010: (f"(uint64_t)((int64_t){n} >> {amt})" if sf
                               else f"(uint32_t)((int32_t){n} >> {amt})"),
                    0b001011: f"ror{ds}({n}, {amt})",
                }
                if (opcode >> 3) == 0b010 and not bit(i, 29):   # CRC32B/H/W/X, CRC32CB/CH/CW/CX
                    sz = opcode & 3
                    if (sz == 3) != bool(sf):
                        raise Unsupported("crc32 unallocated")
                    poly = "0x82F63B78u" if (opcode >> 2) & 1 else "0xEDB88320u"
                    return [W(rd, 0, f"crc32_update({R(rn, 0)}, {R(rm, 1)}, {1 << sz}, {poly})")]
                if opcode not in table or bit(i, 29):
                    raise Unsupported("2-source (pac?)")
                return [W(rd, sf, table[opcode])]
            # 1-source
            opcode = bits(i, 15, 10)
            if bits(i, 20, 16) != 0 or bit(i, 29):
                raise Unsupported("1-source (pac?)")
            n = R(rn, sf)
            if opcode == 0:
                return [W(rd, sf, f"rbit{ds}({n})")]
            if opcode == 1:
                return [W(rd, sf, f"rev16_64({n})")]
            if opcode == 2:
                return [W(rd, sf, f"rev32_64({n})" if sf else f"__builtin_bswap32({n})")]
            if opcode == 3 and sf:
                return [W(rd, sf, f"__builtin_bswap64({n})")]
            if opcode == 4:
                return [W(rd, sf, f"clz{ds}({n})")]
            if opcode == 5:
                return [W(rd, sf, f"cls{ds}({n})")]
            raise Unsupported("1-source opcode")

        if bit(i, 24):                                   # 3-source: MADD, MSUB, SMULL, UMULH...
            if bits(i, 30, 29) != 0:
                raise Unsupported("3-source op54")
            op31, o0, ra = bits(i, 23, 21), bit(i, 15), bits(i, 14, 10)
            sign = "-" if o0 else "+"
            if op31 == 0:
                return [W(rd, sf, f"{R(ra, sf)} {sign} {R(rn, sf)} * {R(rm, sf)}")]
            if not sf:
                raise Unsupported("3-source 32-bit widening")
            wn, wm = R(rn, 0), R(rm, 0)
            if op31 == 0b001:                            # SMADDL/SMSUBL (SMULL)
                prod = f"(uint64_t)((int64_t)(int32_t){wn} * (int64_t)(int32_t){wm})"
                return [W(rd, 1, f"{R(ra, 1)} {sign} {prod}")]
            if op31 == 0b101:                            # UMADDL/UMSUBL (UMULL)
                prod = f"((uint64_t){wn} * (uint64_t){wm})"
                return [W(rd, 1, f"{R(ra, 1)} {sign} {prod}")]
            if op31 == 0b010 and not o0:
                return [W(rd, 1, f"smulh({R(rn, 1)}, {R(rm, 1)})")]
            if op31 == 0b110 and not o0:
                return [W(rd, 1, f"umulh({R(rn, 1)}, {R(rm, 1)})")]
            raise Unsupported("3-source op31")

        raise Unsupported("dp reg other")

    # ======================================================================
    # Scalar floating point
    # ======================================================================
    # ftype (bits 23:22): 00 = single (S, float), 01 = double (D, double), 11 = half (H)

    @staticmethod
    def ftype(ft, half_ok=False):
        if ft == 0:
            return "s"
        if ft == 1:
            return "d"
        if ft == 3 and half_ok:
            return "h"
        raise Unsupported("floating point: half-precision arithmetic (ARMv8.2)" if ft == 3
                          else "floating point: unallocated type")

    @staticmethod
    def FV(n, t):
        """Value of scalar register n as a C float/double."""
        return f"c->V[{n}].f[0]" if t == "s" else f"c->V[{n}].fd[0]"

    @staticmethod
    def FB(n, t):
        """Raw bits of scalar register n."""
        return {"h": f"c->V[{n}].h[0]", "s": f"c->V[{n}].s[0]", "d": f"c->V[{n}].d[0]"}[t]

    def fp_scalar(self, i, pc):
        # bit 31 is "sf" (64-bit integer side) for conversions, and must be 0 for everything else
        if bit(i, 24) == 0 and bit(i, 21) == 0:
            return self.fp_fixed_conv(i)
        if bit(i, 24) == 0 and bits(i, 15, 10) == 0:
            return self.fp_int_conv(i)
        if bit(i, 31):
            raise Unsupported("floating point: unallocated")
        if bit(i, 24):
            return self.fp_3src(i)
        if bits(i, 14, 10) == 0b10000:
            return self.fp_1src(i)
        if bits(i, 13, 10) == 0b1000:
            return self.fp_compare(i)
        if bits(i, 12, 10) == 0b100:
            return self.fp_imm(i)
        k = bits(i, 11, 10)
        if k == 0b01:
            return self.fp_ccmp(i)
        if k == 0b10:
            return self.fp_2src(i)
        return self.fp_csel(i)

    def fp_1src(self, i):
        opc, rd, rn = bits(i, 20, 15), bits(i, 4, 0), bits(i, 9, 5)
        if 0b000100 <= opc <= 0b000111:                  # FCVT between precisions
            src = self.ftype(bits(i, 23, 22), half_ok=True)
            dst = self.ftype(opc & 3, half_ok=True)
            if src == dst:
                raise Unsupported("floating point: FCVT to same type")
            val = {"s": f"(double){self.FV(rn, 's')}", "d": self.FV(rn, "d"),
                   "h": f"f16_to_f64({self.FB(rn, 'h')})"}[src]
            if dst == "h":
                return [f"set_hbits(c, {rd}, f64_to_f16({val}));"]
            return [f"set_{dst}(c, {rd}, ({'float' if dst == 's' else 'double'})({val}));"]
        t = self.ftype(bits(i, 23, 22))
        sign = "0x80000000u" if t == "s" else "0x8000000000000000ull"
        if opc == 0:                                     # FMOV (register)
            return [f"set_{t}bits(c, {rd}, {self.FB(rn, t)});"]
        if opc == 1:                                     # FABS: clear the sign bit
            return [f"set_{t}bits(c, {rd}, {self.FB(rn, t)} & ~{sign});"]
        if opc == 2:                                     # FNEG: flip the sign bit
            return [f"set_{t}bits(c, {rd}, {self.FB(rn, t)} ^ {sign});"]
        if opc == 3:                                     # FSQRT
            fn = "__builtin_sqrtf" if t == "s" else "__builtin_sqrt"
            return [f"set_{t}(c, {rd}, {fn}({self.FV(rn, t)}));"]
        modes = {0b001000: 0, 0b001001: 1, 0b001010: 2, 0b001011: 3, 0b001100: 4, 0b001110: 5, 0b001111: 5}
        if opc in modes:                                 # FRINTN/P/M/Z/A/X/I
            fn = "froundf" if t == "s" else "fround"
            return [f"set_{t}(c, {rd}, {fn}({self.FV(rn, t)}, {modes[opc]}));"]
        raise Unsupported("floating point: 1-source opcode")

    def fp_2src(self, i):
        t = self.ftype(bits(i, 23, 22))
        opc, rd, rn, rm = bits(i, 15, 12), bits(i, 4, 0), bits(i, 9, 5), bits(i, 20, 16)
        a, b = self.FV(rn, t), self.FV(rm, t)
        exprs = {0: f"{a} * {b}", 1: f"{a} / {b}", 2: f"{a} + {b}", 3: f"{a} - {b}",
                 4: f"fmax_{t}({a}, {b})", 5: f"fmin_{t}({a}, {b})",
                 6: f"fmaxnm_{t}({a}, {b})", 7: f"fminnm_{t}({a}, {b})", 8: f"-({a} * {b})"}
        if opc not in exprs:
            raise Unsupported("floating point: 2-source opcode")
        return [f"set_{t}(c, {rd}, {exprs[opc]});"]

    def fp_3src(self, i):
        t = self.ftype(bits(i, 23, 22))
        rd, rn, rm, ra = bits(i, 4, 0), bits(i, 9, 5), bits(i, 20, 16), bits(i, 14, 10)
        n, m, a = self.FV(rn, t), self.FV(rm, t), self.FV(ra, t)
        fma = "__builtin_fmaf" if t == "s" else "__builtin_fma"
        # fused: a single rounding at the end. FMADD a+n*m, FMSUB a-n*m, FNMADD -a-n*m, FNMSUB -a+n*m
        args = {(0, 0): f"{n}, {m}, {a}", (0, 1): f"-{n}, {m}, {a}",
                (1, 0): f"-{n}, {m}, -{a}", (1, 1): f"{n}, {m}, -{a}"}[(bit(i, 21), bit(i, 15))]
        return [f"set_{t}(c, {rd}, {fma}({args}));"]

    def fp_compare(self, i):
        t = self.ftype(bits(i, 23, 22))
        if bits(i, 15, 14) or bits(i, 2, 0):
            raise Unsupported("floating point: compare unallocated")
        rn, rm = bits(i, 9, 5), bits(i, 20, 16)
        b = "0.0" if bit(i, 3) else self.FV(rm, t)       # FCMP Sn, #0.0
        return [f"fcmp(c, {self.FV(rn, t)}, {b});"]      # FCMP and FCMPE give the same flags

    def fp_ccmp(self, i):
        t = self.ftype(bits(i, 23, 22))
        rn, rm, cond, nzcv = bits(i, 9, 5), bits(i, 20, 16), bits(i, 15, 12), bits(i, 3, 0)
        return [f"if ({COND[cond]}) fcmp(c, {self.FV(rn, t)}, {self.FV(rm, t)}); else set_nzcv(c, {nzcv});"]

    def fp_csel(self, i):
        t = self.ftype(bits(i, 23, 22))
        rd, rn, rm, cond = bits(i, 4, 0), bits(i, 9, 5), bits(i, 20, 16), bits(i, 15, 12)
        return [f"set_{t}bits(c, {rd}, ({COND[cond]}) ? {self.FB(rn, t)} : {self.FB(rm, t)});"]

    def fp_imm(self, i):
        t = self.ftype(bits(i, 23, 22))
        if bits(i, 9, 5):
            raise Unsupported("floating point: imm unallocated")
        imm8, rd = bits(i, 20, 13), bits(i, 4, 0)
        n_bits, e, f = (32, 8, 23) if t == "s" else (64, 11, 52)
        b6 = bit(imm8, 6)
        exp = ((b6 ^ 1) << (e - 1)) | ((mask(e - 3) if b6 else 0) << 2) | bits(imm8, 5, 4)
        val = (bit(imm8, 7) << (n_bits - 1)) | (exp << f) | (bits(imm8, 3, 0) << (f - 4))
        return [f"set_{t}bits(c, {rd}, {hx(val)});"]    # FMOV Sd, #imm (VFPExpandImm)

    def fp_int_conv(self, i):
        sf, ft, rmode, opcode = bit(i, 31), bits(i, 23, 22), bits(i, 20, 19), bits(i, 18, 16)
        rd, rn = bits(i, 4, 0), bits(i, 9, 5)
        if opcode in (6, 7):                             # FMOV between general and FP registers
            to_gen = opcode == 6
            if (sf, ft, rmode) == (0, 0, 0):
                return [W(rd, 0, self.FB(rn, "s"))] if to_gen else [f"set_sbits(c, {rd}, {R(rn, 0)});"]
            if (sf, ft, rmode) == (1, 1, 0):
                return [W(rd, 1, self.FB(rn, "d"))] if to_gen else [f"set_dbits(c, {rd}, {R(rn, 1)});"]
            if (sf, ft, rmode) == (1, 2, 1):             # FMOV Xd, Vn.D[1] / Vd.D[1], Xn
                return [W(rd, 1, f"c->V[{rn}].d[1]")] if to_gen else [f"c->V[{rd}].d[1] = {R(rn, 1)};"]
            raise Unsupported("floating point: FMOV half / unallocated")
        t = self.ftype(ft)
        nb = 64 if sf else 32
        val = f"(double){self.FV(rn, t)}" if t == "s" else self.FV(rn, t)
        if opcode in (0, 1):                             # FCVTNS/PS/MS/ZS, FCVTNU/PU/MU/ZU
            return [W(rd, sf, f"fcvt_{'u' if opcode else 's'}({val}, {rmode}, {nb})")]
        if opcode in (4, 5) and rmode == 0:              # FCVTAS/FCVTAU: nearest, ties away
            return [W(rd, sf, f"fcvt_{'u' if opcode == 5 else 's'}({val}, 4, {nb})")]
        if opcode in (2, 3) and rmode == 0:              # SCVTF/UCVTF
            if opcode == 2:
                src = f"(int64_t){R(rn, 1)}" if sf else f"(int32_t){R(rn, 0)}"
            else:
                src = R(rn, sf)
            return [f"set_{t}(c, {rd}, ({'float' if t == 's' else 'double'})({src}));"]
        raise Unsupported("floating point: conversion opcode")

    def fp_fixed_conv(self, i):
        sf, ft, rmode, opcode = bit(i, 31), bits(i, 23, 22), bits(i, 20, 19), bits(i, 18, 16)
        rd, rn, scale = bits(i, 4, 0), bits(i, 9, 5), bits(i, 15, 10)
        t = self.ftype(ft)
        fbits = 64 - scale
        if not sf and scale < 32:
            raise Unsupported("floating point: fixed-point unallocated")
        nb = 64 if sf else 32
        if rmode == 3 and opcode in (0, 1):              # FCVTZS/FCVTZU with fraction bits
            val = f"(double){self.FV(rn, t)} * {float(2 ** fbits).hex()}"
            return [W(rd, sf, f"fcvt_{'u' if opcode else 's'}({val}, 3, {nb})")]
        if rmode == 0 and opcode in (2, 3):              # SCVTF/UCVTF with fraction bits
            ctype = "float" if t == "s" else "double"
            if opcode == 2:
                src = f"(int64_t){R(rn, 1)}" if sf else f"(int32_t){R(rn, 0)}"
            else:
                src = R(rn, sf)
            return [f"set_{t}(c, {rd}, ({ctype})(({ctype})({src}) * {float(2.0 ** -fbits).hex()}));"]
        raise Unsupported("floating point: fixed-point conversion opcode")

    # ======================================================================
    # System registers (only the ones user-mode code can use)
    # ======================================================================
    SYSREGS = {   # (op0, op1, CRn, CRm, op2) -> name
        (3, 3, 13, 0, 2): "tpidr_el0", (3, 3, 13, 0, 3): "tpidrro_el0",
        (3, 3, 4, 4, 0): "fpcr", (3, 3, 4, 4, 1): "fpsr", (3, 3, 4, 2, 0): "nzcv",
        (3, 3, 14, 0, 0): "cntfrq_el0", (3, 3, 14, 0, 1): "cntpct_el0", (3, 3, 14, 0, 2): "cntvct_el0",
        (3, 3, 0, 0, 7): "dczid_el0", (3, 3, 0, 0, 1): "ctr_el0",
    }

    def sysreg(self, i):
        load = bit(i, 21)
        key = (2 + bit(i, 19), bits(i, 18, 16), bits(i, 15, 12), bits(i, 11, 8), bits(i, 7, 5))
        name, rt = self.SYSREGS.get(key), bits(i, 4, 0)
        if name is None:
            raise Unsupported(f"system register {key}")
        if load:                                         # MRS Xt, <reg>
            val = {"tpidr_el0": "c->tpidr_el0", "tpidrro_el0": "c->tpidrro_el0", "fpcr": "c->fpcr",
                   "fpsr": "c->fpsr",
                   "nzcv": "((uint64_t)c->n << 31 | (uint64_t)c->z << 30 | (uint64_t)c->c << 29 | (uint64_t)c->v << 28)",
                   "cntfrq_el0": "19200000ull", "cntpct_el0": "recomp_cntpct()", "cntvct_el0": "recomp_cntpct()",
                   "dczid_el0": "4ull",                  # DC ZVA zeroes 2^4 words = 64 bytes
                   "ctr_el0": "0x8444C004ull"}[name]     # cache type register, Cortex-A57 value
            return [W(rt, 1, val)]
        dst = {"tpidr_el0": "c->tpidr_el0", "fpcr": "c->fpcr", "fpsr": "c->fpsr"}.get(name)
        if name == "nzcv":
            return [f"set_nzcv(c, (int)({R(rt, 1)} >> 28));"]
        if dst is None:
            raise Unsupported(f"write to read-only system register {name}")
        return [f"{dst} = {R(rt, 1)};"]

    def sys_op(self, i):
        op1, crn, crm, op2, rt = bits(i, 18, 16), bits(i, 15, 12), bits(i, 11, 8), bits(i, 7, 5), bits(i, 4, 0)
        if (op1, crn, crm, op2) == (3, 7, 4, 1):        # DC ZVA: zero a 64-byte block
            return [f"{{ uint64_t a = {R(rt, 1)} & ~63ull; RECOMP_MEMCHK(c, a, 64); memset(c->mem + a, 0, 64); }}"]
        if crn == 7:                                     # other DC / IC: caches don't exist for us
            return ["/* cache maintenance */"]
        raise Unsupported("system instruction (SYS)")
