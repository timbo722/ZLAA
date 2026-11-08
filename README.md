<div align="center">
  <img src="docs/icon.png" alt="ZLAA's icon: a glowing egg on a dark mountain at night" width="128">

  <h1>ZLAA</h1>

  <p>Koholint Island in your hands: <i>The Legend of Zelda: Link's Awakening</i>, rebuilt as an Android app</p>

  <p align="center">
    <a href="../../releases">Download</a>
    •
    <a href="#getting-started">Getting started</a>
    •
    <a href="../../issues">Report a bug</a>
    •
    <a href="docs/building.md">Building</a>
  </p>
</div>

> [!IMPORTANT]
> **ZLAA was written with AI.** Almost every line of code and documentation here came from Claude (Anthropic's
> AI) working through Claude Code. A person chose what to build, tested each change on a real handheld and decided
> what stayed. It began as a way to learn how a Switch game can be carried over to a different device, and it is
> shared in case it helps or entertains someone else. If AI-written software isn't something you want to use,
> that's completely fair. Like any program from the internet, run it at your own risk.

# Overview

ZLAA takes the Nintendo Switch version of *The Legend of Zelda: Link's Awakening* (2019) and turns it into a
native Android app. It doesn't pretend to be a Switch. Instead, the game's own ARM64 machine code is converted
into C once, compiled into a library for your device, and run on top of a small stand-in for the pieces of the
Switch the game talks to: its operating system's services, and its NVN graphics API, redone on Vulkan. The game's
GPU shaders are converted to SPIR-V the first time they're used and kept for next time.

All of it happens on the device: point ZLAA at your own game files, let it build the game once, then play.

```
your exefs ──► recompiler ──► C code ──► compiler ──► the game, as a native library
               (Python)                  (Zig)                          │
                                                                        ▼
your romfs ───────────────────────────────────────────────────────────► ZLAA ──► Vulkan, sound, controller
```

> [!NOTE]
> Made for **Android 11+ handhelds and phones** (arm64, Vulkan 1.3). So far it has only been tested on an
> **AYN Thor** (Snapdragon 8 Gen 2, Adreno 740). If you try another device, please say how it went.

![The start screen: night over Koholint, the Wind Fish's egg on Mt. Tamaranch. No Nintendo artwork is included](docs/start_screen.jpg)

## What you need

| | |
|---|---|
| Android | 11 or newer, 64-bit (arm64-v8a) |
| Graphics | Vulkan 1.3 (tested on the Adreno 740) |
| Memory | 8 GB or more (tested on 16 GB) |
| Space | About 6 GB for your game files, 130 MB for the built game, and some room while it builds |
| Controls | A gamepad, built in or connected. The game has no touch controls; the menus work with either |
| Game | Your own copy of *Link's Awakening* v1.0.0, v1.0.1 or v1.1.0, as `exefs` and `romfs` folders |

## Where it stands

ZLAA is young. The opening of the game (Toronbo Shores, Mabe Village, the fields around it and the Mysterious
Forest) plays at a steady 60 fps at 720p on the AYN Thor, with sound, the game's movies and its own dynamic resolution.
Beyond that, little has been played yet, so expect surprises. If something breaks, please
[open an issue](../../issues) and include your device, your game version and the app's `log.txt` (in
`Android/data/io.github.zlaa/files/`).

### Rough edges

- Once in a while (about one run in thirty in testing, seen on the beach and in a Mysterious Forest cave) the
  GPU stalls: a short freeze and a few broken frames, then the game carries on.
- Short hitches the first time a new effect appears, while its shader is prepared, and some stutter on the beach.
- A fine flicker on edges while walking. It's the game's own aliasing: a Switch shows it too, more softly.
- Versions 1.0.1 and 1.1.0 pass tests, but only 1.0.0 has had long play sessions.

## What it does

**Plays like an Android game**
- Native ARM64 code and Vulkan graphics: no emulator in between.
- Handheld mode at 720p, or docked at 1080p.
- Press Home and the game pauses like a Switch would; come back and the pause menu is waiting.
- Movies play through Android's own video decoder.
- Shaders are kept between sessions, so places you've been don't stutter again.

