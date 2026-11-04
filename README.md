# LAARecomp

> [!IMPORTANT]
> **This project was made almost exclusively with AI.** Nearly all of the code, tooling, reverse engineering and
> documentation in this repository was written by an AI (Anthropic's Claude, through Claude Code), directed, tested
> and reviewed by a human. It is a personal learning project: I wanted to understand how a game like this can be
> brought to a new device, and to play it on my handheld. Sharing it is a courtesy. If you don't agree with the use
> of AI or don't trust code written by AI, that is your right; you may simply ignore this project and its code.
> As with anything on the internet, you run the code at your own discretion.

**LAARecomp** is a from-scratch static recompilation of *The Legend of Zelda: Link's Awakening* (Nintendo Switch,
2019) for **Android**. The game's ARM64 code is translated into C ahead of time and compiled into a native library
for the device; a runtime written for this project stands in for the Switch's operating system (Horizon), and its
graphics API (NVN) is reimplemented on Vulkan, with the game's GPU shaders translated to SPIR-V. **This is not
emulation**: no instruction is interpreted while the game runs.

> [!NOTE]
> Currently supported:
> - **Android** 11 or newer, arm64, with a Vulkan 1.3 GPU. Developed and tested on an **AYN Thor** (Snapdragon 8
>   Gen 2, Adreno 740); other devices are untested.
> - Linux x86-64 (desktop build): a development tool, not a way to play.

