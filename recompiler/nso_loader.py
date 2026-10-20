"""
nso_loader.py - load Switch NSO executables (the game's ExeFS) into an Image.

What a game's ExeFS contains:
    rtld        Nintendo's dynamic linker (runs first on real hardware)
    main        the game itself
    subsdk0..9  extra libraries some games ship (optional)
    sdk         the Nintendo SDK: C/C++ libraries + wrappers around OS services
    main.npdm   permissions/metadata (not code, ignored here)

On a real Switch the kernel loads these one after another in that order, then rtld
links them together. We do the same thing ahead of time: load every module at a fixed
address, then apply the relocations (fill in pointers, resolve imports between modules).

NSO format (https://switchbrew.org/wiki/NSO): a 0x100-byte header, then the .text, .rodata
and .data segments, each optionally compressed with LZ4 (block format).
MOD0 (from libnx's crt0): the word at text+4 is the offset of a "MOD0" header that points
at the module's dynamic section, .bss and .eh_frame_hdr.
"""
import hashlib
import os
import struct

from image import Image, Module, Segment

try:
    import lz4.block
except ImportError:
    lz4 = None

# Same convention as yuzu/Ryujinx: the first module is loaded at 0x8000000.
DEFAULT_BASE = 0x8000000
MODULE_ORDER = ["rtld", "main"] + [f"subsdk{i}" for i in range(10)] + ["sdk"]

PAGE = 0x1000

# ELF dynamic section tags we use
DT_NULL, DT_NEEDED, DT_PLTRELSZ, DT_HASH, DT_STRTAB, DT_SYMTAB, DT_RELA, DT_RELASZ = 0, 1, 2, 4, 5, 6, 7, 8
DT_STRSZ, DT_INIT, DT_FINI, DT_JMPREL = 10, 12, 13, 23
DT_INIT_ARRAY, DT_FINI_ARRAY, DT_INIT_ARRAYSZ, DT_FINI_ARRAYSZ = 25, 26, 27, 28

# AArch64 relocation types
R_AARCH64_ABS64 = 257
R_AARCH64_GLOB_DAT = 1025
R_AARCH64_JUMP_SLOT = 1026
R_AARCH64_RELATIVE = 1027
RELOC_NAMES = {257: "ABS64", 1025: "GLOB_DAT", 1026: "JUMP_SLOT", 1027: "RELATIVE",
               1028: "TLS_DTPMOD64", 1029: "TLS_DTPREL64", 1030: "TLS_TPREL64", 1031: "TLSDESC"}

STT_FUNC = 2
SHN_UNDEF = 0
STB_WEAK = 2


def align(v, a=PAGE):
    return (v + a - 1) & ~(a - 1)


def lz4_decompress(data, size):
    if lz4 is not None:
        return lz4.block.decompress(data, uncompressed_size=size)
    return _lz4_decompress_py(data, size)


def _lz4_decompress_py(src, size):
    """Plain-Python LZ4 block decoder (fallback if the lz4 package is missing; slow)."""
    out = bytearray()
    i = 0
    while i < len(src):
        token = src[i]; i += 1
        lit = token >> 4
        if lit == 15:
            while True:
                b = src[i]; i += 1; lit += b
                if b != 255:
                    break
        out += src[i:i + lit]; i += lit
        if i >= len(src):
            break
        offset = src[i] | (src[i + 1] << 8); i += 2
        mlen = token & 15
        if mlen == 15:
            while True:
                b = src[i]; i += 1; mlen += b
                if b != 255:
                    break
        mlen += 4
        start = len(out) - offset
        for k in range(mlen):          # byte by byte: matches may overlap themselves
            out.append(out[start + k])
    if len(out) != size:
        raise ValueError(f"LZ4: expected {size} bytes, got {len(out)}")
    return bytes(out)


