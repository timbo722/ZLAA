#!/usr/bin/env python3
"""
run_tests.py - prove the recompiler is correct.

For every test program (tests/programs/*.c), optimisation level, and format:
  1. compile it to a real ARM64 binary            (zig cc, targeting aarch64)
     - "elf" format: a plain static ELF with a full symbol table
     - "nso" format: built like a Switch module (MOD0, dynamic linking, relocations,
       exception tables), converted to NSO, and loaded through the NSO loader. Function
       discovery must find every function without the symbol table.
  2. recompile that binary back into C            (recompiler/recomp.py)
  3. compile the SAME source natively for this PC (gcc/clang)
  4. call every test_* function both ways with ~800 input pairs and compare results

If the recompiled ARM64 code always gives the same answer as the native build,
the instruction translations are right.

    python tests/run_tests.py              # everything
    python tests/run_tests.py -p bits -O 2 -f nso  # one program, opt level, format
"""
import argparse
import glob
import itertools
import os
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TESTS = os.path.join(ROOT, "tests")
BUILD = os.environ.get("LAAR_BUILD") or os.path.join(ROOT, "build", "tests")
RUNTIME = os.path.join(ROOT, "runtime")
RECOMP = os.path.join(ROOT, "recompiler", "recomp.py")
EXE = ".exe" if os.name == "nt" else ""

OPT_LEVELS = ["O0", "O1", "O2", "O3", "Os"]

# Keep the ARM64 code to integer instructions for now (no SIMD/FP), no C library.
ARM_FLAGS = [
    "-target", "aarch64-linux-gnu",
    "-mcpu=generic-neon",                 # scalar FP allowed, NEON (vector) instructions off
    "-mno-outline-atomics",               # atomics inline as LDXR/STXR loops (like the Switch SDK)
    "-ffreestanding", "-fno-builtin", "-fwrapv", "-fno-stack-protector",
    "-ffp-contract=off", "-fno-math-errno",  # no surprise fused multiply-adds; sqrt inline
    "-I", os.path.join(TESTS, "support"),
    "-fno-sanitize=undefined",            # zig enables UB checks at -O0 by default
    "-fno-pie", "-no-pie", "-fno-asynchronous-unwind-tables",
    "-g",                                 # keeps the symbol table even at -Os
    "-nostdlib", "-static", "-Wl,-e,0", "-Wl,--no-gc-sections",
]
# Switch-style module: position independent, dynamically linked, laid out by nso.ld.
NSO_FLAGS = [
    "-target", "aarch64-linux-gnu", "-mcpu=generic-neon", "-mno-outline-atomics",
    "-ffp-contract=off", "-fno-math-errno", "-I", os.path.join(TESTS, "support"),
    "-fno-builtin", "-fwrapv", "-fno-stack-protector", "-fno-sanitize=undefined",
    "-fPIC", "-shared", "-funwind-tables", "-g", "-nostdlib",
    "-Wl,-T," + os.path.join(TESTS, "support", "nso.ld"),
    "-Wl,--build-id", "-Wl,-z,now", "-Wl,--eh-frame-hdr",
]
NSO_BASE = 0x8000000
FORMATS = ["elf", "nso"]

# Native build must match ARM64 C semantics: char is unsigned on ARM64, and -fwrapv on both.
# -ffp-contract=off: the recompiled C must never fuse a*b+c by itself (and the reference neither).
NATIVE_FLAGS = ["-O1", "-fwrapv", "-funsigned-char", "-ffp-contract=off", "-DRECOMP_CHECK_MEM", "-w",
                "-I", os.path.join(TESTS, "support")]

EDGES = [0, 1, 2, 3, 7, 31, 32, 63, 64, 100, 0x7F, 0x80, 0xFF, 0x7FFF, 0x8000, 0xFFFF,
         0x7FFFFFFF, 0x80000000, 0xFFFFFFFF, 0x100000000, 0x123456789ABCDEF0,
         0x7FFFFFFFFFFFFFFF, 0x8000000000000000, 0xFFFFFFFFFFFFFFFF, 0xFFFFFFFFFFFFFFFE,
         # floats (bit patterns): 1.0, -1.0, 0.5, 1.5, 2^31, inf, -inf, quiet NaN, signalling NaN, min normal
         0x3F800000, 0xBF800000, 0x3F000000, 0x3FC00000, 0x4F000000, 0x7F800000, 0xFF800000,
         0x7FC00000, 0x7F800001, 0x00800000,
         # doubles: 1.0, -2.5, 2^63, inf, quiet NaN, signalling NaN, smallest subnormal
         0x3FF0000000000000, 0xC004000000000000, 0x43E0000000000000, 0x7FF0000000000000,
         0x7FF8000000000000, 0x7FF0000000000001, 0x0000000000000001]


