#!/bin/bash
# build_apk.sh - package LAARecomp as an Android app (APK), step by step, without Gradle.
#
#   tools/android/build_apk.sh            -> $LAA_ANDROID/LAARecomp.apk
#
# An APK is a zip file holding:
#   AndroidManifest.xml   what the app is (binary XML, made by aapt2)
#   classes.dex           its Java code, compiled for Android's runtime (SDL's activity, our LAAActivity)
#   lib/arm64-v8a/*.so    native libraries: SDL3 and libmain.so (our runtime; the game itself is
#                         a library built from your own dump, pushed by tools/thor/deploy.sh)
#   assets/res/...        the menus' documents, style sheets and fonts (res/ in the repo)
#   assets/builder/...    what building the game code on the device needs besides lib/ (make_toolchain.py)
#   META-INF/...          a signature: Android installs only signed apps
#
# Needs (see tools/android/README.md): $LAA_ANDROID with jdk-17*/, sdk/ (build-tools 35,
# platform 35, NDK r27c), src/SDL3-*/ (SDL's Java code), prefix/lib/libSDL3.so, and
# build/game/libmain.so from the Android CMake build.
set -euo pipefail

A=${LAA_ANDROID:-$HOME/laar-build/android}
HERE=$(cd "$(dirname "$0")" && pwd)
JDK=$(ls -d "$A"/jdk-17* | head -1)
BT=$A/sdk/build-tools/35.0.0
PLATFORM=$A/sdk/platforms/android-35/android.jar
NDK=$(ls -d "$A"/sdk/ndk/* | head -1)
STRIP=$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-strip
SDL_JAVA=$(ls -d "$A"/src/SDL3-*/android-project/app/src/main/java | head -1)
OUT=$A/apk
export JAVA_HOME=$JDK PATH=$JDK/bin:$PATH

rm -rf "$OUT"
mkdir -p "$OUT/classes" "$OUT/dex" "$OUT/zip/lib/arm64-v8a"

# 1. The manifest -> a bare APK (binary XML; no other resources)
"$BT/aapt2" link -o "$OUT/base.apk" -I "$PLATFORM" --manifest "$HERE/AndroidManifest.xml" \
    --min-sdk-version 30 --target-sdk-version 35

# 2. Java: SDL's activity and helpers, our LAAActivity (java/) -> .class files -> classes.dex
find "$SDL_JAVA" "$HERE/java" -name '*.java' > "$OUT/java_files.txt"
javac -nowarn -source 11 -target 11 -classpath "$PLATFORM" -d "$OUT/classes" @"$OUT/java_files.txt" 2>&1 \
    | grep -v -E "^warning|^Note:|^[0-9]+ warning" || true
"$BT/d8" --release --min-api 30 --lib "$PLATFORM" --output "$OUT/dex" $(find "$OUT/classes" -name '*.class')

# 3. Native libraries. Debug info stripped (it would make libmain.so several times bigger);
#    the dynamic symbols stay, so crash backtraces still name f_<guest address> functions.
"$STRIP" --strip-debug -o "$OUT/zip/lib/arm64-v8a/libmain.so" "$A/build/game/libmain.so"
"$STRIP" --strip-debug -o "$OUT/zip/lib/arm64-v8a/libSDL3.so" "$A/prefix/lib/libSDL3.so"

# 3b. The game code is built on the device (host/builder.c): Python, the python command and zig as native
#     libraries (the only place an app may run programs from), the rest as an asset (make_toolchain.py makes them)
TC=$A/toolchain/out
python3 "$HERE/builder/make_toolchain.py" "$A"      # (~5 s: it holds the recompiler, so always fresh)
cp "$TC"/lib/*.so "$OUT/zip/lib/arm64-v8a/"
mkdir -p "$OUT/zip/assets/builder"
cp "$TC/builder.tar" "$TC/builder.id" "$OUT/zip/assets/builder/"

# 4. Into the zip: dex + libraries + the menus' files
cp "$OUT/base.apk" "$OUT/unsigned.apk"
cp "$OUT/dex/classes.dex" "$OUT/zip/"
mkdir -p "$OUT/zip/assets"
cp -r "$HERE/../../res" "$OUT/zip/assets/res"
python3 - "$OUT/unsigned.apk" "$OUT/zip" <<'EOF'
import os, sys, zipfile
apk, root = sys.argv[1], sys.argv[2]
with zipfile.ZipFile(apk, "a", zipfile.ZIP_DEFLATED) as z:
    for dirpath, _, files in os.walk(root):
        for f in files:
            path = os.path.join(dirpath, f)
            z.write(path, os.path.relpath(path, root))
EOF

# 5. Align (uncompressed data on 4-byte boundaries, as Android expects) and sign with a debug
#    key made once (a key of your own; it only proves the next build comes from the same place)
KEY=$A/debug.keystore
if [ ! -f "$KEY" ]; then
    keytool -genkeypair -keystore "$KEY" -storepass android -keypass android -alias laarecomp \
        -keyalg RSA -keysize 2048 -validity 10000 -dname "CN=LAARecomp debug" > /dev/null 2>&1
fi
"$BT/zipalign" -f -p 4 "$OUT/unsigned.apk" "$OUT/aligned.apk"
"$BT/apksigner" sign --ks "$KEY" --ks-pass pass:android --key-pass pass:android \
    --out "$A/LAARecomp.apk" "$OUT/aligned.apk"
ls -la "$A/LAARecomp.apk"
