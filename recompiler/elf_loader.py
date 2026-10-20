"""
elf_loader.py - load a linked ARM64 ELF (our test programs) into an Image.
Function names and sizes come straight from the ELF symbol table.
"""
from elftools.elf.elffile import ELFFile

from image import Image, Module, Segment


def load_elf(path):
    img = Image()
    with open(path, "rb") as f:
        elf = ELFFile(f)
        if elf["e_machine"] != "EM_AARCH64":
            raise ValueError(f"{path}: not an ARM64 ELF")
        mod = Module("elf", 0)
        for i, seg in enumerate(elf.iter_segments()):
            if seg["p_type"] != "PT_LOAD":
                continue
            flags = seg["p_flags"]
            perm = "r" + ("w" if flags & 2 else "") + ("x" if flags & 1 else "")
            data = seg.data() + bytes(seg["p_memsz"] - seg["p_filesz"])  # .bss is zero-filled
            img.add_segment(Segment(f"seg{i}", seg["p_vaddr"], data, perm))
            if flags & 1:
                mod.text = (seg["p_vaddr"], seg["p_vaddr"] + seg["p_memsz"])
        symtab = elf.get_section_by_name(".symtab")
        if symtab is None:
            raise ValueError(f"{path}: no symbol table")
        for sym in symtab.iter_symbols():
            if sym["st_info"]["type"] == "STT_FUNC" and sym["st_size"] > 0:
                img.named_funcs.setdefault(sym["st_value"], (sym.name, sym["st_size"]))
    mod.size = img.mem_end()
    img.modules.append(mod)
    return img