class NsoFile:
    """One parsed NSO file (not yet placed in memory)."""

    def __init__(self, path, verify_hashes=True):
        self.path = path
        raw = open(path, "rb").read()
        if raw[:4] != b"NSO0":
            raise ValueError(f"{path}: not an NSO file (magic {raw[:4]!r})")
        h = raw[:0x100]
        (self.version, _, self.flags) = struct.unpack_from("<III", h, 0x04)
        if self.flags & 0x80:
            raise ValueError(f"{path}: uses zstd compression (firmware 22+ format), not supported yet")
        segs = []
        for idx, off in enumerate((0x10, 0x20, 0x30)):
            file_off, mem_off, size = struct.unpack_from("<III", h, off)
            comp_size = struct.unpack_from("<I", h, 0x60 + idx * 4)[0]
            segs.append((file_off, mem_off, size, comp_size))
        self.bss_size = struct.unpack_from("<I", h, 0x3C)[0]
        self.build_id = h[0x40:0x60]
        self.dynstr = struct.unpack_from("<II", h, 0x90)   # (offset from .rodata, size)
        self.dynsym = struct.unpack_from("<II", h, 0x98)
        hashes = [h[0xA0:0xC0], h[0xC0:0xE0], h[0xE0:0x100]]

        self.segments = []   # (mem_offset, bytes)
        for idx, (file_off, mem_off, size, comp_size) in enumerate(segs):
            if self.flags & (1 << idx):
                data = lz4_decompress(raw[file_off:file_off + comp_size], size)
            else:
                data = raw[file_off:file_off + size]
            if len(data) != size:
                raise ValueError(f"{path}: segment {idx} is truncated")
            if verify_hashes and self.flags & (8 << idx) and hashlib.sha256(data).digest() != hashes[idx]:
                raise ValueError(f"{path}: segment {idx} hash mismatch (corrupt or incomplete dump?)")
            self.segments.append((mem_off, data))

    @property
    def image_size(self):
        mem_off, data = self.segments[2]
        return align(mem_off + len(data) + self.bss_size)


def _symbols(img, mod, symtab, strtab, count):
    """Read Elf64_Sym entries: (name, value, size, type, bind, defined)."""
    syms = []
    for k in range(count):
        e = img.read(symtab + k * 24, 24)
        st_name, st_info, _, st_shndx, st_value, st_size = struct.unpack("<IBBHQQ", e)
        name = _cstr(img, strtab + st_name) if st_name else ""
        syms.append((name, st_value, st_size, st_info & 0xF, st_info >> 4, st_shndx != SHN_UNDEF))
    return syms


def _cstr(img, addr):
    out = bytearray()
    while True:
        b = img.u8(addr)
        if b == 0:
            return out.decode("utf-8", "replace")
        out.append(b)
        addr += 1


