# LAARecomp

A from-scratch **static recompilation** of *The Legend of Zelda: Link's Awakening* (Nintendo Switch, 2019)
for **Android**, tested on an AYN Thor.

Static recompilation means translating the game's ARM64 machine code into C *ahead of time*,
compiling that C for the target device, and providing a runtime that stands in for the
Switch's operating system (Horizon) and GPU. It is not an emulator: no instruction is
interpreted while the game runs.

> **No game files, keys, or Nintendo assets ever go in this repository.**
> You supply your own dump of a game you own and your own Switch's `prod.keys`.
> `.gitignore` blocks the common file types, but check before every commit.

## Status

| Phase | What | State |
|---|---|---|
| 1 | Load the game's executable (NSO), find all functions | **done** (148k functions, 8.77M instructions) |
| 2 | **Recompiler: ARM64 → C** | **done: 99.99% of the game's instructions, fuzz-verified** |
| 3 | Boot: implement Horizon OS services until the main loop runs | not started |
| 4 | Graphics: Switch GPU commands + shaders → Vulkan | not started |
| 5 | Input, audio, Android packaging, polish | not started |

The recompiler handles the whole ARM64 instruction set the game uses: integer, atomics, scalar
floating point, all NEON/SIMD (including structure loads, estimates and the AES/SHA/CRC32 extensions)
and the user-mode system registers. Every instruction form the game uses has been checked bit for bit
against an independent ARM64 emulator (see "Fuzzing" below).

## Game version

We target **Link's Awakening v1.0.0** (the base game, no update), the same version as LASRecomp,
so addresses and findings line up with that project. Code addresses differ between versions.

## Running on the game

1. Dump the **ExeFS** of the base game (v1.0.0, *no update*) from your own Switch with nxdumptool.
   Keep it **outside** this folder, e.g. `Documents\LADump\exefs\` (`main`, `sdk`, `rtld`, `subsdk*`, `main.npdm`).
2. Run the recompiler in report mode:

```
python recompiler/recomp.py <path-to>/exefs --report build/la_report.txt --no-comments
```

The report lists each module and its build ID, how many functions were found and how,
and every instruction the translator can't handle yet, sorted by how often it appears.
That list decides what gets implemented next.

## Layout

```
recompiler/
  arm64.py         decoder + translator: one ARM64 instruction -> C statements
  recomp.py        main tool: load, discover functions, translate, write C + report
  image.py         a loaded program in memory (segments, modules)
  elf_loader.py    loads test ELFs
  nso_loader.py    loads Switch NSO modules: LZ4, MOD0, dynamic symbols, relocations, linking
  discover.py      finds functions in stripped code (symbols, eh_frame, pointers, calls, tail calls)
runtime/
  recomp_runtime.h CPU struct (registers, flags, memory) + helpers the generated C uses
  recomp_runtime.c indirect calls, error reporting (later: OS services)
tests/
  programs/*.c     test programs; every test_* function is checked
  support/         memset/memcpy for the ARM64 side, plus nso.ld / nso_crt0.s / elf2nso.py
                   for building Switch-style test NSOs
  run_tests.py     the test runner
build/             generated files (git-ignored)
```

## How the generated C looks

This ARM64 code

```
add  w8, w0, w1
cmp  w8, #10
csinc w0, w8, wzr, lt
ret
```

becomes

```c
void f_1010120(CPU *c) {
    c->x[8] = (uint32_t)((uint32_t)c->x[0] + (uint32_t)c->x[1]);
    (void)(add32_flags(c, (uint32_t)c->x[8], 0xfffffff5ull, 1));   /* cmp = subtract, keep only flags */
    c->x[0] = (uint32_t)(((c->n != c->v)) ? (uint32_t)c->x[8] : ((uint32_t)0 + 1));
    return;
}
```

Registers live in a `CPU` struct, and guest memory is one flat array (`c->mem`) indexed by the ARM64 address.
Branches inside a function become `goto`, `BL` becomes a C function call, and `BR` (jump tables)
goes through a `switch`. The C compiler then optimises it all back into fast native code.

## Running the tests

Requirements: Python 3.10+, a native C compiler (gcc or clang), and these Python packages:

```
pip install -r requirements.txt
```

The `ziglang` package provides `zig cc`, a Clang that can build ARM64 binaries on any PC with no extra setup.

```
python tests/run_tests.py                    # all programs, -O0 -O1 -O2 -O3 -Os, both formats
python tests/run_tests.py -p bits -O 2 -f nso
```

For each test program the runner:

1. compiles it to a real ARM64 binary, in one of two formats:
   - **elf**: a plain static ELF with a symbol table
   - **nso**: built like a real Switch game, then loaded through the NSO loader. This is two
     modules (`main` plus an `sdk` module it imports `memcpy` and friends from), with MOD0,
     relocations, PLT calls between modules, and exception tables. Function discovery must find
     every function without the symbol table, and the runner checks that it did,
2. recompiles that binary back into C with `recomp.py`,
3. compiles the same source natively for your PC,
4. calls every `test_*` function both ways with about 800 input pairs (edge cases and random values) and compares the results.

If the recompiled ARM64 code always matches the native build, the translations are correct.
Test code must be free of undefined behaviour, because x86 and ARM64 handle it differently.
For example, guard divisions by zero and mask shift amounts.

### Adding a test

Add a function to any file in `tests/programs/` (or create a new file):

```c
uint64_t test_something(uint64_t a, uint64_t b) { ... }
```

The runner finds it automatically.

## Fuzzing

`tests/fuzz.py` checks single instructions against [Unicorn](https://www.unicorn-engine.org/)
(QEMU's ARM64 CPU). For each instruction word it makes random CPU states full of interesting values
(NaNs, infinities, subnormals, saturation edges), runs the instruction in Unicorn and in our translated C,
and compares every register and memory bit for bit.

```
python tests/fuzz.py --random 20000                       # random instructions
python tests/fuzz.py --game <path-to>/exefs --simd-only    # the game's own instruction words
```

On an x86 PC, NaN *payloads* are compared loosely (Intel and ARM pick different NaN bit patterns;
the recompiled code runs on ARM, where they match). `--strict-nan` compares them exactly on an ARM64 host.

## References

- *Arm Architecture Reference Manual for A-profile* (instruction encodings and pseudocode)
- [Switchbrew](https://switchbrew.org): Switch OS services, NSO format
- [envytools](https://envytools.readthedocs.io): NVIDIA GPU documentation
- [N64Recomp](https://github.com/N64Recomp/N64Recomp): the same idea for N64
- [LASRecomp](https://github.com/peiikai/LASRecomp): an existing Windows/macOS Link's Awakening recomp, used only as a reference when stuck
