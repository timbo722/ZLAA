#!/usr/bin/env python3
"""
recomp.py - statically recompile ARM64 code into C.

Input can be:
  * a linked ARM64 ELF (our test programs; uses the symbol table)
  * a single .nso file
  * an ExeFS folder (rtld, main, subsdk*, sdk): the game

Examples:
  python recompiler/recomp.py build/test.elf -o build/recomp.c --embed-image
  python recompiler/recomp.py D:/LADump/exefs --report build/la_report.txt
  python recompiler/recomp.py D:/LADump/exefs -o build/la_c/ --report build/la_report.txt

With -o ending in ".c" everything goes in one file; otherwise -o is a folder and the
output is split into chunks (the game is far too big for one C file).
"""
import argparse
import os
import sys
import time
from collections import Counter

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from arm64 import Translator, Unsupported, branch_info, hx  # noqa: E402
from discover import discover  # noqa: E402

try:
    import capstone
    _cs = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
except ImportError:
    _cs = None

STACK_SIZE = 1 << 20          # 1 MiB guest stack for tests
FUNCS_PER_CHUNK = 2000


def disasm(word, pc):
    """Human-readable disassembly (capstone is only used for comments and the report)."""
    if not _cs:
        return ""
    for ins in _cs.disasm(word.to_bytes(4, "little"), pc):
        return f"{ins.mnemonic} {ins.op_str}".strip()
    return "<invalid>"


def load(path, base):
    with open(path, "rb") if os.path.isfile(path) else open(os.devnull, "rb") as f:
        magic = f.read(4)
    if magic == b"\x7fELF":
        from elf_loader import load_elf
        return load_elf(path)
    from nso_loader import load_exefs
    return load_exefs(path, base=base)


def cname(addr):
    return f"f_{addr:x}"


class Stats:
    def __init__(self):
        self.instructions = 0
        self.unsupported = Counter()       # mnemonic -> count
        self.unsupported_why = Counter()   # reason -> count
        self.unsupported_funcs = 0
        self.bad_jumps = 0                 # direct jumps to an address that isn't a known function start


class FuncContext:
    def __init__(self, funcs, start, size, labels):
        self.funcs, self.start, self.end, self.labels = funcs, start, start + size, labels

    def local_label(self, addr):
        if self.start <= addr < self.end:
            self.labels.add(addr)
            return f"L_{addr:x}"
        return None

    def func_name(self, addr):
        return cname(addr) if addr in self.funcs else None


def recompile_function(img, funcs, start, name, size, stats, comments=True):
    words = [img.word(pc) for pc in range(start, start + size, 4)]
    # Pass 1: which addresses are branch targets inside this function? Is there a BR (jump table)?
    labels, has_br = set(), False
    for k, w in enumerate(words):
        info = branch_info(w, start + 4 * k)
        if info and info[0] in ("b", "cond") and start <= info[1] < start + size:
            labels.add(info[1])
        if info and info[0] == "br":
            has_br = True
    if has_br:  # an indirect jump could land on any instruction
        labels.update(range(start, start + size, 4))

    # Pass 2: translate
    ctx = FuncContext(funcs, start, size, labels)
    tr = Translator(ctx)
    body, last_info, func_bad = [], None, False
    for k, insn in enumerate(words):
        pc = start + 4 * k
        if pc in labels:
            body.append(f"L_{pc:x}:;")
        try:
            stmts = tr.translate(insn, pc)
        except Unsupported as e:
            text = disasm(insn, pc)
            stats.unsupported[text.split(" ")[0] or f"0x{insn:08x}"] += 1
            stats.unsupported_why[str(e)] += 1
            func_bad = True
            stmts = [f"recomp_unimplemented(c, {hx(pc)}, 0x{insn:08x}u); /* {e} */"]
        stats.instructions += 1
        stats.bad_jumps += sum(s.count("recomp_bad_jump") for s in stmts)
        if comments:
            body.append(f"    /* {pc:x}: {disasm(insn, pc)} */")
        body.extend("    " + s for s in stmts)
        last_info = branch_info(insn, pc)
    stats.unsupported_funcs += func_bad

    # If the last instruction can fall through, continue at the next address.
    if not last_info or last_info[0] in ("cond", "bl", "blr"):
        body.append("    " + tr.jump(start + size - 4, start + size))

    out = [f"/* {name} */", f"void {cname(start)}(CPU *c) {{"]
    if has_br:
        out.append("    uint64_t jt;")
    out.extend(body)
    if has_br:
        out.append("dispatch:")
        out.append("    switch (jt) {")
        for pc in range(start, start + size, 4):
            out.append(f"    case {hx(pc)}: goto L_{pc:x};")
        out.append("    default: recomp_call_addr(c, jt); return;  /* indirect tail call */")
        out.append("    }")
    out.append("}")
    return out