def arm_cc():
    if os.environ.get("ARM64_CC"):
        return os.environ["ARM64_CC"].split()
    return [sys.executable, "-m", "ziglang", "cc"]


def native_cc():
    if os.environ.get("CC"):
        return os.environ["CC"].split()
    for cc in ("gcc", "clang", "cc"):
        if shutil.which(cc):
            return [cc]
    sys.exit("No native C compiler found (install gcc or clang, or set CC)")


def run(cmd, what):
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        raise RuntimeError(f"{what} failed:\n  {' '.join(cmd)}\n{r.stdout}{r.stderr}")
    return r.stdout


def find_tests(source):
    """Names of test_* functions defined in a test program."""
    import re
    return re.findall(r"^uint64_t\s+(test_\w+)\s*\(", open(source).read(), re.M)


def write_harness(path, tests, symbols):
    edges = ", ".join(f"0x{v:x}ull" for v in EDGES)
    lines = [
        '#include "recomp_runtime.h"',
        "#include <stdio.h>",
        "#include <inttypes.h>",
        "#include <stdlib.h>",
        "void recomp_init(CPU *c);",
        "/* test programs make no OS calls; the game's SVCs live in runtime/svc.c */",
        'void recomp_svc(CPU *c, uint64_t pc, uint32_t imm) { (void)c; fprintf(stderr, "SVC 0x%x at 0x%" PRIx64 "\\n", imm, pc); abort(); }',
        "extern const uint64_t recomp_stack_top;",
    ]
    for t in tests:
        lines.append(f"uint64_t {t}(uint64_t, uint64_t);")
        lines.append(f"void f_{symbols[t]:x}(CPU *c);")
    lines += [
        "typedef struct { const char *name; uint64_t (*native)(uint64_t, uint64_t); RecompFunc recomp; } Test;",
        "static const Test tests[] = {",
    ]
    lines += [f'    {{ "{t}", {t}, f_{symbols[t]:x} }},' for t in tests]
    lines += [
        "};",
        f"static const uint64_t edges[] = {{ {edges} }};",
        r"""
static CPU cpu;
static int check(const Test *t, uint64_t a, uint64_t b, int fails) {
    uint64_t want = t->native(a, b);
    cpu.x[0] = a; cpu.x[1] = b; cpu.x[30] = 0; cpu.sp = recomp_stack_top;
    t->recomp(&cpu);
    uint64_t got = cpu.x[0];
    int bad = got != want || cpu.sp != recomp_stack_top;
    if (bad && fails < 3)
        printf("    MISMATCH %s(0x%" PRIx64 ", 0x%" PRIx64 "): native 0x%" PRIx64 ", recompiled 0x%" PRIx64 "%s\n",
               t->name, a, b, want, got, cpu.sp != recomp_stack_top ? " (SP not restored!)" : "");
    return bad;
}

int main(void) {
    recomp_use_game(recomp_func_table, recomp_func_count, NULL, 0);   /* (linked in, no game library) */
    recomp_init(&cpu);
    int failed = 0;
    long calls = 0;
    size_t nedges = sizeof edges / sizeof edges[0];
    for (size_t k = 0; k < sizeof tests / sizeof tests[0]; k++) {
        const Test *t = &tests[k];
        int fails = 0;
        for (size_t i = 0; i < nedges; i++)
            for (size_t j = 0; j < nedges; j++) { fails += check(t, edges[i], edges[j], fails); calls++; }
        uint64_t s = 0x243f6a8885a308d3ull;
        for (int r = 0; r < 200; r++) {
            s ^= s << 13; s ^= s >> 7; s ^= s << 17; uint64_t a = s;
            s ^= s << 13; s ^= s >> 7; s ^= s << 17; uint64_t b = s;
            if (r & 1) b &= 0xffff;   /* small numbers matter too */
            fails += check(t, a, b, fails); calls++;
        }
        if (fails) { printf("    FAIL %s: %d mismatches\n", t->name, fails); failed++; }
    }
    printf("RESULT %d failed of %d tests (%ld calls)\n", failed, (int)(sizeof tests / sizeof tests[0]), calls);
    return failed != 0;
}
""",
    ]
    with open(path, "w") as f:
        f.write("\n".join(lines))


def check_discovery(elf, symbols, base):
    """Every function in the (hidden) ELF symbol table must have been discovered."""
    from elftools.elf.elffile import ELFFile
    found = set(symbols.values())
    missing = []
    with open(elf, "rb") as f:
        for sym in ELFFile(f).get_section_by_name(".symtab").iter_symbols():
            if sym["st_info"]["type"] == "STT_FUNC" and sym["st_size"] > 0 and sym["st_value"] + base not in found:
                missing.append(sym.name)
    return missing


