"""
discover.py - find every function in the code, even without a symbol table.

The game is stripped: most functions have no name and no recorded size. We combine
several sources of evidence:

  1. symbols    exported dynamic symbols (names + sizes), mostly in the SDK
  2. eh_frame   C++ exception-unwinding tables list the exact start and length of
                (nearly) every compiled function: the best source we have
  3. pointers   relocated pointers into code: vtables, function pointer tables,
                init arrays, module entry points
  4. calls      the target of every BL instruction is a function start
  5. tail calls a B/B.cond/CBZ/TBZ that leaves its function lands on another function

A function without a known size runs until the next known start.
"""
import bisect
import struct

from arm64 import bl_over_literal, branch_info

SOURCES = ("symbol", "eh_frame", "pointer", "call", "tailcall")


# ---------------------------------------------------------------- eh_frame
def _uleb(img, addr):
    result = shift = 0
    while True:
        b = img.u8(addr); addr += 1
        result |= (b & 0x7F) << shift
        shift += 7
        if not b & 0x80:
            return result, addr


def _sleb(img, addr):
    result = shift = 0
    while True:
        b = img.u8(addr); addr += 1
        result |= (b & 0x7F) << shift
        shift += 7
        if not b & 0x80:
            if b & 0x40:
                result -= 1 << shift
            return result, addr


def _encoded(img, addr, enc, datarel=0):
    """Read a DWARF pointer encoding (DW_EH_PE_*). Returns (value, next_addr)."""
    if enc == 0xFF:
        return None, addr
    fmt, app = enc & 0x0F, enc & 0x70
    if fmt == 0x00:   v, n = img.u64(addr), addr + 8
    elif fmt == 0x01: v, n = _uleb(img, addr)
    elif fmt == 0x02: v, n = struct.unpack("<H", img.read(addr, 2))[0], addr + 2
    elif fmt == 0x03: v, n = img.u32(addr), addr + 4
    elif fmt == 0x04: v, n = img.u64(addr), addr + 8
    elif fmt == 0x09: v, n = _sleb(img, addr)
    elif fmt == 0x0A: v, n = struct.unpack("<h", img.read(addr, 2))[0], addr + 2
    elif fmt == 0x0B: v, n = struct.unpack("<i", img.read(addr, 4))[0], addr + 4
    elif fmt == 0x0C: v, n = struct.unpack("<q", img.read(addr, 8))[0], addr + 8
    else:
        raise ValueError(f"unknown pointer encoding 0x{enc:x}")
    if app == 0x10:
        v += addr          # pc-relative
    elif app == 0x30:
        v += datarel       # relative to .eh_frame_hdr
    return v & 0xFFFFFFFFFFFFFFFF, n


def _cie_fde_encoding(img, cie):
    length = img.u32(cie)
    p = cie + (12 if length == 0xFFFFFFFF else 4)
    p += 8 if length == 0xFFFFFFFF else 4     # CIE id
    p += 1                                     # version
    aug = bytearray()
    while img.u8(p):
        aug.append(img.u8(p)); p += 1
    p += 1
    aug = aug.decode()
    if "eh" in aug:
        p += 8
    _, p = _uleb(img, p)   # code alignment
    _, p = _sleb(img, p)   # data alignment
    _, p = _uleb(img, p)   # return address register
    if not aug.startswith("z"):
        return 0x00
    _, p = _uleb(img, p)   # augmentation data length
    for ch in aug[1:]:
        if ch == "R":
            return img.u8(p)
        if ch == "P":
            enc = img.u8(p)
            _, p = _encoded(img, p + 1, enc & 0x7F)
        elif ch == "L":
            p += 1
    return 0x00