**Menus of its own**
- A night over Koholint on the start screen, and a pause menu on the Home/Mode button or Android's Back button.
- Settings for console mode, an FPS counter and its corner, Nintendo or Xbox button layout, stick dead zone,
  volume, skipping the start screen, and a note while shaders compile. Most take effect at once, even mid-game.
- Use them with the controller or by touch, and switch between the two whenever you like.

  ![Settings: video, input, audio, interface](docs/settings.jpg)

**Your game, your files**
- Any of the game's three releases: 1.0.0, 1.0.1 or 1.1.0.
- The game is built on your device from your files, at the pace you pick (Cool, Balanced or Fast), and the build
  takes a break whenever the device warms up.
- Your files are read where they are: nothing is copied, nothing is uploaded.

# Getting started

> [!IMPORTANT]
> ZLAA comes with **nothing** from the game: no code, art, music or fonts. You need your own legally obtained
> copy of *Link's Awakening*, extracted as its `exefs` and `romfs` folders. This project won't explain how to get
> them, and it will never ask you for keys or firmware.

### 1. Put your game files on the device

Copy the `exefs` and `romfs` folders into one folder in the device's main storage, for example `LADump/1.0.0/`.
Avoid `Android/data`: Android keeps that folder hidden from other apps.

### 2. Install ZLAA

Download `ZLAA.apk` from [Releases](../../releases) and open it on the device. Android will ask you to allow
installs from that app the first time.

### 3. Show ZLAA your game

In ZLAA, open **Settings → Interface → Game Files**. Android asks for **All files access** (this lets ZLAA read
the 6 GB where they are instead of copying them). Turn it on, then pick the folder from step 1. The start screen
shows which version it found.

### 4. Build the game

Choose **Build Game**. ZLAA converts your copy's code and compiles it for your device. This happens once per
game version and takes about 55 minutes on the AYN Thor at the default **Balanced** speed (the board stays
around 47 C). **Cool** runs cooler and takes about twice as long; **Fast** runs warmer. At any speed it pauses
when the device gets too warm. Leave ZLAA open, plugged in if you can. If it gets closed, the build picks up where
it stopped. Afterwards, **Settings → Interface → Free Up Space** clears what the build left behind.

What gets built stays on your device. It's made from your own files and never leaves it.

### 5. Play

Choose **Play**.

# FAQ

