"""
simd.py - translation of Advanced SIMD (NEON) instructions, as a mixin for arm64.Translator.

A vector register is 128 bits, split into lanes: 16 x 8-bit (B), 8 x 16-bit (H), 4 x 32-bit (S)
or 2 x 64-bit (D). "Q" (bit 30) chooses the full 128 bits or just the low 64 (the upper half
of the destination is then zeroed). "size" (bits 23:22) is the lane width: 0=8, 1=16, 2=32, 3=64.

Generated code for e.g. FADD V0.4S, V1.4S, V2.4S:
    { V128 a = c->V[1], b = c->V[2]; V128 r; for (int k = 0; k < 4; k++) { r.f[k] = a.f[k] + b.f[k]; } c->V[0] = r; }
Sources are copied first, so it works even when the destination is also a source.

Encodings follow the Arm ARM section "Data Processing -- Scalar Floating-Point and Advanced SIMD".
"""
from arm64 import Unsupported, bits, bit, hx, mask, R, W, COND  # noqa: F401

UF = ["b", "h", "s", "d"]                       # unsigned lane field by size
SF = ["sb", "sh", "sw", "sx"]                   # signed lane field
UT = ["uint8_t", "uint16_t", "uint32_t", "uint64_t"]
ST = ["int8_t", "int16_t", "int32_t", "int64_t"]


def ffield(size):
    return {2: "f", 3: "fd"}[size]


def fsuf(size):
    return {2: "s", 3: "d"}[size]


def fty(size):
    return {2: "float", 3: "double"}[size]


def ones(size):
    return hx(mask(8 << size))