def load_exefs(paths, base=DEFAULT_BASE, verify_hashes=True, relocate=True):
    """Load a list of NSO files (or one ExeFS directory) into one Image, linked together.

    relocate=False leaves memory exactly as the kernel loads it (rtld links it later, at run
    time); relocations are still read, so code_pointers is the same either way."""
    if isinstance(paths, str) and os.path.isdir(paths):
        found = [os.path.join(paths, n) for n in MODULE_ORDER if os.path.isfile(os.path.join(paths, n))]
        if not found:
            raise ValueError(f"{paths}: no NSO modules found (expected files named main, sdk, rtld, ...)")
        paths = found
    elif isinstance(paths, str):
        paths = [paths]

    img = Image()
    loaded = []   # (Module, NsoFile, dyn info)
    addr = base
    # ---- 1. place every module in memory ----
    for path in paths:
        nso = NsoFile(path, verify_hashes)
        mod = Module(os.path.basename(path), addr)
        mod.size = nso.image_size
        mod.build_id = nso.build_id
        names = ("text", "rodata", "data")
        perms = ("rx", "r", "rw")
        for idx, (mem_off, data) in enumerate(nso.segments):
            if idx == 2:  # .data is followed by zero-filled .bss
                data = data + bytes(mod.size - mem_off - len(data))
            img.add_segment(Segment(f"{mod.name}.{names[idx]}", addr + mem_off, data, perms[idx]))
        text_off, text = nso.segments[0]
        mod.text = (addr + text_off, addr + text_off + len(text))
        img.modules.append(mod)
        loaded.append((mod, nso))
        addr += mod.size

    # ---- 2. read each module's MOD0 + dynamic section + symbols ----
    infos = []
    for mod, nso in loaded:
        info = {"dyn": {}, "syms": []}
        mod0 = mod.base + img.u32(mod.text[0] + 4)
        if img.read(mod0, 4) != b"MOD0":
            img.notes.append(f"{mod.name}: no MOD0 header found; skipping dynamic linking for it")
            infos.append(info)
            continue
        dyn_off, bss_start, bss_end, ehs, ehe = struct.unpack("<iiiii", img.read(mod0 + 4, 20))
        if ehe > ehs:
            mod.eh_frame_hdr = (mod0 + ehs, mod0 + ehe)
        dyn = mod0 + dyn_off
        tags = {}
        while True:
            tag, val = struct.unpack("<QQ", img.read(dyn, 16))
            if tag == DT_NULL:
                break
            tags.setdefault(tag, val)
            dyn += 16
        info["dyn"] = tags
        # Symbol count isn't in the dynamic section; the NSO header records the table size.
        ro_off = nso.segments[1][0]
        symtab = mod.base + (tags[DT_SYMTAB] if DT_SYMTAB in tags else ro_off + nso.dynsym[0])
        strtab = mod.base + (tags[DT_STRTAB] if DT_STRTAB in tags else ro_off + nso.dynstr[0])
        info["syms"] = _symbols(img, mod, symtab, strtab, nso.dynsym[1] // 24)
        for name, value, size, typ, bind, defined in info["syms"]:
            if defined and name:
                mod.exports.setdefault(name, mod.base + value)
                if typ == STT_FUNC:
                    img.named_funcs.setdefault(mod.base + value, (name, size or None))
        infos.append(info)

    # Global symbol lookup in load order (main first, then subsdks, then sdk), like rtld.
    def lookup(name):
        for m in img.modules:
            if name in m.exports:
                return m.exports[name]
        return None

    # ---- 3. relocations ----
    unresolved, unknown = {}, {}
    for (mod, nso), info in zip(loaded, infos):
        tags = info["dyn"]
        tables = []
        if DT_RELA in tags:
            tables.append((tags[DT_RELA], tags.get(DT_RELASZ, 0)))
        if DT_JMPREL in tags:
            tables.append((tags[DT_JMPREL], tags.get(DT_PLTRELSZ, 0)))
        for table, size in tables:
            for k in range(size // 24):
                off, rinfo, addend = struct.unpack("<QQq", img.read(mod.base + table + k * 24, 24))
                rtype, sym_idx = rinfo & 0xFFFFFFFF, rinfo >> 32
                where = mod.base + off
                if rtype == R_AARCH64_RELATIVE:
                    value = mod.base + addend
                elif rtype in (R_AARCH64_GLOB_DAT, R_AARCH64_JUMP_SLOT, R_AARCH64_ABS64):
                    if rtype == R_AARCH64_JUMP_SLOT:
                        # Lazy binding: until rtld resolves it, the slot holds the offset of
                        # PLT0, which calls rtld's resolver. That stub must be recompiled too.
                        plt0 = mod.base + img.u64(where)
                        if img.is_code(plt0):
                            img.code_pointers.add(plt0)
                    name, svalue, _, _, bind, defined = info["syms"][sym_idx]
                    if defined:
                        target = mod.base + svalue
                    else:
                        target = lookup(name)
                        if target is None:
                            if bind != STB_WEAK:
                                unresolved[name] = unresolved.get(name, 0) + 1
                            target = 0
                    value = target + (addend if rtype != R_AARCH64_JUMP_SLOT else 0)
                else:
                    label = RELOC_NAMES.get(rtype, str(rtype))
                    unknown[label] = unknown.get(label, 0) + 1
                    continue
                if relocate:
                    img.write_u64(where, value)
                if img.is_code(value):
                    img.code_pointers.add(value)
        for tag_arr, tag_sz in ((DT_INIT_ARRAY, DT_INIT_ARRAYSZ), (DT_FINI_ARRAY, DT_FINI_ARRAYSZ)):
            if tag_arr in tags:
                for k in range(tags.get(tag_sz, 0) // 8):
                    ptr = img.u64(mod.base + tags[tag_arr] + k * 8)
                    if img.is_code(ptr):
                        img.code_pointers.add(ptr)
        for tag in (DT_INIT, DT_FINI):
            if tag in tags:
                img.code_pointers.add(mod.base + tags[tag])
        img.code_pointers.add(mod.text[0])   # module entry point

    if unresolved:
        img.notes.append(f"{len(unresolved)} imported symbols not found in any module "
                         f"(e.g. {', '.join(sorted(unresolved)[:5])})")
    if unknown:
        img.notes.append("relocation types not handled yet: " +
                         ", ".join(f"{k} x{v}" for k, v in sorted(unknown.items())))
    return img