> [!WARNING]
> **Early and experimental. There is no release yet.** Setting the game up currently needs a PC: the game's code is
> recompiled and built there from your own game files, then copied to the device with adb (see
> [Setup](#setup-for-now)). Building it on the device itself, from the start screen, is the next big step.

> [!NOTE]
> **No game files are included.** LAARecomp needs your own legally obtained copy of the extracted game files of
> *Link's Awakening* **v1.0.0, v1.0.1 or v1.1.0**: the `exefs` and `romfs` folders. Instructions on how to obtain
> these files will **NOT** be provided. No game code, assets or fonts are included in this repository or in any
> build, and you are never asked for keys or firmware.

![The start screen: night over Koholint, the Wind Fish's egg on Mt. Tamaranch. No Nintendo artwork is included](docs/start_screen.jpg)

## System requirements

| | Minimum |
|---|---|
| OS | Android 11 (API 30) or newer, 64-bit (arm64-v8a) |
| GPU | Vulkan 1.3 (tested: Adreno 740) |
| RAM | 8 GB (the game reserves a large address space; tested with 16 GB) |
| Storage | About 6 GB for your game files (romfs), plus about 130 MB for the game's built code |
| Controller | A built-in or connected gamepad. There are no touch controls |

## Status

The game boots, plays and saves. What has been played so far (the opening: the beach, Mabe Village, the fields
around it, the first areas) runs at a steady 60 fps at 720p (handheld mode) on the AYN Thor, with sound, movies
and the game's own dynamic resolution. Further into the game is still largely untested.

### Known issues

- Rarely (about 1 run in 30 in testing) the GPU hangs for a moment on the beach, with glitched frames.
- Some stutter in a few places (the beach), and short hitches the first time new effects appear while their
  shaders are prepared.
- A slight, fast flicker on edges while walking: the game's own aliasing (it shows on a Switch too).
- Only version 1.0.0 has had real play time; 1.0.1 and 1.1.0 load and run in tests.

## Features

**Runs natively**
- **This is not emulation.** The game's code runs as native ARM64 code; its graphics are drawn with Vulkan.
- Works with game versions 1.0.0, 1.0.1 and 1.1.0 (one built game library covers 1.0.1 and 1.1.0: their code is
  identical).
- Handheld (720p) or docked (1080p) mode.
- A start screen and a pause menu (the controller's Home/Mode button, Android's Back button):

  ![Settings: display, audio, controls, game, advanced](docs/settings.jpg)
- Settings: mode, frame rate counter (and its corner), volume, button layout (as printed or swapped), stick dead
  zone, the game's language (English, French, German, Italian, Spanish, Dutch, Russian, Japanese, Korean, Chinese),
  and performance options. Most apply instantly, even from the pause menu.
- Pressing Home suspends the game like a Switch does; coming back opens the pause menu.
- The game's movies play through Android's own video decoder.
- Shaders are compiled once and kept, so later sessions start without stutter.

## Setup (for now)

Until the app can build the game's code itself, everything runs from a PC with Linux or WSL2 (Windows), and the
device connected with USB debugging:

1. Extract your own game's `exefs` and `romfs` folders (not provided here) and keep them **outside** this folder.
2. Set up the Android toolchain and build the menus' libraries: [tools/android/README.md](tools/android/README.md)
   and `tools/build_ui_deps.sh`.
3. Recompile the game's code from **your** `exefs` and build it for Android together with the app:

   ```
   python recompiler/recomp.py <your game>/exefs -o ~/laar-build/gen --no-comments
   cmake -S . -B ~/laar-build/android/build/game -G Ninja -DGAME_GEN_DIR=$HOME/laar-build/gen \
         -DCMAKE_TOOLCHAIN_FILE=$NDK/build/cmake/android.toolchain.cmake -DANDROID_ABI=arm64-v8a \
         -DANDROID_PLATFORM=android-30 -DCMAKE_PREFIX_PATH=~/laar-build/android/prefix
   cmake --build ~/laar-build/android/build/game --target main game
   tools/android/build_apk.sh
   ```
4. Install the APK and copy your `exefs`, `romfs` and the built `libgame-<id>.so` to the app's folder
   (`/sdcard/Android/data/io.github.laarecomp/files/`); [tools/android/README.md](tools/android/README.md) has the
   exact commands. The game library is made from your own files: never share it.

## FAQ

#### What is static recompilation?
Translating a program from one machine to another ahead of time, as a whole. Here every function of the Switch
game's ARM64 code becomes a C function, which a C compiler turns into native code for the device. The project
[N64: Recompiled](https://github.com/N64Recomp/N64Recomp) explains the idea well. Unlike an emulator, nothing is
decoded or translated while the game runs.

#### Is this a port or a decompilation?
Neither. There is no source code of the game here: the recompiler works on the machine code in your own game files,
and the runtime reimplements the parts of the Switch the game talks to (its OS services and its graphics API).

#### How is this related to LAS Recomp?
[LAS Recomp](https://github.com/peiikai/LASRecomp) is an existing recompilation of the same game for Windows,
macOS and Linux, with its own Android fork. LAARecomp is a separate project written from scratch for Android, mostly
to learn how it all works; LAS Recomp was a reference for facts (file layouts, behaviours) when stuck. No code was
copied from it.

#### Can I use an NSP or XCI file instead of the folders?
No. Those are encrypted, and reading them needs your console's keys. This project never asks for keys, so it
takes the already extracted `exefs` and `romfs` folders.

#### Where are my saves?
In the app's folder, `/sdcard/Android/data/io.github.laarecomp/files/save/`. Back it up before uninstalling the app:
Android deletes that folder with it.

## Building from source

The recompiler is Python; the runtime and the generated code are C (GCC or Clang); the menus are C++ (RmlUi).

```
pip install -r requirements.txt
python tests/run_tests.py                        # recompiler tests (needs nothing from the game)
python tests/fuzz.py --random 20000              # instructions vs the Unicorn emulator
python recompiler/recomp.py <your game>/exefs -o ~/laar-build/gen --no-comments
tools/build_ui_deps.sh desktop                   # RmlUi + FreeType for the menus (optional)
cmake -S . -B ~/laar-build/host -G Ninja -DGAME_GEN_DIR=$HOME/laar-build/gen
cmake --build ~/laar-build/host                  # laa_host + libgame-<build id>.so
~/laar-build/host/laa_host <your game>/exefs     # LAA_MENU=1 for the menus
```

The generated C (about 670 MB) is made from your game files: keep it outside this repository. Android:
[tools/android/README.md](tools/android/README.md).

### How it fits together

```
recompiler/   ARM64 -> C: NSO loader, function discovery, the instruction translator (arm64.py), recomp.py
runtime/      what the game runs on: CPU state and memory, the Horizon kernel and services (runtime/services),
              NVN on Vulkan (runtime/nvn, runtime/render), the Maxwell shader translator (runtime/render/shader),
              movies, the menus (runtime/ui)
host/main.c   the program: laa_host on the desktop, libmain.so in the Android app
res/ui/       the menus' documents, style sheets and fonts
tools/        build and test tools; tools/thor: test scripts for the AYN Thor
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

## Credits

**Libraries and tools**
- [SDL 3](https://libsdl.org) - window, input and audio (zlib license)
- [RmlUi](https://github.com/mikke89/RmlUi) - the menus (MIT)
- [FreeType](https://freetype.org) - the menus' text (FreeType License)
- [Nunito](https://github.com/googlefonts/nunito) - the menus' font (SIL Open Font License 1.1, `res/ui/fonts/OFL-Nunito.txt`)
- [Material Symbols](https://github.com/google/material-design-icons) - the menus' icons (Apache 2.0, `res/ui/fonts/LICENSE-MaterialSymbols.txt`)
- [SPIR-V headers](https://github.com/KhronosGroup/SPIRV-Headers) by the Khronos Group - `third_party/spirv` (MIT-style license, in the files)
- [Capstone](https://www.capstone-engine.org), [Unicorn](https://www.unicorn-engine.org), [pyelftools](https://github.com/eliben/pyelftools),
  [python-lz4](https://github.com/python-lz4/python-lz4) - used by the recompiler and its tests
- [Zig](https://ziglang.org) - its `zig cc` builds the ARM64 test programs (and, planned, the game's code on the device)
- [libvpx](https://chromium.googlesource.com/webm/libvpx) - movies on the desktop build (BSD)

**Projects referenced, and special thanks**
- **Ryujinx** - the Maxwell shader instruction encodings in `runtime/render/shader/maxwell_ops.h` were generated
  from its shader decoder (MIT; notice in [THIRD_PARTY.md](THIRD_PARTY.md)), and it was the reference for Maxwell
  instruction behaviour
- **[LAS Recomp](https://github.com/peiikai/LASRecomp)** by peiikai and its
  [Android fork](https://github.com/mamcjr/LASRecompAndroid) by mamcjr - proof that this game can be recompiled,
  and a reference for facts when stuck
- **[Dusklight](https://github.com/TwilitRealm/dusklight)** by the TwilitRealm team - inspiration for the menus
  (and their dark, warm colours)
- **[N64: Recompiled](https://github.com/N64Recomp/N64Recomp)** and **[Zelda 64: Recompiled](https://github.com/Zelda64Recomp/Zelda64Recomp)** -
  the idea of static recompilation for games
- [Switchbrew](https://switchbrew.org) - the Switch homebrew community's documentation of its OS and file formats
- [envytools](https://envytools.readthedocs.io) - NVIDIA GPU documentation
- The *Arm Architecture Reference Manual* - instruction encodings and pseudocode

## Legal

LAARecomp is a fan project. It is not affiliated with, endorsed by or sponsored by Nintendo. *The Legend of Zelda*
and *Link's Awakening* are trademarks of Nintendo. No game files, game code, assets or fonts are included - not in
this repository and not in any build. You must provide your own legally obtained copy of the game files to play.

## License

LAARecomp's own code (the recompiler, the runtime, the tools) is open source under the [MIT License](LICENSE).
Third-party components keep their own licenses, listed in [THIRD_PARTY.md](THIRD_PARTY.md). The license covers
LAARecomp's code only - it grants nothing for Nintendo's game, which you must supply yourself.
