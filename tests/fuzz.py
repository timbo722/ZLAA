#!/usr/bin/env python3
"""
fuzz.py - check single instructions bit-for-bit against a real ARM64 emulator.

For each instruction word we create random CPU states (registers, flags, vector registers full
of interesting floats: NaNs, infinities, subnormals...). We then:
  * run the instruction in Unicorn (QEMU's ARM64 CPU) = the reference
  * run our translated C for the same instruction from the same state
and compare every register (and memory, for stores) bit for bit.

Instruction sources:
  --random N      N random valid instruction words from the major encoding groups
  --game DIR      the distinct instruction words used by a game's ExeFS (stays on your PC)
  --words FILE    hex words, one per line

    python tests/fuzz.py --random 20000
    python tests/fuzz.py --game ../LADump/exefs --per-mnemonic 300
"""
import argparse
import collections
import os
import random
import re
import struct
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "recompiler"))
from arm64 import Translator, Unsupported, branch_info  # noqa: E402

import capstone  # noqa: E402
from unicorn import Uc, UcError, UC_ARCH_ARM64, UC_MODE_ARM  # noqa: E402
from unicorn import arm64_const as A  # noqa: E402

BUILD = os.environ.get("LAAR_BUILD") or os.path.join(ROOT, "build", "tests")
CODE_BASE = 0x10000000
MEM_SIZE = 0x100000           # guest data memory: [0, 1 MiB)
STATE_FMT = "<31QQQ512s"      # x0-x30, sp, nzcv, v0-v31
STATE_SIZE = struct.calcsize(STATE_FMT)
OUT_SIZE = STATE_SIZE + 8     # + memory hash

_cs = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)


def disasm(w):
    for ins in _cs.disasm(w.to_bytes(4, "little"), 0):
        return ins.mnemonic, ins.op_str
    return None, None


def shape(w):
    """'fmul v0.4s, v1.4s, v2.s[1]' -> 'fmul v.4s, v.4s, v.s[n]' (for grouping results)."""
    m, ops = disasm(w)
    if m is None:
        return "<invalid>"
    ops = re.sub(r"\b[vqdshbwx]\d+\b", lambda g: g.group(0)[0], ops)
    ops = re.sub(r"#-?0x[0-9a-f]+|#-?\d+(\.\d+)?(e[+-]?\d+)?", "#i", ops)
    ops = re.sub(r"\[\d+\]", "[n]", ops)
    return f"{m} {ops}"


def mem_pattern():
    return bytes(((i * 2654435761) >> 13) & 0xFF for i in range(MEM_SIZE))


# ---------------------------------------------------------------- random states
F32_SPECIAL = [0x00000000, 0x80000000, 0x3F800000, 0xBF800000, 0x7F800000, 0xFF800000, 0x7FC00000,
               0xFFC00001, 0x7F800001, 0x7FA00000, 0x00000001, 0x807FFFFF, 0x00800000, 0x7F7FFFFF,
               0x3F000000, 0x40490FDB, 0x4B000000, 0xCF000000, 0x4F800000, 0x3EAAAAAB]
F64_SPECIAL = [0, 1 << 63, 0x3FF0000000000000, 0xBFF0000000000000, 0x7FF0000000000000,
               0xFFF0000000000000, 0x7FF8000000000000, 0x7FF0000000000001, 0xFFF4000000000000,
               0x0000000000000001, 0x000FFFFFFFFFFFFF, 0x0010000000000000, 0x7FEFFFFFFFFFFFFF,
               0x3FE0000000000000, 0x43E0000000000000, 0xC3E0000000000000, 0x41DFFFFFFFC00000]


def rand_f32(r):
    k = r.random()
    if k < 0.3:
        return r.choice(F32_SPECIAL)
    if k < 0.7:   # ordinary numbers of moderate size
        return struct.unpack("<I", struct.pack("<f", r.uniform(-1000, 1000)))[0]
    if k < 0.85:  # small integers as floats
        return struct.unpack("<I", struct.pack("<f", float(r.randint(-40, 40))))[0]
    return r.getrandbits(32)


def rand_f64(r):
    k = r.random()
    if k < 0.3:
        return r.choice(F64_SPECIAL)
    if k < 0.75:
        return struct.unpack("<Q", struct.pack("<d", r.uniform(-1e6, 1e6)))[0]
    return r.getrandbits(64)


