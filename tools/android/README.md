# Building ZLAA for Android

Everything lives in `$LAA_ANDROID` (default `~/laar-build/android`), outside the repo. Nothing is
installed system-wide.

## One-time setup

1. **JDK 17** (Eclipse Temurin) unpacked as `$LAA_ANDROID/jdk-17*/`.
2. **Android command-line tools** unpacked as `$LAA_ANDROID/sdk/cmdline-tools/latest/`, then:
   ```
   export JAVA_HOME=$LAA_ANDROID/jdk-17*
   sdkmanager --sdk_root=$LAA_ANDROID/sdk --licenses
   sdkmanager --sdk_root=$LAA_ANDROID/sdk "platform-tools" "build-tools;35.0.0" "platforms;android-35" "ndk;27.2.12479018"
   ```
3. **SDL3 3.4.2 source** unpacked as `$LAA_ANDROID/src/SDL3-3.4.2/`, built and installed for Android:
   ```
   NDK=$LAA_ANDROID/sdk/ndk/27.2.12479018
   cmake -S src/SDL3-3.4.2 -B build/sdl3 -G Ninja -DCMAKE_TOOLCHAIN_FILE=$NDK/build/cmake/android.toolchain.cmake \
         -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-30 -DCMAKE_BUILD_TYPE=Release \
         -DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TEST_LIBRARY=OFF -DCMAKE_INSTALL_PREFIX=$LAA_ANDROID/prefix
   cmake --build build/sdl3 && cmake --install build/sdl3
   ```
4. **The menus' libraries** (RmlUi + FreeType, static; downloaded into `~/laar-build/deps/src`), for Android and the
   desktop: `tools/build_ui_deps.sh` (without them the app starts the game straight away, with no menus).

## Building

The generated game C (`recomp.py -o ~/laar-build/gen`) is the same for desktop and Android.
```
cmake -S . -B $LAA_ANDROID/build/game -G Ninja -DGAME_GEN_DIR=$HOME/laar-build/gen \
      -DCMAKE_TOOLCHAIN_FILE=$NDK/build/cmake/android.toolchain.cmake -DANDROID_ABI=arm64-v8a \
      -DANDROID_PLATFORM=android-30 -DCMAKE_PREFIX_PATH=$LAA_ANDROID/prefix -DCMAKE_FIND_ROOT_PATH=$LAA_ANDROID/prefix
cmake --build $LAA_ANDROID/build/game --target main game # -> libmain.so, libgame-<main build id>.so
tools/android/build_apk.sh                                 # -> $LAA_ANDROID/ZLAA.apk (no game code in it)
```

## On the device

The app reads everything from its own folder, `/sdcard/Android/data/io.github.zlaa/files/`
(no storage permission needed; adb can write there):
```
adb install -r $LAA_ANDROID/ZLAA.apk
adb push <your dump>/exefs /sdcard/Android/data/io.github.zlaa/files/exefs
adb push <your dump>/romfs /sdcard/Android/data/io.github.zlaa/files/romfs
adb shell mkdir -p /sdcard/Android/data/io.github.zlaa/files/game
adb push libgame-<id>.so /sdcard/Android/data/io.github.zlaa/files/game/   # (strip --strip-debug first: ~150 MB)
adb shell am start -n io.github.zlaa/.ZLAAActivity
adb logcat -s ZLAA            # everything the runtime prints (also files/log.txt)
```
Options: put `LAA_*=value` lines in `files/env.txt` (e.g. `LAA_INPUT_SCRIPT=...`, `LAA_SCREENSHOT=/sdcard/Android/data/io.github.zlaa/files/shots`).
The dump comes from your own Switch and stays on your own devices: never in this repo or a release.
The same goes for the game library, which is made from it: at start-up the app copies `files/game/libgame-<id>.so`
to its internal storage (Android runs code only from there) whenever the pushed one differs, then opens it.