def eh_frame_functions(img, hdr_start):
    """Parse .eh_frame_hdr's sorted lookup table, then each FDE for its pc range."""
    version, ptr_enc, count_enc, table_enc = img.read(hdr_start, 4)
    if version != 1:
        raise ValueError("unknown .eh_frame_hdr version")
    _, p = _encoded(img, hdr_start + 4, ptr_enc, hdr_start)
    count, p = _encoded(img, p, count_enc, hdr_start)
    cie_cache, out = {}, []
    for _ in range(count or 0):
        start, p = _encoded(img, p, table_enc, hdr_start)
        fde, p = _encoded(img, p, table_enc, hdr_start)
        length = img.u32(fde)
        q = fde + 4
        if length == 0xFFFFFFFF:
            q += 8
        cie = q - img.u32(q)
        q += 4
        if cie not in cie_cache:
            cie_cache[cie] = _cie_fde_encoding(img, cie)
        enc = cie_cache[cie]
        _, q = _encoded(img, q, enc)            # pc_begin (we already have it)
        size, _ = _encoded(img, q, enc & 0x0F)   # pc_range: same format, no pc-relative
        out.append((start, size))
    return out


# ---------------------------------------------------------------- discovery
class Functions:
    """Result: start -> (name or None, size), plus where each start came from."""

    def __init__(self):
        self.starts = {}    # addr -> source
        self.names = {}     # addr -> name
        self.sizes = {}     # addr -> exact size (from symbol or eh_frame)
        self.funcs = {}     # final: addr -> (name, size)

    def add(self, addr, source, name=None, size=None):
        if addr not in self.starts:
            self.starts[addr] = source
        if name and addr not in self.names:
            self.names[addr] = name
        if size and addr not in self.sizes:
            self.sizes[addr] = size

    def counts(self):
        out = {s: 0 for s in SOURCES}
        for src in self.starts.values():
            out[src] += 1
        return out


def _finalize(img, fs):
    code = sorted((s.vaddr, s.end) for s in img.code_segments())
    starts = sorted(a for a in fs.starts if img.is_code(a) and a % 4 == 0)
    fs.funcs = {}
    for k, a in enumerate(starts):
        seg_end = next(e for s, e in code if s <= a < e)
        nxt = starts[k + 1] if k + 1 < len(starts) else seg_end
        size = fs.sizes.get(a) or (min(nxt, seg_end) - a)
        size = min(size, seg_end - a)
        if size > 0:
            fs.funcs[a] = (fs.names.get(a) or f"sub_{a:x}", size)
    return starts


def discover(img, log=print):
    fs = Functions()
    for addr, (name, size) in img.named_funcs.items():
        fs.add(addr, "symbol", name, size)
    for mod in img.modules:
        if mod.eh_frame_hdr:
            try:
                for start, size in eh_frame_functions(img, mod.eh_frame_hdr[0]):
                    if size:
                        fs.add(start, "eh_frame", size=size)
            except (ValueError, KeyError) as e:
                img.notes.append(f"{mod.name}: could not parse .eh_frame_hdr ({e})")
    for addr in img.code_pointers:
        fs.add(addr, "pointer")

    # One linear scan over every code word: BL targets are function starts, and
    # we remember every direct jump (B, B.cond, CBZ, TBZ) for the tail-call pass.
    jumps = []
    for seg in img.code_segments():
        data, base = seg.data, seg.vaddr
        for off in range(0, len(data) - 3, 4):
            w = int.from_bytes(data[off:off + 4], "little")
            top = w >> 24
            # quick filter: only the branch encodings (bits 30:26 = 00101, or 0x34-0x37/0xb4-0xb7/0x54)
            if (w >> 26) & 0x1F != 0b00101 and top & 0x7C != 0x34 and top != 0x54:
                continue
            pc = base + off
            info = branch_info(w, pc)
            if not info or not img.is_code(info[1] or -1):
                continue
            if info[0] == "bl":
                fs.add(info[1], "call")
                ptr = bl_over_literal(img.word, pc)
                if ptr is not None and img.is_code(ptr):
                    fs.add(ptr, "pointer")
            elif info[0] in ("b", "cond"):
                jumps.append((pc, info[1]))

    # Tail calls: a jump that leaves its function lands on (the start of) another one.
    # Each new start can split a function, so repeat until nothing changes.
    for _ in range(10):
        starts = _finalize(img, fs)
        new = 0
        for pc, target in jumps:
            k = bisect.bisect(starts, pc) - 1
            if k < 0:
                continue
            start = starts[k]
            size = fs.funcs.get(start, (None, 0))[1]
            if not (start <= target < start + size) and target not in fs.starts:
                fs.add(target, "tailcall")
                new += 1
        if not new:
            break
    _finalize(img, fs)
    return fs