#### Is this an emulator?
No. An emulator reads the game's instructions one by one while you play. ZLAA converts all of them ahead of time
(static recompilation): every function in the game becomes a C function, and a C compiler turns those into
native code for your device. [N64: Recompiled](https://github.com/N64Recomp/N64Recomp) is a good introduction to
the idea.

#### Is it a port or a decompilation?
Neither. There is no game source code anywhere in this project. ZLAA works from the machine code in your own
files, and supplies its own versions of the Switch parts the game relies on.

#### How does it relate to LAS Recomp?
[LAS Recomp](https://github.com/peiikai/LASRecomp) recompiles the same game for Windows, macOS and Linux, and has
an Android fork. ZLAA is a separate project, built from scratch for Android handhelds, mostly to learn how this
all works. LAS Recomp was useful for checking facts (file layouts, how the game behaves) when stuck; no code was
taken from it.

#### Can I use an NSP or XCI file?
No. Those files are encrypted, and opening them needs keys from a console. ZLAA never asks for keys, so it only
takes the already extracted `exefs` and `romfs` folders.

#### Why does it need "All files access"?
The game reads thousands of files from its 6 GB `romfs` while you play. Android's regular file picker passes
files to an app one at a time through a slow channel, so the alternative would be a second 6 GB copy inside the
app. With this permission ZLAA reads your folder directly. It only reads the folder you choose, and only writes to
its own app folder.

#### Why is building so slow, and why on my device?
The game has about 150,000 functions, and every one is converted to C and compiled: heavy work for a handheld.
It can't be handed out ready-made, because the result would be Nintendo's code. You build it once per game
version; a ZLAA update that changes the conversion asks for a rebuild.

#### Can I build on a PC instead?
There's no player-friendly way yet. If you're comfortable with developer tools, you can build it on a PC
([docs/building.md](docs/building.md)) and copy the `libgame-<build id>.so` file into
`/sdcard/Android/data/io.github.zlaa/files/game/`. ZLAA picks it up the next time it starts.

#### Where are my saves?
In `/sdcard/Android/data/io.github.zlaa/files/save/`. Copy that folder somewhere safe before uninstalling ZLAA:
Android deletes it along with the app.

# Building

Instructions for building ZLAA yourself (the recompiler, the desktop build used while developing, and the Android
app) are in [docs/building.md](docs/building.md).

# Credits

**Built with**
- [SDL 3](https://libsdl.org) for the window, controller and sound
- [RmlUi](https://github.com/mikke89/RmlUi) and [FreeType](https://freetype.org) for the menus and their text
- [Nunito](https://github.com/googlefonts/nunito) and [Material Symbols](https://github.com/google/material-design-icons) for the menus' lettering and icons
- [Python](https://www.python.org) and [Zig](https://ziglang.org), which build the game on your device
- The [SPIR-V headers](https://github.com/KhronosGroup/SPIRV-Headers) from the Khronos Group (`third_party/spirv`)
- [Capstone](https://www.capstone-engine.org), [Unicorn](https://www.unicorn-engine.org), [pyelftools](https://github.com/eliben/pyelftools)
  and [python-lz4](https://github.com/python-lz4/python-lz4) in the recompiler and its tests
- [libvpx](https://chromium.googlesource.com/webm/libvpx) for movies in the desktop build

**Learned from**
- **Ryujinx**: its shader decoder's instruction patterns became `runtime/render/shader/maxwell_ops.h` (MIT,
  notice in [THIRD_PARTY.md](THIRD_PARTY.md)), and it was the guide to how Maxwell GPU instructions behave
- **[LAS Recomp](https://github.com/peiikai/LASRecomp)** by peiikai and its
  [Android fork](https://github.com/mamcjr/LASRecompAndroid) by mamcjr, which showed this game could be
  recompiled at all
- **[Dusklight](https://github.com/TwilitRealm/dusklight)** by the TwilitRealm team, whose dark, warm menus
  inspired ours
- **[N64: Recompiled](https://github.com/N64Recomp/N64Recomp)** and
  **[Zelda 64: Recompiled](https://github.com/Zelda64Recomp/Zelda64Recomp)**, which made static recompilation of
  games a thing
- [Switchbrew](https://switchbrew.org) for documenting the Switch's system and file formats, and
  [envytools](https://envytools.readthedocs.io) for NVIDIA's GPUs
- The *Arm Architecture Reference Manual*

# Legal

ZLAA is an unofficial fan project with no connection to Nintendo, and Nintendo has neither approved nor
supported it. *The Legend of Zelda* and *Link's Awakening* are Nintendo's trademarks. Nothing from the game is
in this repository or in any ZLAA download: to play, you need your own legally obtained copy.

# License

ZLAA's own code is free to use under the [MIT License](LICENSE). Code from other projects keeps its own license
(see [THIRD_PARTY.md](THIRD_PARTY.md)). None of this gives any rights to Nintendo's game itself.

### Third-party code in the app

The Android app carries these components. Each one's source, at the version inside the app, is linked below:

| Component | Version | License | Source code |
|---|---|---|---|
| SDL 3 | 3.4.2 | zlib | https://github.com/libsdl-org/SDL/releases/tag/release-3.4.2 |
| RmlUi | 6.3 | MIT | https://github.com/mikke89/RmlUi/releases/tag/6.3 |
| FreeType | 2.14.3 | FreeType License | https://gitlab.freedesktop.org/freetype/freetype/-/tags/VER-2-14-3 |
| Python, python.org's Android package (runs the recompiler) | 3.14.10 | PSF License | https://www.python.org/downloads/release/python-31410/ |
| Zig (compiles the game; contains LLVM, Clang, LLD and musl's C headers) | 0.14.1 | MIT; LLVM: Apache 2.0 with LLVM exceptions; musl: MIT | https://ziglang.org/download/0.14.1/zig-0.14.1.tar.xz |
| Nunito font | 3.602 | SIL OFL 1.1 | https://github.com/googlefonts/nunito |
| Material Symbols Rounded font (subset) | 2.978 | Apache 2.0 | https://github.com/google/material-design-icons |

How each one is fetched and built is in `tools/build_ui_deps.sh`, `tools/android/builder/make_toolchain.py`,
`tools/make_fonts.py` and [tools/android/README.md](tools/android/README.md) (SDL 3).
