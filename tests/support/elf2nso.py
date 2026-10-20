#!/usr/bin/env python3
"""
elf2nso.py - turn a test ELF (linked with nso.ld + nso_crt0.s) into an NSO file,
so the NSO loader and function discovery can be tested without any game files.

    python tests/support/elf2nso.py input.elf output.nso
"""
import hashlib
import struct
import sys

import lz4.block
from elftools.elf.elffile import ELFFile


def main(src, dst):
    with open(src, "rb") as f:
        elf = ELFFile(f)
        loads = [s for s in elf.iter_segments() if s["p_type"] == "PT_LOAD"]
        if len(loads) != 3:
            sys.exit(f"{src}: expected 3 PT_LOAD segments (text, rodata, data), got {len(loads)}")
        segs = []
        for s in loads:
            segs.append((s["p_vaddr"], s.data(), s["p_memsz"] - s["p_filesz"]))
        text, ro, data = segs
        bss = data[2]
        ro_base = ro[0]
        sec = {s.name: s for s in elf.iter_sections()}

        def rel_ro(name):
            s = sec.get(name)
            return (s["sh_addr"] - ro_base, s["sh_size"]) if s else (0, 0)

        build_id = b""
        note = sec.get(".note.gnu.build-id")
        if note:
            for n in note.iter_notes():
                if n["n_type"] == "NT_GNU_BUILD_ID":
                    build_id = bytes.fromhex(n["n_desc"])

    header = bytearray(0x100)
    header[0:4] = b"NSO0"
    struct.pack_into("<I", header, 0x0C, 0b111111)   # all segments compressed + hash-checked
    body = bytearray()
    offset = 0x100
    for idx, (vaddr, raw, _) in enumerate(segs):
        comp = lz4.block.compress(raw, store_size=False)
        struct.pack_into("<III", header, 0x10 + idx * 0x10, offset, vaddr, len(raw))
        struct.pack_into("<I", header, 0x60 + idx * 4, len(comp))
        header[0xA0 + idx * 0x20: 0xC0 + idx * 0x20] = hashlib.sha256(raw).digest()
        body += comp
        offset += len(comp)
    struct.pack_into("<I", header, 0x3C, bss)
    header[0x40:0x40 + len(build_id[:0x20])] = build_id[:0x20]
    struct.pack_into("<II", header, 0x90, *rel_ro(".dynstr"))
    struct.pack_into("<II", header, 0x98, *rel_ro(".dynsym"))
    with open(dst, "wb") as f:
        f.write(header + body)


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
