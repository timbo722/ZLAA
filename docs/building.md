# Building ZLAA

ZLAA has three parts: the recompiler (Python), which turns the game's ARM64 code into C; the runtime (C), which
stands in for the Switch's operating system and graphics; and the menus (C++, RmlUi). Players never need any of this:
the app builds the game's code on their device. These steps are for working on ZLAA itself.

Everything that comes from the game (the generated C, the built game library) is made from **your own** game files
and must stay outside this repository.

## Recompiler and tests

```
pip install -r requirements.txt
python tests/run_tests.py                        # recompiler tests (needs nothing from the game)
python tests/fuzz.py --random 20000              # single instructions vs the Unicorn emulator
python tests/test_astc.py                        # the ASTC texture decoder vs ARM's astcenc
python recompiler/recomp.py <your game>/exefs -o ~/laar-build/gen --no-comments
```

`recomp.py` takes about 5 minutes and writes about 670 MB of C. It exits with status 1 when some data words in the
code can't be translated: that is normal.

## Desktop (Linux x86-64)

A development tool, not a way to play: it runs the same runtime on Vulkan, with SDL 3 for the window, input and audio.

```
tools/build_ui_deps.sh desktop                   # RmlUi + FreeType for the menus (optional)
cmake -S . -B ~/laar-build/host -G Ninja -DGAME_GEN_DIR=$HOME/laar-build/gen
cmake --build ~/laar-build/host                  # laa_host + libgame-<build id>.so
~/laar-build/host/laa_host <your game>/exefs     # LAA_MENU=1 for the menus
```

## Android

The app is built without Gradle (aapt2, javac, d8, zipalign, apksigner) against the NDK; the setup, the SDL 3 build
and the commands are in [tools/android/README.md](../tools/android/README.md). `tools/android/build_apk.sh` signs
with a debug key, or with your own release key (`LAA_SIGN_KEYSTORE`, `LAA_SIGN_PASS_FILE`).

A game library built on a PC (`libgame-<build id>.so`) can be copied to
`/sdcard/Android/data/io.github.zlaa/files/game/`: the app picks it up when it starts, instead of Build Game.

## How it fits together

```
recompiler/   ARM64 -> C: NSO loader, function discovery, the instruction translator (arm64.py), recomp.py
runtime/      what the game runs on: CPU state and memory, the Horizon kernel and services (runtime/services),
              NVN on Vulkan (runtime/nvn, runtime/render), the Maxwell shader translator (runtime/render/shader),
              movies, the menus (runtime/ui)
host/         the program: laa_host on the desktop, libmain.so in the Android app (main.c); Build Game (builder.c)
res/          the menus' documents, style sheets, fonts and pictures; the Android icon
tools/        build and test tools; tools/android: the APK and the on-device toolchain; tools/thor: test scripts
tests/        recompiler test programs, fuzzing, ASTC decoder tests
```

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

Branches inside a function become `goto`, `BL` a C call, jump tables a `switch`; the C compiler optimises it all
back into fast native code. The tests build small C programs as real ARM64 binaries (plain ELF, and Switch-style NSO
modules), recompile them, and compare every `test_*` function's results with a native build on about 800 inputs
each; the fuzzer checks single instructions bit for bit against [Unicorn](https://www.unicorn-engine.org).

Generated C is always compiled with `-ffp-contract=off -fwrapv` and never `-ffast-math`, so floating-point results
match the Switch's exactly.
