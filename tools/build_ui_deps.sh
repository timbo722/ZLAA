#!/bin/bash
# build_ui_deps.sh [desktop|android|both] - build the menu's libraries: RmlUi (HTML/CSS-like UI documents, the
# library Dusklight's menus use; MIT) and FreeType (its font rasterizer; FreeType License), as static libraries.
#
#   desktop: installed into $LAA_DEPS_ROOT/usr (default ~/laar-build/deps/root/usr), where CMake finds SDL3 too
#   android: installed into $LAA_ANDROID/prefix (default ~/laar-build/android/prefix), next to SDL3
#
# Sources are downloaded once into ~/laar-build/deps/src (outside the repo).
set -euo pipefail

RMLUI=6.3; FREETYPE=VER-2-14-3
SRC=$HOME/laar-build/deps/src; mkdir -p "$SRC"
A=${LAA_ANDROID:-$HOME/laar-build/android}
DESKTOP_PREFIX=${LAA_DEPS_ROOT:-$HOME/laar-build/deps/root}/usr
[ -d "$SRC/RmlUi-$RMLUI" ] || git clone -q --depth 1 -b $RMLUI https://github.com/mikke89/RmlUi.git "$SRC/RmlUi-$RMLUI"
[ -d "$SRC/freetype-$FREETYPE" ] || git clone -q --depth 1 -b $FREETYPE https://gitlab.freedesktop.org/freetype/freetype.git "$SRC/freetype-$FREETYPE"

build() {   # build <name> <prefix> <cmake args...>
    local name=$1 prefix=$2; shift 2
    local b=$HOME/laar-build/deps/build-$name
    # FreeType without its optional helpers (zlib, bzip2, PNG, HarfBuzz, Brotli): plain TrueType/OpenType fonts
    cmake -S "$SRC/freetype-$FREETYPE" -B "$b/freetype" -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF \
        -DCMAKE_POSITION_INDEPENDENT_CODE=ON -DCMAKE_INSTALL_PREFIX="$prefix" -DFT_DISABLE_ZLIB=ON -DFT_DISABLE_BZIP2=ON \
        -DFT_DISABLE_PNG=ON -DFT_DISABLE_HARFBUZZ=ON -DFT_DISABLE_BROTLI=ON "$@" > /dev/null
    cmake --build "$b/freetype" > /dev/null && cmake --install "$b/freetype" > /dev/null
    cmake -S "$SRC/RmlUi-$RMLUI" -B "$b/rmlui" -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF \
        -DCMAKE_POSITION_INDEPENDENT_CODE=ON -DCMAKE_INSTALL_PREFIX="$prefix" -DCMAKE_PREFIX_PATH="$prefix" \
        -DRMLUI_SAMPLES=OFF -DRMLUI_PRECOMPILED_HEADERS=OFF "$@" > /dev/null
    cmake --build "$b/rmlui" > /dev/null && cmake --install "$b/rmlui" > /dev/null
    echo "$name: RmlUi $RMLUI + FreeType $FREETYPE installed in $prefix"
}

what=${1:-both}
if [ "$what" = desktop ] || [ "$what" = both ]; then build desktop "$DESKTOP_PREFIX"; fi
if [ "$what" = android ] || [ "$what" = both ]; then
    NDK=$(ls -d "$A"/sdk/ndk/* | head -1)
    build android "$A/prefix" -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" -DANDROID_ABI=arm64-v8a \
        -DANDROID_PLATFORM=android-30 -DCMAKE_FIND_ROOT_PATH="$A/prefix"
fi