def run_one(source, opt, fmt):
    name = os.path.splitext(os.path.basename(source))[0]
    out = os.path.join(BUILD, f"{name}-{opt}-{fmt}")
    os.makedirs(out, exist_ok=True)
    elf = os.path.join(out, f"{name}.elf")
    recomp_c = os.path.join(out, "recomp.c")
    syms = os.path.join(out, "symbols.txt")
    harness = os.path.join(out, "harness.c")
    exe = os.path.join(out, "test" + EXE)
    support = os.path.join(TESTS, "support")

    if fmt == "elf":
        run(arm_cc() + ARM_FLAGS + [f"-{opt}", source, os.path.join(support, "arm_support.c"), "-o", elf],
            "ARM64 compile")
        binary = elf
    else:
        # Two modules, like a game's ExeFS: "main" (the test program) imports memset/memcpy
        # from a separate "sdk" module, so calls between modules go through PLT + relocations.
        binary = os.path.join(out, "exefs")
        os.makedirs(binary, exist_ok=True)
        sdk_elf = os.path.join(out, "sdk.elf")
        run(arm_cc() + NSO_FLAGS + ["-O2", os.path.join(support, "nso_crt0.s"),
                                    os.path.join(support, "arm_support.c"), "-o", sdk_elf], "ARM64 sdk compile")
        run(arm_cc() + NSO_FLAGS + [f"-{opt}", os.path.join(support, "nso_crt0.s"), source, "-o", elf],
            "ARM64 main compile")
        run([sys.executable, os.path.join(support, "elf2nso.py"), elf, os.path.join(binary, "main")], "elf2nso")
        run([sys.executable, os.path.join(support, "elf2nso.py"), sdk_elf, os.path.join(binary, "sdk")], "elf2nso")
    r = subprocess.run([sys.executable, RECOMP, binary, "-o", recomp_c, "--embed-image", "--symbols", syms],
                       capture_output=True, text=True)
    recomp_report = (r.stdout + r.stderr).strip()
    if r.returncode not in (0, 1):
        raise RuntimeError(f"recompiler crashed:\n{recomp_report}")
    symbols = {}
    for line in open(syms):
        n, a = line.split()
        symbols[n] = int(a, 16)
    notes = []
    if fmt == "nso":
        missing = check_discovery(elf, symbols, NSO_BASE)
        if missing:
            notes.append(f"function discovery missed: {', '.join(missing)}")
    tests = [t for t in find_tests(source) if t in symbols]
    write_harness(harness, tests, symbols)
    run(native_cc() + NATIVE_FLAGS + ["-I", RUNTIME, source, recomp_c, harness,
                                      os.path.join(RUNTIME, "recomp_runtime.c"),
                                      os.path.join(RUNTIME, "recomp_crypto.c"), "-o", exe, "-lm"], "native compile")
    r = subprocess.run([exe], capture_output=True, text=True, timeout=30)  # a hang = broken (e.g. a lock never released)
    output = "\n".join(notes + [(r.stdout + r.stderr).strip()])
    return r.returncode == 0 and not notes, recomp_report, output


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("-p", "--program", help="only run programs whose name contains this")
    ap.add_argument("-O", "--opt", help="only this optimisation level: 0, 1, 2, 3 or s")
    ap.add_argument("-f", "--format", choices=FORMATS, help="only this binary format")
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()

    sources = sorted(glob.glob(os.path.join(TESTS, "programs", "*.c")))
    if args.program:
        sources = [s for s in sources if args.program in os.path.basename(s)]
    opts = ["O" + args.opt.lstrip("O")] if args.opt else OPT_LEVELS
    passed = total = 0
    fmts = [args.format] if args.format else FORMATS
    for fmt, source, opt in itertools.product(fmts, sources, opts):
        total += 1
        label = f"{os.path.basename(source):<12} -{opt:<3} {fmt}"
        try:
            ok, report, output = run_one(source, opt, fmt)
        except Exception as e:  # noqa: BLE001
            print(f"ERROR {label}\n{e}")
            continue
        passed += ok
        print(f"{'PASS ' if ok else 'FAIL '} {label}   {report.splitlines()[0].split(': ', 1)[-1]}")
        if not ok or args.verbose:
            for line in report.splitlines()[1:] + output.splitlines():
                print("      " + line)
    print(f"\n{passed}/{total} program builds passed")
    return 0 if passed == total else 1


if __name__ == "__main__":
    sys.exit(main())
