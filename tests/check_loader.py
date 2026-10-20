#!/usr/bin/env python3
"""
check_loader.py - check the C loader (runtime/nso_loader.c) against the Python one.

The recompiler translated the game from the memory image nso_loader.py built. At run time
the C loader rebuilds that image from the player's dump, before relocation (rtld relocates
it when the game starts, as on hardware); if a single byte differs, the recompiled code would
see different data than it was made for. So: dump the C loader's memory and compare
everything, including zeroed .bss.

    laa_host <exefs> --dump-image ~/laar-build/image.bin
    python tests/check_loader.py <exefs> ~/laar-build/image.bin
"""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "recompiler"))
from nso_loader import load_exefs  # noqa: E402


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    img = load_exefs(sys.argv[1], relocate=False)
    start = img.modules[0].base
    end = img.modules[-1].base + img.modules[-1].size
    expected = bytearray(end - start)          # gaps between segments are zero
    for seg in img.segments:
        expected[seg.vaddr - start:seg.end - start] = seg.data
    got = open(sys.argv[2], "rb").read()

    if len(got) != len(expected):
        sys.exit(f"FAIL: dump is {len(got):#x} bytes, expected {len(expected):#x}")
    if got == expected:
        print(f"PASS: C loader image matches the Python loader ({len(got):,} bytes, "
              f"{len(img.modules)} modules)")
        return 0
    bad = [k for k in range(0, len(got), 8) if got[k:k + 8] != expected[k:k + 8]]
    print(f"FAIL: {len(bad)} of {len(got) // 8} 8-byte words differ")
    for k in bad[:10]:
        addr = start + k
        seg = img.find(addr)
        print(f"  0x{addr:x} ({seg.name if seg else 'gap'}): C {got[k:k + 8].hex()}  "
              f"Python {bytes(expected[k:k + 8]).hex()}")
    return 1


if __name__ == "__main__":
    sys.exit(main())