def rand_vreg(r):
    k = r.random()
    if k < 0.35:
        return sum(rand_f32(r) << (32 * i) for i in range(4))
    if k < 0.55:
        return sum(rand_f64(r) << (64 * i) for i in range(2))
    if k < 0.7:   # small signed lanes (good for saturating / narrowing ops)
        width = r.choice([8, 16, 32, 64])
        return sum((r.randint(-130, 130) & ((1 << width) - 1)) << (width * i) for i in range(128 // width))
    if k < 0.8:   # byte indices (TBL) and shift counts
        return sum(r.randint(0, 40) << (8 * i) for i in range(16))
    return r.getrandbits(128)


def rand_xreg(r):
    k = r.random()
    if k < 0.45:
        return 0x8000 + r.randrange(0, 0x1000, 8)   # usable as a memory address
    if k < 0.65:
        return r.randint(0, 70)
    if k < 0.75:
        return r.choice([0, 1, (1 << 64) - 1, 1 << 63, (1 << 63) - 1, 0xFFFFFFFF, 0x80000000])
    return r.getrandbits(64)


def rand_state(r):
    x = [rand_xreg(r) for _ in range(31)]
    sp = 0x80000 + r.randrange(0, 0x1000, 16)
    nzcv = r.getrandbits(4) << 28
    v = b"".join(rand_vreg(r).to_bytes(16, "little") for _ in range(32))
    return x, sp, nzcv, v


# ---------------------------------------------------------------- instruction words
GROUPS = [   # (mask, value) of the top-level encoding groups to draw random words from
    (0x0E000000, 0x0E000000),   # SIMD & FP data processing (x111)
    (0x0E000000, 0x0E000000),   # (twice: it's the area under test)
    (0x0E000000, 0x0C000000),   # SIMD & FP loads/stores (x1x0 with V=1)
    (0x1C000000, 0x10000000),   # data processing - immediate (100x)
    (0x0E000000, 0x0A000000),   # data processing - register (x101)
    (0x0E000000, 0x08000000),   # integer loads/stores (x1x0 with V=0)
]


def usable(w, tr):
    """A word we can test: valid, not a branch/system op, and our translator handles it."""
    if branch_info(w, 0) or (w >> 22) == 0b1101010100 or (w >> 24) == 0b11010100:
        return False
    m, _ = disasm(w)
    if m is None or m.startswith(("ldx", "stx", "lda", "stl", "ldax", "stlx", "prfm", "prfum")):
        return False   # exclusives depend on monitor state; prefetch has no visible effect
    try:
        tr.translate(w, CODE_BASE)
        return True
    except Unsupported:
        return False


class _Ctx:
    def local_label(self, a): return None
    def func_name(self, a): return None


def random_words(n, seed, tr):
    r = random.Random(seed)
    out, unsupported = [], collections.Counter()
    tries = 0
    while len(out) < n and tries < n * 400:
        tries += 1
        mask, val = r.choice(GROUPS)
        w = (r.getrandbits(32) & ~mask) | val
        if usable(w, tr):
            out.append(w)
        else:
            m, _ = disasm(w)
            if m and not branch_info(w, 0):
                try:
                    tr.translate(w, CODE_BASE)
                except Unsupported:
                    unsupported[shape(w)] += 1
    return out, unsupported


def game_words(exefs, per_mnemonic, seed, tr):
    from nso_loader import load_exefs
    from discover import discover
    img = load_exefs(exefs)
    fs = discover(img)
    by_shape = collections.defaultdict(set)
    for start, (_, size) in fs.funcs.items():
        for pc in range(start, start + size, 4):
            w = img.word(pc)
            by_shape[shape(w)].add(w)
    r = random.Random(seed)
    out = []
    for sh, ws in sorted(by_shape.items()):
        ws = sorted(ws)
        r.shuffle(ws)
        for w in ws[:per_mnemonic]:
            if usable(w, tr):
                out.append(w)
    return out


# ---------------------------------------------------------------- reference: Unicorn
XREGS = [getattr(A, f"UC_ARM64_REG_X{i}") for i in range(29)] + [A.UC_ARM64_REG_X29, A.UC_ARM64_REG_X30]
VREGS = [getattr(A, f"UC_ARM64_REG_V{i}") for i in range(32)]


class Reference:
    def __init__(self, n_words):
        self.uc = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
        self.uc.mem_map(0, MEM_SIZE)
        self.uc.mem_map(CODE_BASE, (n_words * 4 + 0xFFFF) & ~0xFFF)
        try:
            self.uc.reg_write(A.UC_ARM64_REG_CPACR_EL1, 0x300000)   # enable FP/SIMD
        except UcError:
            pass
        self.pattern = mem_pattern()
        self.uc.mem_write(0, self.pattern)

    def run(self, idx, word, state, is_store):
        uc = self.uc
        addr = CODE_BASE + 4 * idx
        uc.mem_write(addr, struct.pack("<I", word))
        x, sp, nzcv, v = state
        for k in range(31):
            uc.reg_write(XREGS[k], x[k])
        uc.reg_write(A.UC_ARM64_REG_SP, sp)
        uc.reg_write(A.UC_ARM64_REG_NZCV, nzcv)
        uc.reg_write(A.UC_ARM64_REG_FPCR, 0)
        for k in range(32):
            uc.reg_write(VREGS[k], int.from_bytes(v[16 * k:16 * k + 16], "little"))
        try:
            uc.emu_start(addr, addr + 4, count=1)
        except UcError:
            if is_store:
                uc.mem_write(0, self.pattern)
            return None
        xs = [uc.reg_read(XREGS[k]) for k in range(31)]
        vs = b"".join(uc.reg_read(VREGS[k]).to_bytes(16, "little") for k in range(32))
        mh = 0
        if is_store:
            mh = fnv(uc.mem_read(0, MEM_SIZE))
            uc.mem_write(0, self.pattern)
        return xs, uc.reg_read(A.UC_ARM64_REG_SP), uc.reg_read(A.UC_ARM64_REG_NZCV) & 0xF0000000, vs, mh


def fnv(data):
    """FNV-1a over 8-byte words (same as the C harness)."""
    h = 0xCBF29CE484222325
    for (q,) in struct.iter_unpack("<Q", bytes(data)):
        h = ((h ^ q) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return h


# ---------------------------------------------------------------- ours: translated C
HARNESS = r"""
#include "recomp_runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <setjmp.h>
#define MEM_SIZE 0x100000
static jmp_buf fault;
void recomp_mem_fault(CPU *c, uint64_t a, int n) { (void)c; (void)a; (void)n; longjmp(fault, 1); }
void recomp_call_addr(CPU *c, uint64_t a) { (void)c; (void)a; longjmp(fault, 2); }
void recomp_unimplemented(CPU *c, uint64_t p, uint32_t i) { (void)c; (void)p; (void)i; longjmp(fault, 3); }
void recomp_bad_jump(CPU *c, uint64_t p, uint64_t t) { (void)c; (void)p; (void)t; longjmp(fault, 4); }
void recomp_svc(CPU *c, uint64_t p, uint32_t i) { (void)c; (void)p; (void)i; longjmp(fault, 5); }
void recomp_trap(CPU *c, uint64_t p, uint32_t i) { (void)c; (void)p; (void)i; longjmp(fault, 6); }
uint64_t recomp_cntpct(void) { return 0; }
#pragma pack(push, 1)
typedef struct { uint64_t x[31], sp, nzcv; uint8_t v[512]; } State;
typedef struct { State s; uint64_t memhash; } Out;
#pragma pack(pop)
static uint8_t mem[MEM_SIZE];
static void pattern(void) { for (uint64_t i = 0; i < MEM_SIZE; i++) mem[i] = (uint8_t)((i * 2654435761ull) >> 13); }
static uint64_t fnv(void) {
    uint64_t h = 0xCBF29CE484222325ull, q;
    for (uint64_t i = 0; i < MEM_SIZE; i += 8) { memcpy(&q, mem + i, 8); h = (h ^ q) * 0x100000001B3ull; }
    return h;
}
"""

MAIN = r"""
int main(int argc, char **argv) {
    FILE *in = fopen(argv[1], "rb"), *out = fopen(argv[2], "wb");
    static CPU cpu;
    pattern();
    for (int i = 0; i < N_TESTS; i++) {
        State s; Out o;
        if (fread(&s, sizeof s, 1, in) != 1) return 1;
        memset(&cpu, 0, sizeof cpu);
        memcpy(cpu.x, s.x, sizeof s.x);
        cpu.sp = s.sp;
        cpu.n = (s.nzcv >> 31) & 1; cpu.z = (s.nzcv >> 30) & 1; cpu.c = (s.nzcv >> 29) & 1; cpu.v = (s.nzcv >> 28) & 1;
        memcpy(cpu.V, s.v, 512);
        cpu.mem = mem; cpu.mem_size = MEM_SIZE; cpu.excl_addr = ~0ull;
        int faulted = setjmp(fault);
        if (!faulted) tests[i](&cpu);
        memcpy(o.s.x, cpu.x, sizeof o.s.x);
        o.s.sp = cpu.sp;
        o.s.nzcv = faulted ? ~0ull : ((uint64_t)cpu.n << 31) | ((uint64_t)cpu.z << 30) | ((uint64_t)cpu.c << 29) | ((uint64_t)cpu.v << 28);
        memcpy(o.s.v, cpu.V, 512);
        o.memhash = stores[i] ? fnv() : 0;
        if (stores[i]) pattern();
        fwrite(&o, sizeof o, 1, out);
    }
    fclose(out);
    return 0;
}
"""


def build_and_run_ours(words, states, stores, workdir, cc):
    tr = Translator(_Ctx())
    lines = [HARNESS]
    for k, w in enumerate(words):
        stmts = tr.translate(w, CODE_BASE + 4 * k)
        lines.append(f"static void t{k}(CPU *c) {{ {' '.join(stmts)} }}")
    lines.append("static void (*const tests[])(CPU *) = {" + ",".join(f"t{k}" for k in range(len(words))) + "};")
    lines.append("static const unsigned char stores[] = {" + ",".join("1" if s else "0" for s in stores) + "};")
    lines.append(f"#define N_TESTS {len(words)}")
    lines.append(MAIN)
    src = os.path.join(workdir, "fuzz_harness.c")
    exe = os.path.join(workdir, "fuzz_harness" + (".exe" if os.name == "nt" else ""))
    with open(src, "w") as f:
        f.write("\n".join(lines))
    r = subprocess.run(cc + ["-O1", "-w", "-ffp-contract=off", "-fwrapv", "-DRECOMP_CHECK_MEM",
                             "-I", os.path.join(ROOT, "runtime"), src, os.path.join(ROOT, "runtime", "recomp_crypto.c"),
                             "-o", exe, "-lm"],
                       capture_output=True, text=True)
    if r.returncode:
        sys.exit("harness compile failed:\n" + r.stderr[-3000:])
    sin, sout = os.path.join(workdir, "states.bin"), os.path.join(workdir, "out.bin")
    with open(sin, "wb") as f:
        for (x, sp, nzcv, v) in states:
            f.write(struct.pack(STATE_FMT, *x, sp, nzcv, v))
    r = subprocess.run([exe, sin, sout], capture_output=True, text=True, timeout=600)
    if r.returncode:
        sys.exit(f"harness crashed ({r.returncode}):\n{r.stderr[-2000:]}")
    results = []
    data = open(sout, "rb").read()
    for k in range(len(words)):
        chunk = data[k * OUT_SIZE:(k + 1) * OUT_SIZE]
        vals = struct.unpack(STATE_FMT, chunk[:STATE_SIZE])
        x, sp, nzcv, v = list(vals[:31]), vals[31], vals[32], vals[33]
        mh = struct.unpack("<Q", chunk[STATE_SIZE:])[0]
        results.append(None if nzcv == 0xFFFFFFFFFFFFFFFF else (x, sp, nzcv, v, mh))
    return results


def _nan_only(a, b):
    """True if a and b (little-endian bytes) differ only in lanes where both are NaN,
    viewing them as 32-bit or as 64-bit floats. x86 hosts produce different NaN payloads/signs
    than ARM (e.g. default NaN 0xFFC00000 vs 0x7FC00000); an ARM host matches exactly."""
    for width, expmask, fracmask in ((4, 0x7F800000, 0x7FFFFF), (8, 0x7FF0000000000000, (1 << 52) - 1)):
        ok = True
        for k in range(0, len(a), width):
            x = int.from_bytes(a[k:k + width], "little")
            y = int.from_bytes(b[k:k + width], "little")
            if x != y and not (x & expmask == expmask and x & fracmask and y & expmask == expmask and y & fracmask):
                ok = False
                break
        if ok:
            return True
    return False


NAN_STRICT = False


def diff(ref, ours):
    out = []
    for k in range(31):
        if ref[0][k] != ours[0][k] and (NAN_STRICT or not _nan_only(
                ref[0][k].to_bytes(8, "little"), ours[0][k].to_bytes(8, "little"))):
            out.append(f"x{k}: want {ref[0][k]:#x} got {ours[0][k]:#x}")
    if ref[1] != ours[1]:
        out.append(f"sp: want {ref[1]:#x} got {ours[1]:#x}")
    if ref[2] != ours[2]:
        out.append(f"nzcv: want {ref[2] >> 28:04b} got {ours[2] >> 28:04b}")
    for k in range(32):
        a, b = ref[3][16 * k:16 * k + 16], ours[3][16 * k:16 * k + 16]
        if a != b and (NAN_STRICT or not _nan_only(a, b)):
            out.append(f"v{k}: want {a[::-1].hex()} got {b[::-1].hex()}")
    if ref[4] != ours[4]:
        out.append("memory differs")
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--random", type=int, default=0)
    ap.add_argument("--game")
    ap.add_argument("--words")
    ap.add_argument("--per-mnemonic", type=int, default=200)
    ap.add_argument("--states", type=int, default=4, help="random states per instruction word")
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--batch", type=int, default=4000)
    ap.add_argument("--filter", help="only words whose disassembly matches this regex")
    ap.add_argument("--simd-only", action="store_true", help="only SIMD & FP instructions (data processing + loads/stores)")
    ap.add_argument("--show", type=int, default=2, help="example failures to print per shape")
    ap.add_argument("--strict-nan", action="store_true",
                    help="also require identical NaN payloads (only meaningful on an ARM64 host)")
    args = ap.parse_args()
    global NAN_STRICT
    NAN_STRICT = args.strict_nan
    cc = (os.environ.get("CC") or "gcc").split()
    tr = Translator(_Ctx())

    words = []
    if args.random:
        rw, unsup = random_words(args.random, args.seed, tr)
        words += rw
        if unsup:
            print(f"(random words our translator doesn't support yet: {sum(unsup.values())}; top: "
                  + ", ".join(f"{k} x{v}" for k, v in unsup.most_common(8)) + ")")
    if args.game:
        words += game_words(args.game, args.per_mnemonic, args.seed, tr)
    if args.words:
        words += [int(l.split()[0], 16) for l in open(args.words) if l.strip()]
    if args.simd_only:
        words = [w for w in words if (w >> 25) & 0b111 == 0b111 or ((w >> 25) & 0b101 == 0b100 and (w >> 26) & 1)]
    if args.filter:
        rx = re.compile(args.filter)
        words = [w for w in words if rx.search(" ".join(x or "" for x in disasm(w)))]
    r = random.Random(args.seed)
    tests = [(w, rand_state(r)) for w in words for _ in range(args.states)]
    print(f"{len(words)} instruction words x {args.states} states = {len(tests)} tests")

    os.makedirs(BUILD, exist_ok=True)
    per_shape = collections.defaultdict(lambda: [0, 0, []])   # shape -> [pass, fail, examples]
    skipped = 0
    for b in range(0, len(tests), args.batch):
        batch = tests[b:b + args.batch]
        bw = [w for w, _ in batch]
        stores = [(disasm(w)[0] or "").startswith("st") for w in bw]
        ref = Reference(len(batch))
        refs = [ref.run(k, w, s, st) for k, ((w, s), st) in enumerate(zip(batch, stores))]
        ours = build_and_run_ours(bw, [s for _, s in batch], stores, BUILD, cc)
        for (w, s), rr, oo in zip(batch, refs, ours):
            if rr is None or oo is None:
                skipped += 1   # faulted (e.g. address outside test memory) in either
                continue
            d = diff(rr, oo)
            entry = per_shape[shape(w)]
            if d:
                entry[1] += 1
                if len(entry[2]) < args.show:
                    m, ops = disasm(w)
                    entry[2].append(f"{w:08x} {m} {ops}: " + "; ".join(d[:4]))
            else:
                entry[0] += 1
        print(f"  batch {b // args.batch + 1}: {min(b + args.batch, len(tests))}/{len(tests)} done", flush=True)

    failing = {k: v for k, v in per_shape.items() if v[1]}
    total_pass = sum(v[0] for v in per_shape.values())
    total_fail = sum(v[1] for v in per_shape.values())
    for sh, (p, f, ex) in sorted(failing.items(), key=lambda kv: -kv[1][1]):
        print(f"FAIL {sh}: {f} of {p + f}")
        for e in ex:
            print("     " + e)
    print(f"\n{total_pass} passed, {total_fail} failed, {skipped} skipped (memory faults); "
          f"{len(per_shape)} instruction shapes, {len(failing)} with failures")
    return 1 if total_fail else 0


if __name__ == "__main__":
    sys.exit(main())
