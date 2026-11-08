<div align="center">
  <img src="docs/icon.png" alt="ZLAA's icon: a glowing egg on a dark mountain at night" width="128">

  <h1>ZLAA</h1>

  <p><i>The Legend of Zelda: Link's Awakening</i>, natively on Android</p>

  <p align="center">
    <a href="../../releases">Download</a>
    •
    <a href="#setup">Setup</a>
    •
    <a href="../../issues">Report a bug</a>
    •
    <a href="docs/building.md">Building</a>
  </p>
</div>

> [!IMPORTANT]
> **This project was made almost exclusively with AI.** Nearly all of the code, tooling, reverse engineering and
> documentation in this repository was written by an AI (Anthropic's Claude, through Claude Code), directed, tested
> and reviewed by a human. It is a personal learning project: I wanted to understand how a game like this can be
> brought to a new device, and to play it on my handheld. Sharing it is a courtesy. If you don't agree with the use
> of AI or don't trust code written by AI, that is your right; you may simply ignore this project and its code.
> As with anything on the internet, you run the code at your own discretion.

# Overview

**ZLAA** is a from-scratch static recompilation of *The Legend of Zelda: Link's Awakening* (Nintendo Switch, 2019)
for **Android**. The game's ARM64 code is translated into C ahead of time and compiled into a native library on your
device; a runtime written for this project stands in for the Switch's operating system (Horizon), and its graphics
API (NVN) is reimplemented on Vulkan, with the game's GPU shaders translated to SPIR-V. **This is not emulation**:
no instruction is interpreted while the game runs.

Everything happens on the device: you choose your own game files, the app builds the game's code from them once,
then you play. No PC is needed.

> [!NOTE]
> Currently supported: **Android** 11 or newer, arm64, with a Vulkan 1.3 GPU. Developed and tested on an
> **AYN Thor** (Snapdragon 8 Gen 2, Adreno 740); other devices are untested, and reports are welcome.

![The start screen: night over Koholint, the Wind Fish's egg on Mt. Tamaranch. No Nintendo artwork is included](docs/start_screen.jpg)

## System requirements

| | Minimum |
|---|---|
| OS | Android 11 (API 30) or newer, 64-bit (arm64-v8a) |
| GPU | Vulkan 1.3 (tested: Adreno 740) |
| RAM | 8 GB (the game reserves a large address space; tested with 16 GB) |
| Storage | About 6 GB for your game files (romfs), about 130 MB for the game's built code, plus room for Build Game's temporary files |
| Controller | A built-in or connected gamepad. There are no touch controls in the game (the menus also work by touch) |

## Status

> [!WARNING]
> **Early and experimental.** Bugs are likely and crashes can happen. To report one, open a
> [GitHub issue](../../issues) with your device, the game version and what happened (the app's `log.txt`, in
> `Android/data/io.github.zlaa/files/`, helps a lot).

The game boots, plays and saves. What has been played so far (the opening: the beach, Mabe Village, the fields
around it, the first areas) runs at a steady 60 fps at 720p (handheld mode) on the AYN Thor, with sound, movies
and the game's own dynamic resolution. Further into the game is still largely untested.

### Known issues

- Rarely (about 1 run in 30 in testing: the beach, a cave in the Mysterious Forest) the GPU hangs for a moment,
  with a short freeze and glitched frames. The game carries on.
- Some stutter in a few places (the beach), and short hitches the first time new effects appear while their
  shaders are prepared.
- A slight, fast flicker on edges while walking: the game's own aliasing (it shows on a Switch too).
- Only version 1.0.0 has had real play time; 1.0.1 and 1.1.0 load and run in tests.

## Features

- **This is not emulation.** The game's code runs as native ARM64 code; its graphics are drawn with Vulkan.
- Works with game versions 1.0.0, 1.0.1 and 1.1.0.
- The game's code is built on your device from your own files, at the speed you choose (Cool, Balanced or Fast),
  pausing whenever the device gets warm.
- Handheld (720p) or docked (1080p) mode.
- A start screen and a pause menu (the controller's Home/Mode button, Android's Back button), usable with the
  controller or by touch:

  ![Settings: video, input, audio, interface](docs/settings.jpg)
- Settings: console mode, an FPS counter (and its corner), button layout (Nintendo or Xbox style), stick dead zone,
  volume, skipping the start screen, and an overlay while shaders compile. Most apply instantly, even from the
  pause menu.
- Pressing Home suspends the game like a Switch does; coming back opens the pause menu.
- The game's movies play through Android's own video decoder.
- Shaders are compiled once and kept, so later sessions start without stutter.

# Setup

> [!IMPORTANT]
> **ZLAA does not include any game files.** You need your own legally obtained copy of *Link's Awakening*
> **v1.0.0, v1.0.1 or v1.1.0**, extracted as its `exefs` and `romfs` folders. Instructions on how to obtain these
> files will **NOT** be provided. No game code, assets or fonts are included in this repository or in any build,
> and you are never asked for keys or firmware.

### 1. Get your game files onto the device

Copy your own game's `exefs` and `romfs` folders into one folder on the device, for example `LADump/1.0.0/` in its
main storage. (A folder inside `Android/data` can't be chosen: Android hides it from apps.)

### 2. Install ZLAA

Download `ZLAA.apk` from the [Releases](../../releases) page and open it on the device. Android asks you to allow
installing apps from that source.

### 3. Choose your game files

Open ZLAA, go to **Settings > Interface > Game Files**, allow **All files access** when Android asks (the 6 GB
are read where they are, never copied), then choose the folder from step 1. The start screen shows the version it
found.

### 4. Build the game

Choose **Build Game**. The app translates your copy's code and compiles it for the device, once. On the AYN Thor
this takes about 55 minutes at the default speed, **Balanced** (the board around 47 C). **Cool** keeps the device
cooler and takes about twice as long; **Fast** lets it get warmer. Whatever the speed, it pauses whenever the device
gets too warm. Keep the app open, ideally plugged in; if it closes, the build carries on from where it stopped.
Afterwards, **Settings > Interface > Free Up Space** removes what the build left behind.

The game code made here stays on your device: it is built from your own files and is never shared.

### 5. Play

Choose **Play**.

# FAQ

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
macOS and Linux, with its own Android fork. ZLAA is a separate project written from scratch for Android, mostly
to learn how it all works; LAS Recomp was a reference for facts (file layouts, behaviours) when stuck. No code was
copied from it.

#### Can I use an NSP or XCI file instead of the folders?
No. Those are encrypted, and reading them needs your console's keys. This project never asks for keys, so it
takes the already extracted `exefs` and `romfs` folders.

#### Why does it want "All files access"?
To read your game files where you put them. Android's normal file picker hands an app one file at a time through a
slow channel; the game reads thousands of files from its 6 GB `romfs` while it runs. With this permission the app
reads them directly, and doesn't need a second 6 GB copy in its own folder. It only reads the folder you choose (and
writes its own app folder).

#### Why does Build Game take so long, and why must it run on my device?
The game's code (about 150,000 functions) is translated into C and compiled for your device: a lot of work for a
phone. It can't be downloaded ready-made, because that would mean distributing Nintendo's code. It happens once
per game version; an update of ZLAA that changes how the code is translated asks for it again.

#### Can I build the game's code on a PC instead?
Not with a tool meant for players, for now. Developers can build it on a PC ([docs/building.md](docs/building.md))
and copy the `libgame-<build id>.so` into `/sdcard/Android/data/io.github.zlaa/files/game/`: the app picks it up
when it starts.

#### Where are my saves?
In the app's folder, `/sdcard/Android/data/io.github.zlaa/files/save/`. Back it up before uninstalling the app:
Android deletes that folder with it.

# Building

To build ZLAA from source, see [docs/building.md](docs/building.md) (the recompiler, the desktop build used for
development, and the Android app).

# Credits

**Libraries and tools**
- [SDL 3](https://libsdl.org) - window, input and audio
- [RmlUi](https://github.com/mikke89/RmlUi) - the menus
- [FreeType](https://freetype.org) - the menus' text
- [Nunito](https://github.com/googlefonts/nunito) - the menus' and overlays' font
- [Material Symbols](https://github.com/google/material-design-icons) - the menus' icons
- [Python](https://www.python.org) and [Zig](https://ziglang.org) - run the recompiler and compile the game's code
  on the device
- [SPIR-V headers](https://github.com/KhronosGroup/SPIRV-Headers) by the Khronos Group - `third_party/spirv`
- [Capstone](https://www.capstone-engine.org), [Unicorn](https://www.unicorn-engine.org), [pyelftools](https://github.com/eliben/pyelftools),
  [python-lz4](https://github.com/python-lz4/python-lz4) - used by the recompiler and its tests
- [libvpx](https://chromium.googlesource.com/webm/libvpx) - movies on the desktop build

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

# Legal

ZLAA is a fan project. It is not affiliated with, endorsed by or sponsored by Nintendo. *The Legend of Zelda*
and *Link's Awakening* are trademarks of Nintendo. No game files, game code, assets or fonts are included - not in
this repository and not in any build. You must provide your own legally obtained copy of the game files to play.

# License

ZLAA's own code (the recompiler, the runtime, the tools) is open source under the [MIT License](LICENSE).
Third-party components keep their own licenses, listed in [THIRD_PARTY.md](THIRD_PARTY.md). The license covers
ZLAA's code only - it grants nothing for Nintendo's game, which you must supply yourself.

### Source code of bundled third-party components

The Android app includes these components; their source code for the versions shipped is available here:

| Component | Version | License | Source |
|---|---|---|---|
| SDL 3 | 3.4.2 | zlib | https://github.com/libsdl-org/SDL/releases/tag/release-3.4.2 |
| RmlUi | 6.3 | MIT | https://github.com/mikke89/RmlUi/releases/tag/6.3 |
| FreeType | 2.14.3 | FreeType License | https://gitlab.freedesktop.org/freetype/freetype/-/tags/VER-2-14-3 |
| Python (python.org's Android package; runs the recompiler) | 3.14.10 | PSF License | https://www.python.org/downloads/release/python-31410/ |
| Zig (compiles the game's code; includes LLVM, Clang, LLD; musl's C headers) | 0.14.1 | MIT; LLVM: Apache 2.0 with LLVM exceptions; musl: MIT | https://ziglang.org/download/0.14.1/zig-0.14.1.tar.xz |
| Nunito font | 3.602 | SIL OFL 1.1 | https://github.com/googlefonts/nunito |
| Material Symbols Rounded font (a subset) | 2.978 | Apache 2.0 | https://github.com/google/material-design-icons |

The scripts that fetch and build them (with checksums where downloaded) are in this repository:
`tools/build_ui_deps.sh`, `tools/android/builder/make_toolchain.py`, `tools/make_fonts.py` and
[tools/android/README.md](tools/android/README.md) (SDL 3).