def emit_image(img):
    out = ["", "/* ---- program memory image (test builds only) ---- */", "#include <stdlib.h>"]
    for idx, seg in enumerate(img.segments):
        data = bytes(seg.data).rstrip(b"\0")   # trailing zeros (.bss) come from calloc
        out.append(f"static const uint8_t seg{idx}[{max(len(data), 1)}] = {{")
        for k in range(0, len(data), 24):
            out.append("    " + ",".join(str(b) for b in data[k:k + 24]) + ",")
        out.append("};")
    mem_size = ((img.mem_end() + 0xFFFF) & ~0xFFFF) + STACK_SIZE
    out += [
        f"const uint64_t recomp_mem_size = {hx(mem_size)};",
        f"const uint64_t recomp_stack_top = {hx(mem_size - 16)};",
        "void recomp_init(CPU *c) {",
        "    memset(c, 0, sizeof *c);",
        "    c->mem = calloc(1, recomp_mem_size);",
        "    c->mem_size = recomp_mem_size;",
    ]
    for idx, seg in enumerate(img.segments):
        n = len(bytes(seg.data).rstrip(b"\0"))
        if n:
            out.append(f"    memcpy(c->mem + {hx(seg.vaddr)}, seg{idx}, {n});")
    out += ["    c->sp = recomp_stack_top;", "    c->excl_addr = ~0ull;", "}"]
    return out


# Build IDs of Link's Awakening's main executable (source: https://zeldamods.org/las/Versions)
KNOWN_BUILDS = {
    "ae16f71e002af8cb059a9a74c4d90f34ba984892": "Link's Awakening v1.0.0  <- the version we target",
    "909e904af78ac1b8deefe97ab2ccdb51968f0ec7": "Link's Awakening v1.0.1  (update applied! we target v1.0.0)",
}


def identify(build_id):
    return next((v for k, v in KNOWN_BUILDS.items() if build_id.hex().startswith(k)), None)


def write_report(path, img, fs, stats, seconds):
    lines = ["LAARecomp recompiler report", "=" * 40, ""]
    lines.append("Modules:")
    for m in img.modules:
        bid = m.build_id.rstrip(bytes(1)).hex()
        lines.append(f"  {m.name:<10} base 0x{m.base:x}  size 0x{m.size:x}  "
                     f"text 0x{m.text[1] - m.text[0]:x}  build id {bid}")
        known = identify(m.build_id)
        if known:
            lines.append(f"             = {known}")
    lines.append("")
    counts = fs.counts()
    lines.append(f"Functions found: {len(fs.funcs)}")
    for src, n in counts.items():
        lines.append(f"  from {src:<9} {n}")
    lines.append("")
    total_bad = sum(stats.unsupported.values())
    pct = 100.0 * (stats.instructions - total_bad) / max(stats.instructions, 1)
    lines.append(f"Instructions: {stats.instructions}  translated: {stats.instructions - total_bad} ({pct:.2f}%)")
    lines.append(f"Functions containing an unsupported instruction: {stats.unsupported_funcs} of {len(fs.funcs)}")
    lines.append(f"Jumps into the middle of another function (need handling later): {stats.bad_jumps}")
    lines.append("")
    lines.append("Unsupported instructions by category:")
    for why, n in stats.unsupported_why.most_common():
        lines.append(f"  {n:>9}  {why}")
    lines.append("")
    lines.append("Unsupported instructions by mnemonic:")
    for mnem, n in stats.unsupported.most_common():
        lines.append(f"  {n:>9}  {mnem}")
    if img.notes:
        lines += ["", "Loader notes:"] + [f"  - {n}" for n in img.notes]
    lines += ["", f"Took {seconds:.1f} s"]
    text = "\n".join(lines) + "\n"
    if path:
        os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
        with open(path, "w") as f:
            f.write(text)
    return text


