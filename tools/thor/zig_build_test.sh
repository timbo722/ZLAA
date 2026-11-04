#!/bin/bash
# zig_build_test.sh <zig tarball> [gen dir]: build the game library ON THE THOR with zig cc (the Stage 4 experiment:
# can a player's device build the game code?). Packs the generated C + runtime headers + libmain.so, pushes them and
# zig (zig-aarch64-linux-<v>.tar.xz from ziglang.org, trimmed to what compiling needs) to /data/local/tmp/zt,
# compiles 2 chunks at a time (each waits while a CPU zone is over 70 C: 6 at once reached 94 C), links with
# zig's ld.lld against libmain.so (no libc needed) and pulls the result to $LAA_HOST_DIR/game as
# libgame-<id>.ondevice-zig.so (try it with ab_game.sh). Deletes /data/local/tmp/zt afterwards.
# 2026-11-01 (1.0.0, zig 0.14.1): 292 chunks ~5-11 s each, link < 1 s, game speed the same as the NDK build.
. "$(dirname "$0")/env.sh"
set -e
ZT=$1; GEN=${2:-$HOME/laar-build/gen}
T=$(mktemp -d); R=$(cd "$(dirname "$0")/../.." && pwd)
ID=$(sed -n 's/set(GAME_BUILD_ID \(.*\))/\1/p' $GEN/sources.cmake)
ABI=$(grep -o 'RECOMP_ABI=\\"[0-9a-f]*' ~/laar-build/android/build/game/build.ninja | head -1 | sed 's/.*"//')
tar xf $ZT -C $T; Z=$(ls -d $T/zig-*); mkdir -p $T/pkg/zig/lib/libc/include $T/pkg/gamesrc
cp $Z/zig $T/pkg/zig/; cp -r $Z/lib/include $Z/lib/std $Z/lib/compiler $T/pkg/zig/lib/
cp -r $Z/lib/libc/include/{generic-musl,aarch64-linux-musl,any-linux-any} $T/pkg/zig/lib/libc/include/
cp $GEN/*.c $GEN/funcs.h $R/runtime/recomp_runtime.h $R/runtime/recomp_simd.h $R/runtime/game_abi.c $T/pkg/gamesrc/
cp ~/laar-build/android/build/game/libmain.so $T/pkg/gamesrc/
cat > $T/pkg/build.sh <<EOF
cd /data/local/tmp/zt; export ZIG_GLOBAL_CACHE_DIR=\$PWD/cache ZIG_LOCAL_CACHE_DIR=\$PWD/cache; mkdir -p obj
cat > cool.sh <<'X'
while :; do m=0; for z in /sys/class/thermal/thermal_zone*; do case \$(cat \$z/type) in cpu-*) v=\$(cat \$z/temp 2>/dev/null); [ "\${v:-0}" -gt \$m ] && m=\$v;; esac; done; [ \$m -lt 70000 ] && break; sleep 10; done
X
cat > cc.sh <<'X'
sh cool.sh; ./zig/zig cc -target aarch64-linux-musl -mcpu=generic+v8_2a -O2 -g0 -fPIC -fwrapv -ffp-contract=off -fno-math-errno -w -fvisibility=hidden -fdata-sections -ffunction-sections -funwind-tables "-DRECOMP_ABI=\"$ABI\"" -Igamesrc -c \$1 -o obj/\$(basename \$1 .c).o || echo "FAILED \$1"
X
t0=\$(date +%s); ls gamesrc/*.c | xargs -P 2 -n 1 sh cc.sh; t1=\$(date +%s)
./zig/zig ld.lld -shared -o libgame-$ID.so obj/*.o gamesrc/libmain.so -soname libgame-$ID.so --gc-sections -z noexecstack
echo "compiled \$(ls obj | wc -l) chunks in \$((t1 - t0)) s, linked in \$((\$(date +%s) - t1)) s"
EOF
tar czf $X/transfer/zt.tgz -C $T/pkg .; rm -rf $T
$W shell "rm -rf /data/local/tmp/zt; mkdir -p /data/local/tmp/zt"
$W push "$(xa transfer/zt.tgz)" /data/local/tmp/zt/ > /dev/null
$W shell "cd /data/local/tmp/zt && tar xzf zt.tgz && rm zt.tgz && sh build.sh"
$W pull /data/local/tmp/zt/libgame-$ID.so "$(xa game/libgame-$ID.ondevice-zig.so)" | tail -1
$W shell "rm -rf /data/local/tmp/zt"