class SimdMixin:
    # ------------------------------------------------------------------ emit helpers
    @staticmethod
    def vloop(rd, n, body, srcs=(), acc=False, q=1, zero=False, pre="", post=""):
        """Emit one vector operation: for k in [0, n): body (writes r.<lane>[k]).
        srcs: (name, reg) copied first. acc: r starts as Vd. zero: the result is a scalar, so
        everything except lane 0 ends up zero (also when acc). q=0: the upper 64 bits are cleared."""
        decl = "".join(f"V128 {nm} = c->V[{rg}]; " for nm, rg in srcs)
        if acc:
            init = f"V128 r = c->V[{rd}]; "
        elif zero:
            init = "V128 r; memset(&r, 0, 16); "
        else:
            init = "V128 r; "
        tail = "r.d[1] = 0; " if (q == 0 and not zero) else ""
        if zero and acc:      # scalar accumulate: keep only the low lane of the result
            field = body.split("r.", 1)[1].split("[", 1)[0]
            tail = (f"{{ V128 t; memset(&t, 0, 16); memcpy(&t, &r.{field}[0], sizeof r.{field}[0]); r = t; }} ")
        loop = (f"for (int k = 0; k < {n}; k++) {{ {body} }} " if n > 1
                else f"{{ const int k = 0; {body} }} ")
        return [f"{{ {decl}{init}{pre}{loop}{post}{tail}c->V[{rd}] = r; }}"]

    @staticmethod
    def nlanes(q, size):
        return (16 if q else 8) >> size

    # ------------------------------------------------------------------ dispatch
    def simd(self, i, pc):
        b3128, b2824 = bits(i, 31, 28), bits(i, 28, 24)
        scalar = bits(i, 31, 30) == 0b01 and bit(i, 28)
        vector = bit(i, 31) == 0 and bit(i, 28) == 0

        # crypto
        if bits(i, 31, 24) == 0x4E and bits(i, 21, 17) == 0b10100 and bits(i, 11, 10) == 0b10:
            return self.crypto_aes(i)
        if bits(i, 31, 24) == 0x5E and bit(i, 21) == 0 and bit(i, 15) == 0 and bits(i, 11, 10) == 0:
            return self.crypto_sha3(i)
        if bits(i, 31, 24) == 0x5E and bits(i, 21, 17) == 0b10100 and bits(i, 11, 10) == 0b10:
            return self.crypto_sha2(i)

        if (vector and bits(i, 28, 24) == 0b01110) or (scalar and bits(i, 28, 24) == 0b11110):
            if bit(i, 21):
                if bit(i, 10):
                    return self.three_same(i, scalar)
                if bits(i, 11, 10) == 0b00 and vector:
                    return self.three_diff(i)
                if bits(i, 11, 10) == 0b10:
                    if bits(i, 20, 17) == 0b0000:
                        return self.two_misc(i, scalar)
                    if bits(i, 20, 17) == 0b1000:
                        return self.pairwise_scalar(i) if scalar else self.across(i)
                raise Unsupported("SIMD: unallocated (bit21 group)")
            # bit 21 == 0
            if bits(i, 23, 21) == 0 and bit(i, 15) == 0 and bit(i, 10) == 1:
                return self.copy(i, scalar)
            if vector and bit(i, 29) and bits(i, 23, 21) == 0 and bit(i, 15) == 0 and bit(i, 10) == 0:
                return self.ext(i)
            if vector and not bit(i, 29) and bits(i, 23, 21) == 0 and bit(i, 15) == 0 and bits(i, 11, 10) == 0:
                return self.tbl(i)
            if vector and not bit(i, 29) and bit(i, 21) == 0 and bit(i, 15) == 0 and bits(i, 11, 10) == 0b10:
                return self.permute(i)
            raise Unsupported("SIMD: FP16 / unallocated (bit21=0 group)")
        if (vector and b2824 == 0b01111) or (scalar and b2824 == 0b11111):
            if bit(i, 10):
                if vector and bits(i, 23, 19) == 0:
                    return self.mod_imm(i)
                if bits(i, 22, 19) == 0:
                    raise Unsupported("SIMD: unallocated (scalar mod imm)")
                return self.shift_imm(i, scalar)
            return self.by_element(i, scalar)
        raise Unsupported(f"SIMD: unknown group {b3128:04b}")

    # ------------------------------------------------------------------ crypto
    def crypto_aes(self, i):
        op = bits(i, 16, 12) - 0b00100
        if bits(i, 23, 22) or op not in (0, 1, 2, 3):
            raise Unsupported("SIMD: crypto AES unallocated")
        rd, rn = bits(i, 4, 0), bits(i, 9, 5)
        return [f"{{ V128 n = c->V[{rn}]; recomp_aes(&c->V[{rd}], &n, {op}); }}"]

    def crypto_sha3(self, i):
        op = bits(i, 14, 12)
        if bits(i, 23, 22) or op == 7:
            raise Unsupported("SIMD: crypto SHA unallocated")
        rd, rn, rm = bits(i, 4, 0), bits(i, 9, 5), bits(i, 20, 16)
        return [f"{{ V128 n = c->V[{rn}], m = c->V[{rm}]; recomp_sha3(&c->V[{rd}], &n, &m, {op}); }}"]

    def crypto_sha2(self, i):
        op = bits(i, 16, 12)
        if bits(i, 23, 22) or op > 2:
            raise Unsupported("SIMD: crypto SHA unallocated")
        rd, rn = bits(i, 4, 0), bits(i, 9, 5)
        return [f"{{ V128 n = c->V[{rn}]; recomp_sha2(&c->V[{rd}], &n, {op}); }}"]

    # ------------------------------------------------------------------ copy: DUP/INS/UMOV/SMOV
    def copy(self, i, scalar):
        q, op, imm5, imm4 = bit(i, 30), bit(i, 29), bits(i, 20, 16), bits(i, 14, 11)
        rd, rn = bits(i, 4, 0), bits(i, 9, 5)
        size = (imm5 & -imm5).bit_length() - 1 if imm5 & 0xF else 4
        if size > 3:
            raise Unsupported("SIMD: copy unallocated")
        idx = imm5 >> (size + 1)
        u = UF[size]
        if scalar:                                       # DUP (element), scalar: MOV Sd, Vn.S[i]
            if op or imm4:
                raise Unsupported("SIMD: scalar copy unallocated")
            return self.vloop(rd, 1, f"r.{u}[0] = a.{u}[{idx}];", [("a", rn)], zero=True)
        if op:                                           # INS (element)
            idx2 = imm4 >> size
            return [f"{{ {UT[size]} e = c->V[{rn}].{u}[{idx2}]; c->V[{rd}].{u}[{idx}] = e; }}"]
        if imm4 == 0:                                    # DUP (element)
            return self.vloop(rd, self.nlanes(q, size), f"r.{u}[k] = e;", [("a", rn)], q=q,
                              pre=f"{UT[size]} e = a.{u}[{idx}]; ")
        if imm4 == 1:                                    # DUP (general)
            return self.vloop(rd, self.nlanes(q, size), f"r.{u}[k] = e;", q=q,
                              pre=f"{UT[size]} e = ({UT[size]}){R(rn, 1)}; ")
        if imm4 == 0b0011:                               # INS (general): MOV Vd.S[i], Wn
            return [f"c->V[{rd}].{u}[{idx}] = ({UT[size]}){R(rn, 1)};"]
        if imm4 == 0b0101:                               # SMOV
            if size >= 2 + q:
                raise Unsupported("SIMD: smov unallocated")
            return [W(rd, q, f"(uint64_t)(int64_t)c->V[{rn}].{SF[size]}[{idx}]")]
        if imm4 == 0b0111:                               # UMOV / MOV Wd, Vn.S[i]
            if (q and size != 3) or (not q and size == 3):
                raise Unsupported("SIMD: umov unallocated")
            return [W(rd, q, f"c->V[{rn}].{u}[{idx}]")]
        raise Unsupported("SIMD: copy unallocated")

    # ------------------------------------------------------------------ EXT, TBL, ZIP/UZP/TRN
    def ext(self, i):
        q, imm4 = bit(i, 30), bits(i, 14, 11)
        rd, rn, rm = bits(i, 4, 0), bits(i, 9, 5), bits(i, 20, 16)
        if not q and imm4 >= 8:
            raise Unsupported("SIMD: ext unallocated")
        nb = 16 if q else 8
        body = f"int p = k + {imm4}; r.b[k] = p < {nb} ? a.b[p] : b.b[p - {nb}];"
        return self.vloop(rd, nb, body, [("a", rn), ("b", rm)], q=q)

    def tbl(self, i):
        q, length, tbx = bit(i, 30), bits(i, 14, 13) + 1, bit(i, 12)
        rd, rn, rm = bits(i, 4, 0), bits(i, 9, 5), bits(i, 20, 16)
        nb = 16 if q else 8
        regs = [(rn + k) % 32 for k in range(length)]
        pre = f"uint8_t tab[{16 * length}]; " + "".join(
            f"memcpy(tab + {16 * k}, &c->V[{r}], 16); " for k, r in enumerate(regs))
        miss = "" if tbx else " else r.b[k] = 0;"
        body = f"uint8_t x = idx.b[k]; if (x < {16 * length}) r.b[k] = tab[x];{miss}"
        return self.vloop(rd, nb, body, [("idx", rm)], acc=bool(tbx), q=q, pre=pre)

    def permute(self, i):
        q, size, opc = bit(i, 30), bits(i, 23, 22), bits(i, 14, 12)
        rd, rn, rm = bits(i, 4, 0), bits(i, 9, 5), bits(i, 20, 16)
        if size == 3 and not q:
            raise Unsupported("SIMD: permute unallocated")
        e, u = self.nlanes(q, size), UF[size]
        h = e // 2
        bodies = {
            0b001: f"r.{u}[k] = k < {h} ? a.{u}[2 * k] : b.{u}[2 * k - {e}];",            # UZP1
            0b101: f"r.{u}[k] = k < {h} ? a.{u}[2 * k + 1] : b.{u}[2 * k + 1 - {e}];",    # UZP2
            0b010: f"r.{u}[k] = (k & 1) ? b.{u}[k - 1] : a.{u}[k];",                       # TRN1
            0b110: f"r.{u}[k] = (k & 1) ? b.{u}[k] : a.{u}[k + 1];",                       # TRN2
            0b011: f"r.{u}[k] = (k & 1) ? b.{u}[k >> 1] : a.{u}[k >> 1];",                 # ZIP1
            0b111: f"r.{u}[k] = (k & 1) ? b.{u}[(k >> 1) + {h}] : a.{u}[(k >> 1) + {h}];", # ZIP2
        }
        if opc not in bodies:
            raise Unsupported("SIMD: permute unallocated")
        return self.vloop(rd, e, bodies[opc], [("a", rn), ("b", rm)], q=q)

    # ------------------------------------------------------------------ three same
    def three_same(self, i, scalar):
        q, u_, size, opc = bit(i, 30), bit(i, 29), bits(i, 23, 22), bits(i, 15, 11)
        rd, rn, rm = bits(i, 4, 0), bits(i, 9, 5), bits(i, 20, 16)
        if opc >= 0b11000:
            return self.three_same_fp(i, scalar)
        if opc == 0b00011:                               # logic: AND BIC ORR ORN EOR BSL BIT BIF
            if scalar:
                raise Unsupported("SIMD: scalar logic unallocated")
            ops = {(0, 0): "a.d[k] & b.d[k]", (0, 1): "a.d[k] & ~b.d[k]", (0, 2): "a.d[k] | b.d[k]",
                   (0, 3): "a.d[k] | ~b.d[k]", (1, 0): "a.d[k] ^ b.d[k]",
                   (1, 1): "b.d[k] ^ ((b.d[k] ^ a.d[k]) & r.d[k])",      # BSL
                   (1, 2): "r.d[k] ^ ((r.d[k] ^ a.d[k]) & b.d[k])",      # BIT
                   (1, 3): "r.d[k] ^ ((r.d[k] ^ a.d[k]) & ~b.d[k])"}     # BIF
            acc = u_ == 1 and size > 0
            return self.vloop(rd, 2 if q else 1, f"r.d[k] = {ops[(u_, size)]};",
                              [("a", rn), ("b", rm)], acc=acc, q=q)
        if scalar and size != 3 and opc not in (0b00001, 0b00101, 0b01001, 0b01011, 0b10110):
            raise Unsupported("SIMD: scalar three-same needs 64-bit lanes")
        if size == 3 and not q and not scalar:
            raise Unsupported("SIMD: 1D vector unallocated")
        n = 1 if scalar else self.nlanes(q, size)
        es = 8 << size
        uf, sf, ut = UF[size], SF[size], UT[size]
        A = f"a.{sf}[k]" if not u_ else f"a.{uf}[k]"     # signedness from U
        B = f"b.{sf}[k]" if not u_ else f"b.{uf}[k]"
        wide = "(__int128)"
        sat = f"sat_u({wide}{A} {{op}} {B}, {es})" if u_ else f"(uint64_t)sat_s({wide}{A} {{op}} {B}, {es})"
        sh_b = f"(int)b.sb[k * {1 << size}]"             # shift amount: low byte of each lane
        acc = False
        if opc == 0b00000:   expr = f"({wide}{A} + {B}) >> 1"                          # SHADD/UHADD
        elif opc == 0b00010: expr = f"({wide}{A} + {B} + 1) >> 1"                      # SRHADD/URHADD
        elif opc == 0b00100: expr = f"({wide}{A} - {B}) >> 1"                          # SHSUB/UHSUB
        elif opc == 0b00001: expr = sat.format(op="+")                                 # SQADD/UQADD
        elif opc == 0b00101: expr = sat.format(op="-")                                 # SQSUB/UQSUB
        elif opc == 0b00110: expr = f"({A} > {B}) ? {ones(size)} : 0"                  # CMGT/CMHI
        elif opc == 0b00111: expr = f"({A} >= {B}) ? {ones(size)} : 0"                 # CMGE/CMHS
        elif opc in (0b01000, 0b01010):                                                # SSHL/USHL/SRSHL/URSHL
            expr = f"shift_gen({wide}{A}, {sh_b}, {1 if opc == 0b01010 else 0})"
        elif opc in (0b01001, 0b01011):                                                # SQSHL/UQSHL/SQRSHL/UQRSHL
            inner = f"shift_gen({wide}{A}, {sh_b}, {1 if opc == 0b01011 else 0})"
            expr = f"sat_u({inner}, {es})" if u_ else f"(uint64_t)sat_s({inner}, {es})"
        elif opc == 0b01100: expr = f"({A} > {B}) ? {A} : {B}"                         # SMAX/UMAX
        elif opc == 0b01101: expr = f"({A} < {B}) ? {A} : {B}"                         # SMIN/UMIN
        elif opc in (0b01110, 0b01111):                                                # SABD/UABD/SABA/UABA
            expr = f"({A} > {B}) ? ({wide}{A} - {B}) : ({wide}{B} - {A})"
            if opc == 0b01111:
                expr, acc = f"r.{uf}[k] + ({expr})", True
        elif opc == 0b10000: expr = f"a.{uf}[k] {'-' if u_ else '+'} b.{uf}[k]"        # ADD/SUB
        elif opc == 0b10001:                                                           # CMTST/CMEQ
            expr = (f"(a.{uf}[k] == b.{uf}[k]) ? {ones(size)} : 0" if u_
                    else f"(a.{uf}[k] & b.{uf}[k]) ? {ones(size)} : 0")
        elif opc == 0b10010 and size < 3:                                              # MLA/MLS
            expr, acc = f"r.{uf}[k] {'-' if u_ else '+'} ({ut})(a.{uf}[k] * b.{uf}[k])", True
        elif opc == 0b10011 and size < 3:                                              # MUL/PMUL
            if u_:
                if size:
                    raise Unsupported("SIMD: pmul unallocated")
                expr = "(uint8_t)pmull64(a.b[k], b.b[k])"
            else:
                expr = f"({ut})((uint64_t)a.{uf}[k] * b.{uf}[k])"
        elif opc in (0b10100, 0b10101, 0b10111) and size < 3 or (opc == 0b10111 and size == 3):
            return self.pairwise_int(i)
        elif opc == 0b10110 and size in (1, 2):                                        # SQDMULH/SQRDMULH
            rnd = f" + ((__int128)1 << {es - 1})" if u_ else ""
            expr = f"(uint64_t)sat_s(((__int128)2 * a.{sf}[k] * b.{sf}[k]{rnd}) >> {es}, {es})"
        else:
            raise Unsupported("SIMD: three-same opcode")
        return self.vloop(rd, n, f"r.{uf}[k] = ({ut})({expr});", [("a", rn), ("b", rm)],
                          acc=acc, q=q, zero=scalar)

    def pairwise_int(self, i):
        q, u_, size, opc = bit(i, 30), bit(i, 29), bits(i, 23, 22), bits(i, 15, 11)
        rd, rn, rm = bits(i, 4, 0), bits(i, 9, 5), bits(i, 20, 16)
        e, uf, sf, ut = self.nlanes(q, size), UF[size], SF[size], UT[size]
        f = uf if u_ else sf
        x, y = f"cat.{f}[2 * k]", f"cat.{f}[2 * k + 1]"
        expr = {0b10100: f"{x} > {y} ? {x} : {y}", 0b10101: f"{x} < {y} ? {x} : {y}",
                0b10111: f"cat.{uf}[2 * k] + cat.{uf}[2 * k + 1]"}[opc]
        if opc == 0b10111 and u_:
            raise Unsupported("SIMD: addp unallocated")
        # concatenate a and b into one lane array: lanes 0..e-1 from a, e..2e-1 from b
        body = f"r.{uf}[k] = ({ut})({expr.replace('cat.', 'C.')});"
        pre = (f"union {{ uint8_t b[32]; uint16_t h[16]; uint32_t s[8]; uint64_t d[4]; int8_t sb[32]; "
               f"int16_t sh[16]; int32_t sw[8]; int64_t sx[4]; }} C; memcpy(C.b, &a, {e << size}); "
               f"memcpy(C.b + {e << size}, &b, {e << size}); ")
        return self.vloop(rd, e, body, [("a", rn), ("b", rm)], q=q, pre=pre)

    def three_same_fp(self, i, scalar):
        q, u_, size, opc = bit(i, 30), bit(i, 29), bits(i, 23, 22), bits(i, 15, 11)
        rd, rn, rm = bits(i, 4, 0), bits(i, 9, 5), bits(i, 20, 16)
        a_ = size >> 1
        sz = 2 + (size & 1)
        if sz == 3 and not q and not scalar:
            raise Unsupported("SIMD: FP 1D unallocated")
        fF, s_, T, uf = ffield(sz), fsuf(sz), fty(sz), UF[sz]
        A, B, D = f"a.{fF}[k]", f"b.{fF}[k]", f"r.{fF}[k]"
        key = (opc, u_, a_)
        if key in ((0b11000, 1, 0), (0b11000, 1, 1), (0b11010, 1, 0), (0b11110, 1, 0), (0b11110, 1, 1)):
            if scalar:
                raise Unsupported("SIMD: scalar FP pairwise is a different encoding")
            return self.pairwise_fp(i)
        T_ONE = ones(sz)
        table = {
            (0b11000, 0, 0): (f"fmaxnm_{s_}({A}, {B})", False),
            (0b11000, 0, 1): (f"fminnm_{s_}({A}, {B})", False),
            (0b11001, 0, 0): (f"__builtin_fma{'f' if sz == 2 else ''}({A}, {B}, {D})", True),
            (0b11001, 0, 1): (f"__builtin_fma{'f' if sz == 2 else ''}(-{A}, {B}, {D})", True),
            (0b11010, 0, 0): (f"{A} + {B}", False),
            (0b11010, 0, 1): (f"{A} - {B}", False),
            (0b11010, 1, 1): (f"fabd_{s_}({A}, {B})", False),
            (0b11011, 0, 0): (f"fmulx_{s_}({A}, {B})", False),
            (0b11011, 1, 0): (f"{A} * {B}", False),
            (0b11110, 0, 0): (f"fmax_{s_}({A}, {B})", False),
            (0b11110, 0, 1): (f"fmin_{s_}({A}, {B})", False),
            (0b11111, 0, 0): (f"frecps_{s_}({A}, {B})", False),
            (0b11111, 0, 1): (f"frsqrts_{s_}({A}, {B})", False),
            (0b11111, 1, 0): (f"{A} / {B}", False),
        }
        cmps = {(0b11100, 0, 0): f"{A} == {B}", (0b11100, 1, 0): f"{A} >= {B}", (0b11100, 1, 1): f"{A} > {B}",
                (0b11101, 1, 0): f"__builtin_fabs{'f' if sz == 2 else ''}({A}) >= __builtin_fabs{'f' if sz == 2 else ''}({B})",
                (0b11101, 1, 1): f"__builtin_fabs{'f' if sz == 2 else ''}({A}) > __builtin_fabs{'f' if sz == 2 else ''}({B})"}
        n = 1 if scalar else self.nlanes(q, sz)
        if key in cmps:
            return self.vloop(rd, n, f"r.{uf}[k] = ({cmps[key]}) ? {T_ONE} : 0;", [("a", rn), ("b", rm)],
                              q=q, zero=scalar)
        if key not in table or (scalar and key in ((0b11000, 0, 0), (0b11000, 0, 1), (0b11001, 0, 0),
                                                   (0b11001, 0, 1), (0b11010, 0, 0), (0b11010, 0, 1),
                                                   (0b11011, 1, 0), (0b11110, 0, 0), (0b11110, 0, 1),
                                                   (0b11111, 1, 0))):
            raise Unsupported("SIMD: FP three-same opcode")
        expr, acc = table[key]
        return self.vloop(rd, n, f"r.{fF}[k] = ({T})({expr});", [("a", rn), ("b", rm)],
                          acc=acc, q=q, zero=scalar)

    def pairwise_fp(self, i):
        q, u_, size, opc = bit(i, 30), bit(i, 29), bits(i, 23, 22), bits(i, 15, 11)
        rd, rn, rm = bits(i, 4, 0), bits(i, 9, 5), bits(i, 20, 16)
        a_, sz = size >> 1, 2 + (size & 1)
        fF, s_, T = ffield(sz), fsuf(sz), fty(sz)
        e = self.nlanes(q, sz)
        fn = {(0b11000, 0): f"fmaxnm_{s_}", (0b11000, 1): f"fminnm_{s_}", (0b11010, 0): None,
              (0b11110, 0): f"fmax_{s_}", (0b11110, 1): f"fmin_{s_}"}[(opc, a_)]
        x, y = f"C[2 * k]", f"C[2 * k + 1]"
        expr = f"{x} + {y}" if fn is None else f"{fn}({x}, {y})"
        pre = f"{T} C[{2 * e}]; memcpy(C, &a, {4 * e if sz == 2 else 8 * e}); memcpy(C + {e}, &b, {4 * e if sz == 2 else 8 * e}); "
        return self.vloop(rd, e, f"r.{fF}[k] = {expr};", [("a", rn), ("b", rm)], q=q, pre=pre)

    # ------------------------------------------------------------------ three different (long / wide / narrow)
    def three_diff(self, i):
        q, u_, size, opc = bit(i, 30), bit(i, 29), bits(i, 23, 22), bits(i, 15, 12)
        rd, rn, rm = bits(i, 4, 0), bits(i, 9, 5), bits(i, 20, 16)
        if opc == 0b1110:                                # PMULL / PMULL2
            if size == 0:
                return self.vloop(rd, 8, f"r.h[k] = (uint16_t)pmull64(a.b[k + {8 * q}], b.b[k + {8 * q}]);",
                                  [("a", rn), ("b", rm)])
            if size == 3:
                return [f"{{ unsigned __int128 p = pmull64(c->V[{rn}].d[{q}], c->V[{rm}].d[{q}]); "
                        f"c->V[{rd}].d[0] = (uint64_t)p; c->V[{rd}].d[1] = (uint64_t)(p >> 64); }}"]
            raise Unsupported("SIMD: pmull unallocated")
        if size == 3:
            raise Unsupported("SIMD: three-different size 3")
        n = 8 >> size                                    # result lanes (wide)
        off = n * q                                      # source lanes start: upper half for the "2" forms
        nf = UF[size] if u_ else SF[size]                # narrow source field (signedness from U)
        wf, wt = UF[size + 1], UT[size + 1]
        wsf = SF[size + 1] if not u_ else UF[size + 1]
        es = 8 << size
        A, B = f"(__int128)a.{nf}[k + {off}]", f"(__int128)b.{nf}[k + {off}]"
        Aw = f"(__int128)a.{wsf}[k]"
        acc = False
        if opc in (0b0100, 0b0110):                      # ADDHN/RADDHN/SUBHN/RSUBHN: narrow high half
            rnd = f" + ((__int128)1 << {es - 1})" if u_ else ""
            sign = "+" if opc == 0b0100 else "-"
            body = (f"r.{UF[size]}[k + {off}] = ({UT[size]})(((__int128)a.{wf}[k] {sign} "
                    f"(__int128)b.{wf}[k]{rnd}) >> {es});")
            if q:     # "2" form: write the upper half, keep the lower
                return self.vloop(rd, n, body, [("a", rn), ("b", rm)], acc=True)
            return self.vloop(rd, n, body, [("a", rn), ("b", rm)], q=0)
        exprs = {
            0b0000: f"{A} + {B}", 0b0010: f"{A} - {B}",                       # SADDL/UADDL, SSUBL/USUBL
            0b0001: f"{Aw} + {B}", 0b0011: f"{Aw} - {B}",                     # SADDW, SSUBW
            0b0111: f"{A} > {B} ? {A} - {B} : {B} - {A}",                     # SABDL
            0b1100: f"{A} * {B}",                                              # SMULL/UMULL
        }
        if opc in exprs:
            expr = exprs[opc]
        elif opc == 0b0101:                                                    # SABAL
            expr, acc = f"(__int128)r.{wf}[k] + ({A} > {B} ? {A} - {B} : {B} - {A})", True
        elif opc in (0b1000, 0b1010):                                          # SMLAL / SMLSL
            expr, acc = f"(__int128)r.{wf}[k] {'+' if opc == 0b1000 else '-'} {A} * {B}", True
        elif not u_ and opc in (0b1001, 0b1011, 0b1101) and size in (1, 2):   # SQDMLAL/SQDMLSL/SQDMULL
            prod = f"sat_s(2 * {A} * {B}, {2 * es})"
            if opc == 0b1101:
                expr = f"(uint64_t){prod}"
            else:
                expr = f"(uint64_t)sat_s((__int128)r.{SF[size + 1]}[k] {'+' if opc == 0b1001 else '-'} {prod}, {2 * es})"
                acc = True
        else:
            raise Unsupported("SIMD: three-different opcode")
        return self.vloop(rd, n, f"r.{wf}[k] = ({wt})({expr});", [("a", rn), ("b", rm)], acc=acc)

    # ------------------------------------------------------------------ two-register misc
    def two_misc(self, i, scalar):
        q, u_, size, opc = bit(i, 30), bit(i, 29), bits(i, 23, 22), bits(i, 16, 12)
        rd, rn = bits(i, 4, 0), bits(i, 9, 5)
        if opc >= 0b01100 and opc not in (0b10010, 0b10011, 0b10100):
            return self.two_misc_fp(i, scalar)
        if opc in (0b10010, 0b10100, 0b10011) or (opc == 0b10110):
            return self.narrow_misc(i, scalar)
        if size == 3 and not q and not scalar:
            raise Unsupported("SIMD: 1D unallocated")
        if scalar and opc not in (0b00011, 0b00111, 0b01000, 0b01001, 0b01010, 0b01011):
            raise Unsupported("SIMD: scalar two-reg misc opcode")
        if scalar and size != 3 and opc in (0b01000, 0b01001, 0b01010, 0b01011):
            raise Unsupported("SIMD: scalar compare needs D")
        n = 1 if scalar else self.nlanes(q, size)
        uf, sf, ut, es = UF[size], SF[size], UT[size], 8 << size
        A = f"a.{sf}[k]"
        acc = False
        if opc == 0b00000:                               # REV64 / REV32
            cont = 64 if not u_ else 32
            if es >= cont:
                raise Unsupported("SIMD: rev unallocated")
            per = cont // es
            return self.vloop(rd, n, f"r.{uf}[k] = a.{uf}[(k / {per}) * {per} + ({per} - 1 - k % {per})];",
                              [("a", rn)], q=q)
        if opc == 0b00001 and not u_:                    # REV16
            if size:
                raise Unsupported("SIMD: rev16 unallocated")
            return self.vloop(rd, n, "r.b[k] = a.b[k ^ 1];", [("a", rn)], q=q)
        if opc in (0b00010, 0b00110):                    # SADDLP/UADDLP, SADALP/UADALP
            if size == 3:
                raise Unsupported("SIMD: addlp unallocated")
            f = uf if u_ else sf
            wf, wt = UF[size + 1], UT[size + 1]
            add = f"r.{wf}[k] + " if opc == 0b00110 else ""
            return self.vloop(rd, n // 2, f"r.{wf}[k] = ({wt})({add}(int64_t)a.{f}[2 * k] + a.{f}[2 * k + 1]);",
                              [("a", rn)], acc=opc == 0b00110, q=q)
        if opc == 0b00011:                               # SUQADD / USQADD
            if u_:
                expr = f"sat_u((__int128)r.{uf}[k] + a.{sf}[k], {es})"
            else:
                expr = f"(uint64_t)sat_s((__int128)r.{sf}[k] + a.{uf}[k], {es})"
            return self.vloop(rd, n, f"r.{uf}[k] = ({ut})({expr});", [("a", rn)], acc=True, q=q, zero=scalar)
        if opc == 0b00100:                               # CLS / CLZ
            if size == 3:
                raise Unsupported("SIMD: clz unallocated")
            expr = f"clz_n(a.{uf}[k], {es})" if u_ else f"cls_n(a.{uf}[k], {es})"
            return self.vloop(rd, n, f"r.{uf}[k] = ({ut}){expr};", [("a", rn)], q=q)
        if opc == 0b00101:                               # CNT / NOT / RBIT
            if not u_ and size == 0:
                body = "r.b[k] = (uint8_t)popcount8(a.b[k]);"
            elif u_ and size == 0:
                body = "r.b[k] = (uint8_t)~a.b[k];"
            elif u_ and size == 1:
                body = "r.b[k] = rbit8(a.b[k]);"
            else:
                raise Unsupported("SIMD: cnt/not/rbit unallocated")
            return self.vloop(rd, 16 if q else 8, body, [("a", rn)], q=q)
        if opc == 0b00111:                               # SQABS / SQNEG
            expr = f"sat_s(-(__int128){A}, {es})" if u_ else f"sat_s({A} < 0 ? -(__int128){A} : (__int128){A}, {es})"
        elif opc == 0b01000:   expr = f"({A} {'>=' if u_ else '>'} 0) ? {ones(size)} : 0"   # CMGE/CMGT #0
        elif opc == 0b01001:   expr = f"({A} {'<=' if u_ else '=='} 0) ? {ones(size)} : 0"  # CMLE/CMEQ #0
        elif opc == 0b01010 and not u_: expr = f"({A} < 0) ? {ones(size)} : 0"             # CMLT #0
        elif opc == 0b01011:                                                                # ABS / NEG
            expr = f"0 - a.{uf}[k]" if u_ else f"{A} < 0 ? 0 - a.{uf}[k] : a.{uf}[k]"
        else:
            raise Unsupported("SIMD: two-reg misc opcode")
        return self.vloop(rd, n, f"r.{uf}[k] = ({ut})({expr});", [("a", rn)], q=q, acc=acc, zero=scalar)

    def narrow_misc(self, i, scalar):
        """XTN, SQXTN, UQXTN, SQXTUN, SHLL, FCVTN, FCVTL, FCVTXN (+ the "2" forms)."""
        q, u_, size, opc = bit(i, 30), bit(i, 29), bits(i, 23, 22), bits(i, 16, 12)
        rd, rn = bits(i, 4, 0), bits(i, 9, 5)
        if opc == 0b10011:                               # SHLL / SHLL2
            if not u_ or size == 3 or scalar:
                raise Unsupported("SIMD: shll unallocated")
            n, off = 8 >> size, (8 >> size) * q
            return self.vloop(rd, n, f"r.{UF[size + 1]}[k] = ({UT[size + 1]})((uint64_t)a.{UF[size]}[k + {off}] << {8 << size});",
                              [("a", rn)])
        if opc == 0b10110:
            raise Unsupported("SIMD: fcvtn/fcvtxn")   # handled in two_misc_fp
        if size == 3:
            raise Unsupported("SIMD: narrow unallocated")
        n = 1 if scalar else 8 >> size
        off = 0 if scalar else n * q
        nf, nt, es = UF[size], UT[size], 8 << size
        src_s, src_u = f"a.{SF[size + 1]}[k]", f"a.{UF[size + 1]}[k]"
        if opc == 0b10010 and not u_:   expr = src_u                                 # XTN
        elif opc == 0b10010 and u_:     expr = f"sat_u({src_s}, {es})"               # SQXTUN
        elif opc == 0b10100 and not u_: expr = f"sat_s({src_s}, {es})"               # SQXTN
        elif opc == 0b10100 and u_:     expr = f"sat_u({src_u}, {es})"               # UQXTN
        else:
            raise Unsupported("SIMD: narrow opcode")
        if scalar and opc == 0b10010 and not u_:
            raise Unsupported("SIMD: scalar xtn unallocated")
        body = f"r.{nf}[k + {off}] = ({nt})({expr});"
        if scalar:
            return self.vloop(rd, 1, body, [("a", rn)], zero=True)
        if q:
            return self.vloop(rd, n, body, [("a", rn)], acc=True)
        return self.vloop(rd, n, body, [("a", rn)], q=0)

    def two_misc_fp(self, i, scalar):
        q, u_, size, opc = bit(i, 30), bit(i, 29), bits(i, 23, 22), bits(i, 16, 12)
        rd, rn = bits(i, 4, 0), bits(i, 9, 5)
        a_, sz = size >> 1, 2 + (size & 1)
        if opc in (0b10110, 0b10111) and a_ == 0:       # FCVTN / FCVTXN / FCVTL (precision change)
            return self.fcvt_long_narrow(i, scalar)
        if sz == 3 and not q and not scalar:
            raise Unsupported("SIMD: FP 1D unallocated")
        n = 1 if scalar else self.nlanes(q, sz)
        fF, s_, T, uf, ut = ffield(sz), fsuf(sz), fty(sz), UF[sz], UT[sz]
        f_ = "f" if sz == 2 else ""
        A = f"a.{fF}[k]"
        val = f"(double){A}" if sz == 2 else A
        nb = 8 << sz
        key = (opc, u_, a_)
        fexpr = {   # result is a float
            (0b11000, 0, 0): f"fround{f_}({A}, 0)", (0b11001, 0, 0): f"fround{f_}({A}, 2)",
            (0b11000, 0, 1): f"fround{f_}({A}, 1)", (0b11001, 0, 1): f"fround{f_}({A}, 3)",
            (0b11000, 1, 0): f"fround{f_}({A}, 4)", (0b11001, 1, 0): f"fround{f_}({A}, 5)",
            (0b11001, 1, 1): f"fround{f_}({A}, 5)",
            (0b11101, 0, 0): f"({T})(a.{SF[sz]}[k])",                 # SCVTF
            (0b11101, 1, 0): f"({T})(a.{uf}[k])",                     # UCVTF
            (0b11101, 0, 1): f"frecpe_{s_}({A})",
            (0b11101, 1, 1): f"frsqrte_{s_}({A})",
            (0b11111, 1, 1): f"__builtin_sqrt{f_}({A})",
        }
        iexpr = {   # result is an integer (bits)
            (0b11010, 0, 0): f"fcvt_s({val}, 0, {nb})", (0b11010, 1, 0): f"fcvt_u({val}, 0, {nb})",
            (0b11011, 0, 0): f"fcvt_s({val}, 2, {nb})", (0b11011, 1, 0): f"fcvt_u({val}, 2, {nb})",
            (0b11100, 0, 0): f"fcvt_s({val}, 4, {nb})", (0b11100, 1, 0): f"fcvt_u({val}, 4, {nb})",
            (0b11010, 0, 1): f"fcvt_s({val}, 1, {nb})", (0b11010, 1, 1): f"fcvt_u({val}, 1, {nb})",
            (0b11011, 0, 1): f"fcvt_s({val}, 3, {nb})", (0b11011, 1, 1): f"fcvt_u({val}, 3, {nb})",
            (0b01100, 0, 1): f"({A} > 0) ? {ones(sz)} : 0", (0b01100, 1, 1): f"({A} >= 0) ? {ones(sz)} : 0",
            (0b01101, 0, 1): f"({A} == 0) ? {ones(sz)} : 0", (0b01101, 1, 1): f"({A} <= 0) ? {ones(sz)} : 0",
            (0b01110, 0, 1): f"({A} < 0) ? {ones(sz)} : 0",
            (0b01111, 0, 1): f"a.{uf}[k] & ~(1ull << {nb - 1})",     # FABS
            (0b01111, 1, 1): f"a.{uf}[k] ^ (1ull << {nb - 1})",      # FNEG
        }
        if scalar and key == (0b11111, 0, 1):            # FRECPX (scalar only)
            iexpr[key] = f"frecpx_bits(a.{uf}[k], {nb})"
        if key in ((0b11100, 0, 1), (0b11100, 1, 1)):    # URECPE / URSQRTE
            if sz != 2 or scalar:
                raise Unsupported("SIMD: urecpe unallocated")
            fn = "urecpe" if not u_ else "ursqrte"
            return self.vloop(rd, n, f"r.s[k] = {fn}(a.s[k]);", [("a", rn)], q=q)
        scalar_ok = {(0b11010, 0, 0), (0b11010, 1, 0), (0b11011, 0, 0), (0b11011, 1, 0), (0b11100, 0, 0),
                     (0b11100, 1, 0), (0b11010, 0, 1), (0b11010, 1, 1), (0b11011, 0, 1), (0b11011, 1, 1),
                     (0b11101, 0, 0), (0b11101, 1, 0), (0b11101, 0, 1), (0b11101, 1, 1), (0b11111, 0, 1),
                     (0b01100, 0, 1), (0b01100, 1, 1), (0b01101, 0, 1), (0b01101, 1, 1), (0b01110, 0, 1)}
        if scalar and key not in scalar_ok:
            raise Unsupported("SIMD: scalar FP two-reg misc opcode")
        if key in fexpr:
            return self.vloop(rd, n, f"r.{fF}[k] = {fexpr[key]};", [("a", rn)], q=q, zero=scalar)
        if key in iexpr:
            return self.vloop(rd, n, f"r.{uf}[k] = ({ut})({iexpr[key]});", [("a", rn)], q=q, zero=scalar)
        raise Unsupported("SIMD: FP two-reg misc opcode")

    def fcvt_long_narrow(self, i, scalar):
        q, u_, size, opc = bit(i, 30), bit(i, 29), bits(i, 23, 22), bits(i, 16, 12)
        rd, rn = bits(i, 4, 0), bits(i, 9, 5)
        sz = size & 1                                    # 0: half<->single, 1: single<->double
        if scalar or (opc == 0b10110 and u_):
            raise Unsupported("SIMD: FCVTXN (round to odd) not supported yet")
        if opc == 0b10111:                               # FCVTL / FCVTL2: widen
            if u_:
                raise Unsupported("SIMD: fcvtl unallocated")
            if sz:
                return self.vloop(rd, 2, f"r.fd[k] = (double)a.f[k + {2 * q}];", [("a", rn)])
            return self.vloop(rd, 4, f"r.f[k] = (float)f16_to_f64(a.h[k + {4 * q}]);", [("a", rn)])
        # FCVTN / FCVTN2: narrow
        if sz:
            body, n, f = f"r.f[k + {2 * q}] = (float)a.fd[k];", 2, "f"
        else:
            body, n, f = f"r.h[k + {4 * q}] = f64_to_f16((double)a.f[k]);", 4, "h"
        if q:
            return self.vloop(rd, n, body, [("a", rn)], acc=True)
        return self.vloop(rd, n, body, [("a", rn)], q=0)

    # ------------------------------------------------------------------ across lanes / scalar pairwise
    def across(self, i):
        q, u_, size, opc = bit(i, 30), bit(i, 29), bits(i, 23, 22), bits(i, 16, 12)
        rd, rn = bits(i, 4, 0), bits(i, 9, 5)
        if opc in (0b01100, 0b01111):                    # FMAXNMV/FMINNMV/FMAXV/FMINV (4S only)
            if not u_ or not q or size & 1:
                raise Unsupported("SIMD: FP across-lanes unallocated / FP16")
            fn = {(0b01100, 0): "fmaxnm_s", (0b01100, 1): "fminnm_s",
                  (0b01111, 0): "fmax_s", (0b01111, 1): "fmin_s"}[(opc, size >> 1)]
            # Arm reduces in pairs: op(op(e0, e1), op(e2, e3))
            return [f"{{ V128 a = c->V[{rn}]; float v = {fn}({fn}(a.f[0], a.f[1]), {fn}(a.f[2], a.f[3])); "
                    f"set_s(c, {rd}, v); }}"]
        if size == 3 or (size == 2 and not q):
            raise Unsupported("SIMD: across-lanes unallocated")
        e, uf, sf, es = self.nlanes(q, size), UF[size], SF[size], 8 << size
        f = uf if u_ else sf
        if opc == 0b00011:                               # SADDLV / UADDLV
            return [f"{{ V128 a = c->V[{rn}]; int64_t s = 0; for (int k = 0; k < {e}; k++) s += a.{f}[k]; "
                    f"set_vbits(c, {rd}, (uint64_t)s & {hx(mask(2 * es))}, {2 * es // 8}); }}"]
        if opc in (0b01010, 0b11010):                    # SMAXV/UMAXV/SMINV/UMINV
            cmp = ">" if opc == 0b01010 else "<"
            return [f"{{ V128 a = c->V[{rn}]; {ST[size] if not u_ else UT[size]} m = a.{f}[0]; "
                    f"for (int k = 1; k < {e}; k++) if (a.{f}[k] {cmp} m) m = a.{f}[k]; "
                    f"set_vbits(c, {rd}, ({UT[size]})m, {es // 8}); }}"]
        if opc == 0b11011 and not u_:                    # ADDV
            return [f"{{ V128 a = c->V[{rn}]; uint64_t s = 0; for (int k = 0; k < {e}; k++) s += a.{uf}[k]; "
                    f"set_vbits(c, {rd}, s & {hx(mask(es))}, {es // 8}); }}"]
        raise Unsupported("SIMD: across-lanes opcode")

    def pairwise_scalar(self, i):
        u_, size, opc = bit(i, 29), bits(i, 23, 22), bits(i, 16, 12)
        rd, rn = bits(i, 4, 0), bits(i, 9, 5)
        if opc == 0b11011 and not u_ and size == 3:      # ADDP Dd, Vn.2D
            return [f"set_dbits(c, {rd}, c->V[{rn}].d[0] + c->V[{rn}].d[1]);"]
        if not u_:
            raise Unsupported("SIMD: scalar pairwise FP16")
        sz = 2 + (size & 1)
        s_, fF = fsuf(sz), ffield(sz)
        fn = {(0b01100, 0): f"fmaxnm_{s_}", (0b01100, 1): f"fminnm_{s_}", (0b01101, 0): None,
              (0b01111, 0): f"fmax_{s_}", (0b01111, 1): f"fmin_{s_}"}.get((opc, size >> 1), "x")
        if fn == "x":
            raise Unsupported("SIMD: scalar pairwise opcode")
        x, y = f"c->V[{rn}].{fF}[0]", f"c->V[{rn}].{fF}[1]"
        return [f"set_{s_}(c, {rd}, {f'{x} + {y}' if fn is None else f'{fn}({x}, {y})'});"]

    # ------------------------------------------------------------------ modified immediate: MOVI/MVNI/ORR/BIC/FMOV
    def mod_imm(self, i):
        q, op, cmode, o2 = bit(i, 30), bit(i, 29), bits(i, 15, 12), bit(i, 11)
        rd = bits(i, 4, 0)
        imm8 = (bits(i, 18, 16) << 5) | bits(i, 9, 5)
        if o2:
            raise Unsupported("SIMD: FMOV FP16 immediate")
        c3 = cmode >> 1
        if c3 == 0:   imm = imm8 * 0x0000000100000001
        elif c3 == 1: imm = (imm8 << 8) * 0x0000000100000001
        elif c3 == 2: imm = (imm8 << 16) * 0x0000000100000001
        elif c3 == 3: imm = (imm8 << 24) * 0x0000000100000001
        elif c3 == 4: imm = imm8 * 0x0001000100010001
        elif c3 == 5: imm = (imm8 << 8) * 0x0001000100010001
        elif c3 == 6:
            v = ((imm8 << 8) | 0xFF) if not (cmode & 1) else ((imm8 << 16) | 0xFFFF)
            imm = v * 0x0000000100000001
        else:
            if not (cmode & 1):
                if not op:
                    imm = imm8 * 0x0101010101010101
                else:
                    imm = sum((0xFF << (8 * k)) for k in range(8) if (imm8 >> k) & 1)
            else:
                b6 = bit(imm8, 6)
                if not op:      # FMOV Vd.4S/2S, #imm
                    e32 = ((b6 ^ 1) << 7) | ((0x1F if b6 else 0) << 2) | bits(imm8, 5, 4)
                    f32 = (bit(imm8, 7) << 31) | (e32 << 23) | (bits(imm8, 3, 0) << 19)
                    imm = f32 * 0x0000000100000001
                else:           # FMOV Vd.2D, #imm
                    if not q:
                        raise Unsupported("SIMD: fmov 1D unallocated")
                    e64 = ((b6 ^ 1) << 10) | ((0xFF if b6 else 0) << 2) | bits(imm8, 5, 4)
                    imm = (bit(imm8, 7) << 63) | (e64 << 52) | (bits(imm8, 3, 0) << 48)
        imm &= (1 << 64) - 1
        if cmode in (0b0001, 0b0011, 0b0101, 0b0111, 0b1001, 0b1011):          # ORR / BIC
            expr = f"r.d[k] | {hx(imm)}" if not op else f"r.d[k] & {hx(~imm)}"
            return self.vloop(rd, 2 if q else 1, f"r.d[k] = {expr};", acc=True, q=q)
        mvn = op and cmode != 0b1110 and cmode != 0b1111                       # MVNI
        val = (~imm) & ((1 << 64) - 1) if mvn else imm
        return self.vloop(rd, 2 if q else 1, f"r.d[k] = {hx(val)};", q=q)

    # ------------------------------------------------------------------ shift by immediate
    def shift_imm(self, i, scalar):
        q, u_, immh, immb, opc = bit(i, 30), bit(i, 29), bits(i, 22, 19), bits(i, 18, 16), bits(i, 15, 11)
        rd, rn = bits(i, 4, 0), bits(i, 9, 5)
        size = immh.bit_length() - 1                     # lane size from the highest set bit of immh
        es = 8 << size
        immhb = (immh << 3) | immb
        rshift, lshift = 2 * es - immhb, immhb - es
        if opc in (0b10000, 0b10001, 0b10010, 0b10011, 0b10100):   # narrowing / lengthening
            return self.shift_narrow_long(i, scalar)
        if opc in (0b11100, 0b11111):                    # SCVTF/UCVTF/FCVTZS/FCVTZU (fixed point)
            if size < 2:
                raise Unsupported("SIMD: fixed-point FP16")
            if size == 3 and not q and not scalar:
                raise Unsupported("SIMD: 1D unallocated")
            n = 1 if scalar else self.nlanes(q, size)
            fbits = rshift
            fF, T, uf, ut = ffield(size), fty(size), UF[size], UT[size]
            if opc == 0b11100:
                src = f"a.{uf}[k]" if u_ else f"a.{SF[size]}[k]"
                body = f"r.{fF}[k] = ({T})(({T})({src}) * {float(2.0 ** -fbits).hex()});"
            else:
                val = f"(double)a.{fF}[k] * {float(2 ** fbits).hex()}"
                body = f"r.{uf}[k] = ({ut})fcvt_{'u' if u_ else 's'}({val}, 3, {es});"
            return self.vloop(rd, n, body, [("a", rn)], q=q, zero=scalar)
        if size == 3 and not q and not scalar:
            raise Unsupported("SIMD: 1D unallocated")
        if scalar and size != 3 and opc not in (0b01110, 0b01100):
            raise Unsupported("SIMD: scalar shift needs D")
        n = 1 if scalar else self.nlanes(q, size)
        uf, sf, ut = UF[size], SF[size], UT[size]
        A = f"(__int128)a.{uf if u_ else sf}[k]"
        acc = False
        if opc in (0b00000, 0b00010, 0b00100, 0b00110):  # SSHR/USHR/SSRA/USRA/SRSHR/URSHR/SRSRA/URSRA
            expr = f"shift_gen({A}, -{rshift}, {1 if opc in (0b00100, 0b00110) else 0})"
            if opc in (0b00010, 0b00110):
                expr, acc = f"r.{uf}[k] + ({ut})({expr})", True
        elif opc == 0b01000 and u_:                      # SRI
            m = mask(es) >> rshift if rshift < es else 0
            expr, acc = f"(r.{uf}[k] & {hx(~m & mask(es))}) | ((uint64_t)a.{uf}[k] >> {rshift} & {hx(m)})" \
                if rshift < es else f"r.{uf}[k]", True
        elif opc == 0b01010:                             # SHL / SLI
            if u_:
                m = (mask(es) << lshift) & mask(es)
                expr, acc = f"(r.{uf}[k] & {hx(~m & mask(es))}) | (((uint64_t)a.{uf}[k] << {lshift}) & {hx(m)})", True
            else:
                expr = f"(uint64_t)a.{uf}[k] << {lshift}"
        elif opc == 0b01100 and u_:                      # SQSHLU
            expr = f"sat_u((__int128)a.{sf}[k] << {lshift}, {es})"
        elif opc == 0b01110:                             # SQSHL / UQSHL (immediate)
            expr = (f"sat_u((__int128)a.{uf}[k] << {lshift}, {es})" if u_
                    else f"(uint64_t)sat_s((__int128)a.{sf}[k] << {lshift}, {es})")
        else:
            raise Unsupported("SIMD: shift-immediate opcode")
        return self.vloop(rd, n, f"r.{uf}[k] = ({ut})({expr});", [("a", rn)], acc=acc, q=q, zero=scalar)

    def shift_narrow_long(self, i, scalar):
        q, u_, immh, immb, opc = bit(i, 30), bit(i, 29), bits(i, 22, 19), bits(i, 18, 16), bits(i, 15, 11)
        rd, rn = bits(i, 4, 0), bits(i, 9, 5)
        size = immh.bit_length() - 1
        es = 8 << size
        immhb = (immh << 3) | immb
        if opc == 0b10100:                               # SSHLL / USHLL (SXTL/UXTL)
            if size == 3 or scalar:
                raise Unsupported("SIMD: shll unallocated")
            sh = immhb - es
            n, off = 8 >> size, (8 >> size) * q
            src = f"a.{UF[size] if u_ else SF[size]}[k + {off}]"
            return self.vloop(rd, n, f"r.{UF[size + 1]}[k] = ({UT[size + 1]})((int64_t){src} << {sh});",
                              [("a", rn)])
        if size == 3:
            raise Unsupported("SIMD: narrow shift unallocated")
        sh = 2 * es - immhb                              # right shift amount, 1..es
        n = 1 if scalar else 8 >> size
        off = 0 if scalar else n * q
        wsrc_s, wsrc_u = f"(__int128)a.{SF[size + 1]}[k]", f"(__int128)a.{UF[size + 1]}[k]"
        rnd = opc in (0b10001, 0b10011)
        if opc in (0b10000, 0b10001):
            if not u_:                                   # SHRN / RSHRN
                expr = f"shift_gen({wsrc_u}, -{sh}, {int(rnd)})"
            else:                                        # SQSHRUN / SQRSHRUN
                expr = f"sat_u(shift_gen({wsrc_s}, -{sh}, {int(rnd)}), {es})"
        else:                                            # SQSHRN / SQRSHRN / UQSHRN / UQRSHRN
            if u_:
                expr = f"sat_u(shift_gen({wsrc_u}, -{sh}, {int(rnd)}), {es})"
            else:
                expr = f"sat_s(shift_gen({wsrc_s}, -{sh}, {int(rnd)}), {es})"
        if scalar and opc in (0b10000, 0b10001) and not u_:
            raise Unsupported("SIMD: scalar shrn unallocated")
        body = f"r.{UF[size]}[k + {off}] = ({UT[size]})({expr});"
        if scalar:
            return self.vloop(rd, 1, body, [("a", rn)], zero=True)
        if q:
            return self.vloop(rd, n, body, [("a", rn)], acc=True)
        return self.vloop(rd, n, body, [("a", rn)], q=0)

    # ------------------------------------------------------------------ by element: FMUL V0.4S, V1.4S, V2.S[1] ...
    def by_element(self, i, scalar):
        q, u_, size, opc = bit(i, 30), bit(i, 29), bits(i, 23, 22), bits(i, 15, 12)
        L, M, H = bit(i, 21), bit(i, 20), bit(i, 11)
        rd, rn = bits(i, 4, 0), bits(i, 9, 5)
        fp = opc in (0b0001, 0b0101, 0b1001) and not (u_ and opc != 0b1001)
        if fp:
            if size < 2:
                raise Unsupported("SIMD: FP16 by element")
            sz = size
            if sz == 2:
                idx, rm = (H << 1) | L, (M << 4) | bits(i, 19, 16)
            else:
                if L:
                    raise Unsupported("SIMD: by element unallocated")
                idx, rm = H, (M << 4) | bits(i, 19, 16)
            if sz == 3 and not q and not scalar:
                raise Unsupported("SIMD: 1D unallocated")
            n = 1 if scalar else self.nlanes(q, sz)
            fF, s_, T = ffield(sz), fsuf(sz), fty(sz)
            pre = f"{T} e = c->V[{rm}].{fF}[{idx}]; "
            A, D = f"a.{fF}[k]", f"r.{fF}[k]"
            f_ = "f" if sz == 2 else ""
            expr = {(0b0001, 0): f"__builtin_fma{f_}({A}, e, {D})",
                    (0b0101, 0): f"__builtin_fma{f_}(-{A}, e, {D})",
                    (0b1001, 0): f"{A} * e", (0b1001, 1): f"fmulx_{s_}({A}, e)"}[(opc, u_)]
            acc = opc in (0b0001, 0b0101)
            return self.vloop(rd, n, f"r.{fF}[k] = {expr};", [("a", rn)], acc=acc, q=q, zero=scalar, pre=pre)
        # integer
        if size == 1:
            idx, rm = (H << 2) | (L << 1) | M, bits(i, 19, 16)
        elif size == 2:
            idx, rm = (H << 1) | L, (M << 4) | bits(i, 19, 16)
        else:
            raise Unsupported("SIMD: by element size")
        uf, sf, ut, es = UF[size], SF[size], UT[size], 8 << size
        if opc in (0b0000, 0b0100, 0b1000):              # MLA / MLS / MUL (same width)
            if scalar or (opc != 0b1000 and not u_) or (opc == 0b1000 and u_):
                raise Unsupported("SIMD: by element unallocated")
            n = self.nlanes(q, size)
            pre = f"{ut} e = c->V[{rm}].{uf}[{idx}]; "
            if opc == 0b1000:
                return self.vloop(rd, n, f"r.{uf}[k] = ({ut})((uint64_t)a.{uf}[k] * e);", [("a", rn)], q=q, pre=pre)
            sign = "+" if opc == 0b0000 else "-"
            return self.vloop(rd, n, f"r.{uf}[k] = ({ut})(r.{uf}[k] {sign} (uint64_t)a.{uf}[k] * e);",
                              [("a", rn)], acc=True, q=q, pre=pre)
        if opc in (0b1100, 0b1101):                      # SQDMULH / SQRDMULH
            n = 1 if scalar else self.nlanes(q, size)
            pre = f"int64_t e = c->V[{rm}].{sf}[{idx}]; "
            rnd = f" + ((__int128)1 << {es - 1})" if opc == 0b1101 else ""
            return self.vloop(rd, n, f"r.{uf}[k] = ({ut})sat_s(((__int128)2 * a.{sf}[k] * e{rnd}) >> {es}, {es});",
                              [("a", rn)], q=q, zero=scalar, pre=pre)
        # long forms: SMULL/UMULL, SMLAL/UMLAL, SMLSL/UMLSL, SQDMULL/SQDMLAL/SQDMLSL (by element)
        n = 1 if scalar else 8 >> size
        off = 0 if scalar else n * q
        f = uf if u_ else sf
        wf, wt, wsf = UF[size + 1], UT[size + 1], SF[size + 1]
        pre = f"__int128 e = c->V[{rm}].{f}[{idx}]; "
        A = f"(__int128)a.{f}[k + {off}]"
        if opc == 0b1010:
            expr, acc = f"{A} * e", False
        elif opc in (0b0010, 0b0110):
            expr, acc = f"(__int128)r.{wf}[k] {'+' if opc == 0b0010 else '-'} {A} * e", True
        elif not u_ and opc in (0b1011, 0b0011, 0b0111):
            prod = f"sat_s(2 * {A} * e, {2 * es})"
            if opc == 0b1011:
                expr, acc = f"(uint64_t){prod}", False
            else:
                expr, acc = f"(uint64_t)sat_s((__int128)r.{wsf}[k] {'+' if opc == 0b0011 else '-'} {prod}, {2 * es})", True
        else:
            raise Unsupported("SIMD: by element opcode")
        if scalar and opc in (0b1010, 0b0010, 0b0110):
            raise Unsupported("SIMD: scalar by element unallocated")
        return self.vloop(rd, n, f"r.{wf}[k] = ({wt})({expr});", [("a", rn)], acc=acc, pre=pre, zero=scalar)

    # ------------------------------------------------------------------ LD1..LD4 / ST1..ST4 structures
    def ldst_struct(self, i):
        q, load, rn, rt = bit(i, 30), bit(i, 22), bits(i, 9, 5), bits(i, 4, 0)
        post, rm = bit(i, 23), bits(i, 20, 16)
        base = R(rn, 1, sp=True)
        if bit(i, 24) == 0:                              # multiple structures
            if not post and rm:
                raise Unsupported("SIMD: ld/st multiple unallocated")
            opc, size = bits(i, 15, 12), bits(i, 11, 10)
            table = {0b0000: (1, 4), 0b0010: (4, 1), 0b0100: (1, 3), 0b0110: (3, 1), 0b0111: (1, 1),
                     0b1000: (1, 2), 0b1010: (2, 1)}
            if opc not in table or (size == 3 and not q and table[opc][1] > 1):
                raise Unsupported("SIMD: ld/st multiple unallocated")
            rpt, selem = table[opc]
            esz = 1 << size
            total = rpt * selem * (16 if q else 8)
            op = f"simd_ldst_multi(c, {rt}, a, {q}, {esz}, {rpt}, {selem}, {load});"
        else:                                            # single structure (one lane), or replicate
            if not post and rm:
                raise Unsupported("SIMD: ld/st single unallocated")
            opc, S, size, Rbit = bits(i, 15, 13), bit(i, 12), bits(i, 11, 10), bit(i, 21)
            selem = (((opc & 1) << 1) | Rbit) + 1
            scale = opc >> 1
            if scale == 3:                               # LD1R..LD4R
                if not load or S:
                    raise Unsupported("SIMD: ld replicate unallocated")
                esz = 1 << size
                total = selem * esz
                op = f"simd_ld_replicate(c, {rt}, a, {q}, {esz}, {selem});"
            else:
                if scale == 0:
                    idx = (q << 3) | (S << 2) | size
                elif scale == 1:
                    if size & 1:
                        raise Unsupported("SIMD: ld/st single unallocated")
                    idx = (q << 2) | (S << 1) | (size >> 1)
                else:
                    if size & 2:
                        raise Unsupported("SIMD: ld/st single unallocated")
                    if size & 1:
                        if S:
                            raise Unsupported("SIMD: ld/st single unallocated")
                        scale, idx = 3, q
                    else:
                        idx = (q << 1) | S
                esz = 1 << scale
                total = selem * esz
                op = f"simd_ldst_single(c, {rt}, a, {esz}, {idx}, {selem}, {load});"
        if post:
            inc = hx(total) if rm == 31 else R(rm, 1)
            return [f"{{ uint64_t a = {base}; {op} {W(rn, 1, f'a + {inc}', sp=True)} }}"]
        return [f"{{ uint64_t a = {base}; {op} }}"]