def main():
    ap = argparse.ArgumentParser(description="Recompile ARM64 code (ELF, NSO or ExeFS folder) into C")
    ap.add_argument("input")
    ap.add_argument("-o", "--output", help="output .c file, or a folder for chunked output")
    ap.add_argument("--report", help="write a coverage report here")
    ap.add_argument("--embed-image", action="store_true", help="embed memory image + recomp_init (tests)")
    ap.add_argument("--symbols", help="write a list of function name -> address here")
    ap.add_argument("--no-comments", action="store_true", help="don't put disassembly comments in the C")
    ap.add_argument("--base", type=lambda s: int(s, 0), default=0x8000000, help="load address for NSO modules")
    args = ap.parse_args()

    t0 = time.time()
    img = load(args.input, args.base)
    fs = discover(img)
    funcs = fs.funcs
    stats = Stats()
    comments = not args.no_comments

    single = args.output and args.output.endswith(".c")
    if args.output and not single:
        os.makedirs(args.output, exist_ok=True)
        with open(os.path.join(args.output, "funcs.h"), "w") as f:
            f.write('#include "recomp_runtime.h"\n')
            f.writelines(f"void {cname(a)}(CPU *c);\n" for a in funcs)

    def chunk_file(n):
        return os.path.join(args.output, f"funcs_{n:04d}.c")

    out_f, items = None, list(funcs.items())
    if single:
        out_f = open(args.output, "w")
        out_f.write(f"/* Generated by recomp.py from {os.path.basename(args.input)} - do not edit */\n")
        out_f.write('#include "recomp_runtime.h"\n\n')
        out_f.writelines(f"void {cname(a)}(CPU *c);\n" for a in funcs)
    for k, (addr, (name, size)) in enumerate(items):
        if args.output and not single and k % FUNCS_PER_CHUNK == 0:
            if out_f:
                out_f.close()
            out_f = open(chunk_file(k // FUNCS_PER_CHUNK), "w")
            out_f.write('/* Generated by recomp.py - do not edit */\n#include "funcs.h"\n\n')
        code = recompile_function(img, funcs, addr, name, size, stats, comments)
        if out_f:
            out_f.write("\n".join(code) + "\n\n")
        if len(items) > 20000 and k % 20000 == 0 and k:
            print(f"  ... {k}/{len(items)} functions", file=sys.stderr)

    if args.output:
        if not single:
            out_f.close()
            out_f = open(os.path.join(args.output, "functable.c"), "w")
            out_f.write('#include "funcs.h"\n')
        out_f.write("const RecompFuncEntry recomp_func_table[] = {\n")
        out_f.writelines(f"    {{ {hx(a)}, {cname(a)} }},  /* {n} */\n" for a, (n, _) in funcs.items())
        out_f.write("};\n")
        out_f.write(f"const uint64_t recomp_func_count = {len(funcs)};\n")
        if args.embed_image:
            out_f.write("\n".join(emit_image(img)) + "\n")
        out_f.close()

    if args.symbols:
        with open(args.symbols, "w") as f:
            for a, (n, _) in funcs.items():
                f.write(f"{n} {a:#x}\n")

    report = write_report(args.report, img, fs, stats, time.time() - t0)
    total_bad = sum(stats.unsupported.values())
    if args.report:
        print(f"Report written to {args.report}")
        print("\n".join(report.splitlines()[2:14]))
    else:
        print(f"{args.input}: {len(funcs)} functions, {stats.instructions - total_bad} instructions translated, "
              f"{total_bad} unsupported")
        for mnem, count in stats.unsupported.most_common(20):
            print(f"    unsupported: {mnem} x{count}")
    return 1 if total_bad else 0


if __name__ == "__main__":
    sys.exit(main())
