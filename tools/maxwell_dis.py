"""maxwell_dis.py - list the instructions of a dumped Maxwell shader (LAA_DUMP_SHADERS), by name.

    python tools/maxwell_dis.py <shader.code> [--around MNEMONIC]

Names come from the translator's opcode table (runtime/render/shader/maxwell_ops.h). Every 4th
64-bit word is a scheduling word (not an instruction) and is skipped. Only names, not operands:
enough to see which instructions a shader uses and in what order.
"""
import os
import re
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
OPS = re.findall(r"\{ (0x[0-9a-f]+)ull, (0x[0-9a-f]+)ull, OP_\w+, \"(\w+)\" \}",
                 open(os.path.join(HERE, "..", "runtime/render/shader/maxwell_ops.h")).read())
OPS = [(int(m, 16), int(v, 16), n) for m, v, n in OPS]


def name(w):
    for mask, value, n in OPS:
        if w & mask == value:
            return n
    return "?"


def main():
    data = open(sys.argv[1], "rb").read()
    # graphics: the dump starts with a 0x30-byte NVN header, then the 0x50-byte shader header
    start = 0x80 if struct.unpack_from("<I", data, 0)[0] == 0x12345678 else 0
    around = sys.argv[sys.argv.index("--around") + 1] if "--around" in sys.argv else None
    lines = []
    for i in range(start, len(data) - 7, 8):
        if ((i - start) // 8) % 4 == 0:
            continue
        w = struct.unpack_from("<Q", data, i)[0]
        lines.append(f"{i - start:05x}: {w:016x} {name(w)}")
    if around:
        for k, l in enumerate(lines):
            if l.endswith(" " + around):
                print("\n".join(lines[max(0, k - 12):k + 2]))
                print("--")
    else:
        print("\n".join(lines))


if __name__ == "__main__":
    main()
