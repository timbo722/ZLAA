"""
image.py - a loaded program: memory segments, modules, and what we know about its code.

Both loaders (elf_loader.py for test programs, nso_loader.py for the game) produce an Image.
The rest of the recompiler only ever looks at an Image, so it doesn't care where the code came from.
"""
import bisect
import struct


class Segment:
    def __init__(self, name, vaddr, data, perm):
        self.name = name          # e.g. "main.text"
        self.vaddr = vaddr        # guest address of the first byte
        self.data = bytearray(data)
        self.perm = perm          # "rx", "r" or "rw"

    @property
    def end(self):
        return self.vaddr + len(self.data)

    @property
    def executable(self):
        return "x" in self.perm


class Module:
    """One executable file (main, sdk, subsdk0...). Test ELFs are a single module."""

    def __init__(self, name, base):
        self.name = name
        self.base = base
        self.size = 0
        self.build_id = b""
        self.text = (0, 0)                  # (start, end) guest addresses
        self.eh_frame_hdr = None            # (start, end) or None
        self.exports = {}                   # name -> address (defined dynamic symbols)


class Image:
    def __init__(self):
        self.segments = []
        self._starts = []
        self.modules = []
        self.named_funcs = {}       # addr -> (name, size or None): from symbol tables
        self.code_pointers = set()  # addresses known to be called through pointers (relocs, init arrays)
        self.notes = []             # loader warnings for the report

    # ---- building ----
    def add_segment(self, seg):
        idx = bisect.bisect(self._starts, seg.vaddr)
        self.segments.insert(idx, seg)
        self._starts.insert(idx, seg.vaddr)

    # ---- reading/writing guest memory ----
    def find(self, addr):
        idx = bisect.bisect(self._starts, addr) - 1
        if idx >= 0:
            seg = self.segments[idx]
            if addr < seg.end:
                return seg
        return None

    def read(self, addr, n):
        seg = self.find(addr)
        if seg is None or addr + n > seg.end:
            raise ValueError(f"read of {n} bytes at 0x{addr:x} is outside the image")
        off = addr - seg.vaddr
        return bytes(seg.data[off:off + n])

    def u8(self, addr):
        return self.read(addr, 1)[0]

    def u32(self, addr):
        return struct.unpack("<I", self.read(addr, 4))[0]

    def u64(self, addr):
        return struct.unpack("<Q", self.read(addr, 8))[0]

    def write_u64(self, addr, value):
        seg = self.find(addr)
        if seg is None or addr + 8 > seg.end:
            raise ValueError(f"write at 0x{addr:x} is outside the image")
        off = addr - seg.vaddr
        seg.data[off:off + 8] = struct.pack("<Q", value & 0xFFFFFFFFFFFFFFFF)

    def word(self, addr):
        """Fetch one 32-bit instruction word (hot path)."""
        return self.u32(addr)

    # ---- code ----
    def code_segments(self):
        return [s for s in self.segments if s.executable]

    def is_code(self, addr):
        seg = self.find(addr)
        return seg is not None and seg.executable

    def mem_end(self):
        return max(s.end for s in self.segments)
