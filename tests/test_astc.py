"""test_astc.py - check runtime/render/astc.c against ARM's reference codec (astcenc).

    pip install astc_encoder_py
    python tests/test_astc.py [--random N]

Two kinds of input, for each block size and for linear / sRGB:
  1. images (noise, gradients, edges, alpha) compressed by astcenc: realistic blocks;
  2. random 128-bit blocks: every corner of the format, including invalid blocks.
Our decoder must match astcenc's decompressor byte for byte, except for blocks we reject (error
colour: the specification's magenta). astcenc gives opaque white for invalid blocks, and decodes
HDR endpoint modes even in LDR textures, where we follow the specification (error colour).
Such blocks are counted, not failed: textures made for LDR never contain them.
"""
import argparse
import ctypes
import os
import random
import subprocess
import sys
import tempfile

import astc_encoder as astc

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
SIZES = [(4, 4), (5, 4), (5, 5), (6, 5), (6, 6), (8, 5), (8, 6), (8, 8), (10, 5), (10, 6), (10, 8), (10, 10),
         (12, 10), (12, 12)]


def build_lib(tmp):
    lib = os.path.join(tmp, "libastc.so")
    subprocess.run(["cc", "-O2", "-shared", "-fPIC", "-o", lib, os.path.join(ROOT, "runtime/render/astc.c")], check=True)
    so = ctypes.CDLL(lib)
    so.astc_decode.argtypes = [ctypes.c_char_p, ctypes.c_size_t, ctypes.c_uint32, ctypes.c_uint32, ctypes.c_int,
                               ctypes.c_int, ctypes.c_int, ctypes.c_char_p, ctypes.c_size_t]
    return so


def ours(so, blocks, w, h, bw, bh, srgb):
    out = ctypes.create_string_buffer(w * h * 4)
    bx = (w + bw - 1) // bw
    so.astc_decode(blocks, bx * 16, w, h, bw, bh, srgb, out, w * 4)
    return out.raw


def reference(blocks, w, h, bw, bh, srgb):
    profile = astc.ASTCProfile.LDR_SRGB if srgb else astc.ASTCProfile.LDR
    ctx = astc.ASTCContext(astc.ASTCConfig(profile, bw, bh, 1, 60))
    img = astc.ASTCImage(astc.ASTCType.U8, w, h, 1)
    ctx.decompress(blocks, img, astc.ASTCSwizzle())
    return bytes(img.data)


def compress(pixels, w, h, bw, bh, srgb, quality):
    profile = astc.ASTCProfile.LDR_SRGB if srgb else astc.ASTCProfile.LDR
    ctx = astc.ASTCContext(astc.ASTCConfig(profile, bw, bh, 1, quality))
    return ctx.compress(astc.ASTCImage(astc.ASTCType.U8, w, h, 1, pixels), astc.ASTCSwizzle())


def test_images(rng, w=64, h=48):
    out = []
    out.append(bytes(rng.randrange(256) for _ in range(w * h * 4)))                       # noise
    out.append(bytes(v for y in range(h) for x in range(w)                                 # gradients
                     for v in (x * 4 & 255, y * 5 & 255, (x + y) * 2 & 255, 255)))
    out.append(bytes(v for y in range(h) for x in range(w)                                 # edges + alpha
                     for v in ((255, 30, 30, 255) if (x // 7 + y // 5) % 2 else (20, 200, 90, x * 4 & 255))))
    out.append(bytes(v for y in range(h) for x in range(w)                                 # soft greys
                     for v in ((x * y) & 255,) * 3 + (200,)))
    return w, h, out


def first_difference(a, b, w):
    for i in range(0, len(a), 4):
        if a[i:i + 4] != b[i:i + 4]:
            return f"texel ({i // 4 % w}, {i // 4 // w}): ours {tuple(a[i:i + 4])} vs astcenc {tuple(b[i:i + 4])}"
    return ""


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--random", type=int, default=2000, help="random blocks per block size and profile")
    ap.add_argument("--seed", type=int, default=1)
    args = ap.parse_args()
    rng = random.Random(args.seed)
    failures = 0
    with tempfile.TemporaryDirectory() as tmp:
        so = build_lib(tmp)
        w, h, images = test_images(rng)
        for bw, bh in SIZES:
            for srgb in (0, 1):
                name = f"{bw}x{bh}{' sRGB' if srgb else ''}"
                for k, pixels in enumerate(images):
                    for quality in (10, 60):
                        blocks = compress(pixels, w, h, bw, bh, srgb, quality)
                        a, b = ours(so, blocks, w, h, bw, bh, srgb), reference(blocks, w, h, bw, bh, srgb)
                        if a != b:
                            failures += 1
                            print(f"FAIL {name} image {k} quality {quality}: {first_difference(a, b, w)}")
                # random blocks, decoded one at a time so a failure names its block
                n = args.random
                blocks = bytes(rng.randrange(256) for _ in range(16 * n))
                a = ours(so, blocks, bw * n, bh, bw, bh, srgb)
                b = reference(blocks, bw * n, bh, bw, bh, srgb)
                def block(img, i):
                    return b"".join(img[(y * bw * n + i * bw) * 4:(y * bw * n + i * bw + bw) * 4] for y in range(bh))
                magenta, white = bytes((255, 0, 255, 255)) * (bw * bh), bytes((255,) * 4) * (bw * bh)
                bad = [i for i in range(n) if block(a, i) != block(b, i) and block(a, i) != magenta]
                rejected = sum(1 for i in range(n) if block(a, i) == magenta and block(b, i) not in (magenta, white))
                if bad:
                    failures += 1
                    i = bad[0]
                    print(f"FAIL {name}: {len(bad)}/{n} random blocks differ; first {blocks[16 * i:16 * i + 16].hex()}")
                else:
                    print(f"ok   {name}: {len(images) * 2} images, {n} random blocks "
                          f"({rejected} HDR-mode blocks rejected, as the specification says)")
    print("all passed" if not failures else f"{failures} failures")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
